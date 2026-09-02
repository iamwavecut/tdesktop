/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mcp/mcp_http_server.h"

#include "core/mcp/mcp_dispatcher.h"
#include "core/mcp/mcp_http_parser.h"
#include "core/mcp/mcp_protocol.h"

#include <crl/crl_time.h>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

namespace Core::Mcp {
namespace {

constexpr auto kMaxClients = 64;
constexpr auto kMaxActiveCalls = 32;
constexpr auto kMaxSubscriptions = 16;
constexpr auto kMaxResponseSize = 16 * 1024 * 1024;
constexpr auto kMaxResourceSubscriptions = 256;
constexpr auto kMaxIncomingSize = kMaxHttpHeaderSize + 4 + kMaxHttpBodySize;
constexpr auto kReadDeadline = 15 * crl::time(1000);
constexpr auto kCallDeadline = 5 * 60 * crl::time(1000);

[[nodiscard]] QByteArray StatusText(int status) {
	switch (status) {
	case 200: return "200 OK";
	case 202: return "202 Accepted";
	case 401: return "401 Unauthorized";
	case 400: return "400 Bad Request";
	case 403: return "403 Forbidden";
	case 404: return "404 Not Found";
	case 405: return "405 Method Not Allowed";
	case 406: return "406 Not Acceptable";
	case 408: return "408 Request Timeout";
	case 413: return "413 Content Too Large";
	case 415: return "415 Unsupported Media Type";
	case 431: return "431 Request Header Fields Too Large";
	case 503: return "503 Service Unavailable";
	}
	return "500 Internal Server Error";
}

[[nodiscard]] bool SecureEqual(
		const QByteArray &a,
		const QByteArray &b) {
	if (a.size() != b.size()) {
		return false;
	}
	auto difference = uchar(0);
	for (auto i = 0; i != a.size(); ++i) {
		difference |= uchar(a[i]) ^ uchar(b[i]);
	}
	return difference == 0;
}

[[nodiscard]] QByteArray ErrorBody(
		const QJsonValue &id,
		int code,
		const QString &message,
		QJsonObject data = {}) {
	auto error = QJsonObject{
		{ u"code"_q, code },
		{ u"message"_q, message },
	};
	if (!data.isEmpty()) {
		error.insert(u"data"_q, std::move(data));
	}
	return QJsonDocument(QJsonObject{
		{ u"jsonrpc"_q, u"2.0"_q },
		{ u"id"_q, id },
		{ u"error"_q, std::move(error) },
	}).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QByteArray ErrorBody(const ProtocolError &error) {
	return ErrorBody(error.id, error.code, error.message);
}

[[nodiscard]] QString ExpectedName(const Request &request) {
	if (request.method == u"tools/call"_q
		|| request.method == u"prompts/get"_q) {
		return request.params.value(u"name"_q).toString();
	} else if (request.method == u"resources/read"_q) {
		return request.params.value(u"uri"_q).toString();
	} else if (request.method == u"tasks/get"_q
		|| request.method == u"tasks/update"_q
		|| request.method == u"tasks/cancel"_q) {
		return request.params.value(u"taskId"_q).toString();
	}
	return QString();
}

[[nodiscard]] bool IsRequestWithName(const QString &method) {
	return method == u"tools/call"_q
		|| method == u"prompts/get"_q
		|| method == u"resources/read"_q
		|| method == u"tasks/get"_q
		|| method == u"tasks/update"_q
		|| method == u"tasks/cancel"_q;
}

[[nodiscard]] QJsonObject ProgressNotification(
		const QJsonValue &token,
		double progress) {
	return {
		{ u"jsonrpc"_q, u"2.0"_q },
		{ u"method"_q, u"notifications/progress"_q },
		{ u"params"_q, QJsonObject{
			{ u"progressToken"_q, token },
			{ u"progress"_q, progress },
			{ u"total"_q, 1. },
		} },
	};
}

} // namespace

class HttpServer::Worker final : public QObject {
public:
	explicit Worker(not_null<HttpServer*> owner)
	: _owner(owner) {
	}

	[[nodiscard]] bool listen(quint16 port) {
		auto candidate = std::make_unique<QTcpServer>();
		if (!candidate->listen(QHostAddress::LocalHost, port)) {
			_errorString = candidate->errorString();
			return false;
		}
		QObject::connect(
			candidate.get(),
			&QTcpServer::newConnection,
			this,
			[=] { acceptClients(); });
		if (_server) {
			_server->close();
		}
		_server = std::move(candidate);
		_errorString.clear();
		return true;
	}

	[[nodiscard]] quint16 port() const {
		return _server ? _server->serverPort() : 0;
	}

	[[nodiscard]] QString errorString() const {
		return _errorString;
	}

	void stop() {
		_server.reset();
		closeClients();
	}

	void setBearerToken(QByteArray token) {
		_bearerToken = std::move(token);
		closeClients();
	}

	void complete(quint64 serial, QJsonObject response) {
		for (auto i = _clients.begin(); i != _clients.end(); ++i) {
			if (i->serial != serial) {
				continue;
			}
			const auto socket = i.key();
			if (i->counted) {
				--_activeCalls;
				i->counted = false;
			}
			if (i->callTimer) {
				i->callTimer->stop();
			}
			i->serial = 0;
			if (i->stream) {
				if (!i->progressToken.isUndefined()) {
					writeSse(socket, ProgressNotification(i->progressToken, 1.));
				}
				auto body = QJsonDocument(response).toJson(QJsonDocument::Compact);
				if (body.size() > kMaxResponseSize) {
					body = ErrorBody(
						response.value(u"id"_q),
						-32603,
						u"Response too large"_q);
				}
				writeSseBytes(socket, body);
				socket->disconnectFromHost();
			} else {
				const auto body = QJsonDocument(response).toJson(
					QJsonDocument::Compact);
				const auto code = response.value(u"error"_q).toObject().value(
					u"code"_q).toInt();
				const auto status = (code == -32601)
					? 404
					: (code == -32021)
					? 400
					: 200;
				if (body.size() > kMaxResponseSize) {
					writeHttp(
						socket,
						413,
						"text/plain; charset=utf-8",
						"Response too large");
				} else {
					writeHttp(socket, status, "application/json", body);
				}
			}
			return;
		}
	}

	void publish(QString method, QJsonObject params) {
		for (auto i = _clients.begin(); i != _clients.end(); ++i) {
			if (!i->subscription || !accepts(*i, method, params)) {
				continue;
			}
			auto messageParams = params;
			auto meta = messageParams.value(u"_meta"_q).toObject();
			meta.insert(
				u"io.modelcontextprotocol/subscriptionId"_q,
				i->subscriptionId);
			messageParams.insert(u"_meta"_q, std::move(meta));
			writeSse(i.key(), {
				{ u"jsonrpc"_q, u"2.0"_q },
				{ u"method"_q, method },
				{ u"params"_q, std::move(messageParams) },
			});
		}
	}

	void shutdown() {
		stop();
	}

private:
	void closeClients() {
		const auto sockets = _clients.keys();
		for (const auto socket : sockets) {
			const auto i = _clients.find(socket);
			if (i != _clients.end() && i->subscription) {
				writeSse(socket, {
					{ u"jsonrpc"_q, u"2.0"_q },
					{ u"id"_q, i->subscriptionId },
					{ u"result"_q, QJsonObject{
						{ u"resultType"_q, u"complete"_q },
						{ u"_meta"_q, QJsonObject{
							{ u"io.modelcontextprotocol/subscriptionId"_q,
								i->subscriptionId },
						} },
					} },
				});
				socket->flush();
				if (socket->bytesToWrite() > 0) {
					socket->waitForBytesWritten(50);
				}
			}
			QObject::disconnect(socket, nullptr, this, nullptr);
			socket->abort();
			delete socket;
		}
		_clients.clear();
		_activeCalls = 0;
		_activeSubscriptions = 0;
	}

	struct Client {
		QByteArray input;
		quint64 serial = 0;
		QJsonValue progressToken = QJsonValue(QJsonValue::Undefined);
		QJsonValue subscriptionId = QJsonValue(QJsonValue::Undefined);
		QJsonObject notifications;
		bool reading = true;
		bool stream = false;
		bool subscription = false;
		bool counted = false;
		quint64 eventId = 0;
		QPointer<QTimer> readTimer;
		QPointer<QTimer> callTimer;
	};

	void acceptClients() {
		while (_server && _server->hasPendingConnections()) {
			const auto socket = _server->nextPendingConnection();
			if (!socket->peerAddress().isLoopback()
				|| _clients.size() >= kMaxClients) {
				socket->abort();
				socket->deleteLater();
				continue;
			}
			socket->setParent(this);
			socket->setReadBufferSize(kMaxIncomingSize + 1);
			const auto timer = new QTimer(socket);
			timer->setSingleShot(true);
			auto client = Client();
			client.readTimer = timer;
			_clients.insert(socket, std::move(client));
			QObject::connect(socket, &QTcpSocket::readyRead, this, [=] {
				readClient(socket);
			});
			QObject::connect(socket, &QTcpSocket::disconnected, this, [=] {
				removeClient(socket);
			});
			QObject::connect(timer, &QTimer::timeout, this, [=] {
				const auto i = _clients.find(socket);
				if (i != _clients.end() && i->reading) {
					i->reading = false;
					writeHttp(
						socket,
						408,
						"text/plain; charset=utf-8",
						"Request Timeout");
				}
			});
			timer->start(kReadDeadline);
		}
	}

	void readClient(QTcpSocket *socket) {
		const auto i = _clients.find(socket);
		if (i == _clients.end() || !i->reading) {
			socket->read(kMaxIncomingSize);
			if (socket->bytesAvailable() > 0) {
				socket->abort();
			}
			return;
		}
		const auto remaining = kMaxIncomingSize - i->input.size();
		i->input.append(socket->read(remaining + 1));
		if (i->input.size() > kMaxIncomingSize) {
			i->reading = false;
			if (i->readTimer) {
				i->readTimer->stop();
			}
			writeHttp(
				socket,
				413,
				"text/plain; charset=utf-8",
				"Content Too Large");
			return;
		}
		const auto parsed = ParseHttpRequest(i->input, socket->localPort());
		if (parsed.state == HttpParseState::NeedMore) {
			return;
		} else if (parsed.state == HttpParseState::Error) {
			i->reading = false;
			if (i->readTimer) {
				i->readTimer->stop();
			}
			writeHttp(
				socket,
				parsed.status,
				"text/plain; charset=utf-8",
				parsed.message);
			return;
		} else if (parsed.consumed != i->input.size()) {
			i->reading = false;
			if (i->readTimer) {
				i->readTimer->stop();
			}
			writeHttp(
				socket,
				400,
				"text/plain; charset=utf-8",
				"Bad Request");
			return;
		}
		i->reading = false;
		if (i->readTimer) {
			i->readTimer->stop();
		}
		if (!_bearerToken.isEmpty()) {
			const auto authorization = parsed.request.headers.value(
				"authorization");
			const auto separator = authorization.indexOf(' ');
			const auto scheme = authorization.left(separator);
			const auto encoded = (separator > 0)
				? authorization.mid(separator + 1).trimmed()
				: QByteArray();
			const auto decoded = QByteArray::fromBase64(
				encoded,
				QByteArray::Base64UrlEncoding
					| QByteArray::AbortOnBase64DecodingErrors);
			if (scheme.compare("Bearer", Qt::CaseInsensitive) != 0
				|| !SecureEqual(decoded, _bearerToken)) {
				auto challenge = QByteArray(
					"WWW-Authenticate: Bearer realm=\"Forkgram MCP\"");
				if (!authorization.isEmpty()) {
					challenge += ", error=\"invalid_token\"";
				}
				writeHttp(
					socket,
					401,
					"text/plain; charset=utf-8",
					"Unauthorized",
					challenge + "\r\n");
				return;
			}
		}
		const auto headerVersion = parsed.request.headers.value(
			"mcp-protocol-version");
		const auto parsedRequest = ParseRequest(
			parsed.request.body,
			headerVersion);
		if (const auto error = std::get_if<ProtocolError>(&parsedRequest)) {
			writeHttp(socket, 400, "application/json", ErrorBody(*error));
			return;
		}
		const auto request = std::get<Request>(parsedRequest);
		if (request.dialect == ProtocolDialect::Compatibility) {
			const auto requestedVersion = (request.method == u"initialize"_q)
				? request.params.value(u"protocolVersion"_q).toString().toUtf8()
				: headerVersion;
			if (requestedVersion != kCompatibilityProtocolVersion) {
				writeHttp(
					socket,
					400,
					"application/json",
					ErrorBody(
						request.id,
						-32022,
						u"Unsupported protocol version"_q,
						{
							{ u"requested"_q,
								QString::fromUtf8(requestedVersion) },
							{ u"supported"_q, QJsonArray{
								QString::fromLatin1(
									kCompatibilityProtocolVersion),
							} },
						}));
				return;
			} else if (request.id.isUndefined()) {
				writeHttp(socket, 202, "application/json", {});
				return;
			}
		} else {
			const auto metadataVersion = request.params.value(
				u"_meta"_q).toObject().value(
				u"io.modelcontextprotocol/protocolVersion"_q).toString().toUtf8();
			const auto method = parsed.request.headers.value("mcp-method");
			const auto expectedName = ExpectedName(request);
			const auto actualName = parsed.request.headers.value("mcp-name");
			if (headerVersion != metadataVersion
				|| method != request.method.toUtf8()
				|| (IsRequestWithName(request.method)
					&& (expectedName.isEmpty()
						|| actualName != expectedName.toUtf8()))
				|| (!IsRequestWithName(request.method)
					&& !actualName.isEmpty())) {
				writeHttp(
					socket,
					400,
					"application/json",
					ErrorBody(
						request.id,
						-32020,
						u"Header mismatch"_q));
				return;
			}
		}
		if (request.dialect == ProtocolDialect::Current
			&& headerVersion != kProtocolVersion) {
			writeHttp(
				socket,
				400,
				"application/json",
				ErrorBody(
					request.id,
					-32022,
					u"Unsupported protocol version"_q,
					{
						{ u"requested"_q,
							QString::fromUtf8(headerVersion) },
						{ u"supported"_q, QJsonArray{
							QString::fromLatin1(kProtocolVersion),
						} },
					}));
			return;
		}
		if (request.method == u"subscriptions/listen"_q) {
			startSubscription(socket, request);
			return;
		} else if (_activeCalls >= kMaxActiveCalls) {
			writeHttp(
				socket,
				503,
				"text/plain; charset=utf-8",
				"Busy");
			return;
		}
		++_activeCalls;
		i->counted = true;
		const auto callTimer = new QTimer(socket);
		callTimer->setSingleShot(true);
		i->callTimer = callTimer;
		QObject::connect(callTimer, &QTimer::timeout, this, [=] {
			if (_clients.contains(socket)) {
				socket->abort();
			}
		});
		callTimer->start(kCallDeadline);
		i->serial = ++_nextSerial;
		i->progressToken = request.params.value(u"_meta"_q).toObject().value(
			u"progressToken"_q);
		i->stream = (request.method == u"tools/call"_q)
			&& (i->progressToken.isString()
				|| i->progressToken.isDouble());
		if (i->stream) {
			beginSse(socket);
			writeSse(socket, ProgressNotification(i->progressToken, 0.));
			writeSse(socket, ProgressNotification(i->progressToken, 0.5));
		}
		const auto owner = QPointer<HttpServer>(_owner);
		const auto serial = i->serial;
		QMetaObject::invokeMethod(_owner, [owner, serial, request] {
			if (owner) {
				owner->dispatch(serial, request);
			}
		});
	}

	void startSubscription(QTcpSocket *socket, const Request &request) {
		const auto i = _clients.find(socket);
		if (_activeSubscriptions >= kMaxSubscriptions) {
			writeHttp(
				socket,
				503,
				"text/plain; charset=utf-8",
				"Too many subscriptions");
			return;
		}
		const auto value = request.params.value(u"notifications"_q);
		if (!value.isObject()) {
			writeHttp(
				socket,
				400,
				"application/json",
				ErrorBody(
					request.id,
					-32602,
					u"Invalid subscription filter"_q));
			return;
		}
		const auto requested = value.toObject();
		auto honored = QJsonObject();
		for (const auto &key : {
			u"toolsListChanged"_q,
			u"resourcesListChanged"_q,
		}) {
			const auto flag = requested.value(key);
			if (!flag.isUndefined() && !flag.isBool()) {
				writeHttp(
					socket,
					400,
					"application/json",
					ErrorBody(
						request.id,
						-32602,
						u"Invalid subscription filter"_q));
				return;
			} else if (flag.isBool()) {
				honored.insert(key, flag);
			}
		}
		const auto resources = requested.value(
			u"resourceSubscriptions"_q);
		if (!resources.isUndefined()) {
			if (!resources.isArray()
				|| resources.toArray().size() > kMaxResourceSubscriptions) {
				writeHttp(
					socket,
					400,
					"application/json",
					ErrorBody(
						request.id,
						-32602,
						u"Invalid resource subscriptions"_q));
				return;
			}
			auto accepted = QJsonArray();
			for (const auto &entry : resources.toArray()) {
				if (!entry.isString() || entry.toString().isEmpty()) {
					writeHttp(
						socket,
						400,
						"application/json",
						ErrorBody(
							request.id,
							-32602,
							u"Invalid resource subscription URI"_q));
					return;
				}
				accepted.append(entry);
			}
			honored.insert(u"resourceSubscriptions"_q, std::move(accepted));
		}
		++_activeSubscriptions;
		i->subscription = true;
		i->subscriptionId = request.id;
		i->notifications = honored;
		beginSse(socket);
		writeSse(socket, {
			{ u"jsonrpc"_q, u"2.0"_q },
			{ u"method"_q,
				u"notifications/subscriptions/acknowledged"_q },
			{ u"params"_q, QJsonObject{
				{ u"_meta"_q, QJsonObject{
					{ u"io.modelcontextprotocol/subscriptionId"_q,
						request.id },
				} },
				{ u"notifications"_q, honored },
			} },
		});
	}

	[[nodiscard]] bool accepts(
			const Client &client,
			const QString &method,
			const QJsonObject &params) const {
		if (method == u"notifications/tools/list_changed"_q) {
			return client.notifications.value(u"toolsListChanged"_q).toBool();
		} else if (method == u"notifications/resources/list_changed"_q) {
			return client.notifications.value(
				u"resourcesListChanged"_q).toBool();
		} else if (method == u"notifications/resources/updated"_q) {
			const auto uri = params.value(u"uri"_q).toString();
			for (const auto &value : client.notifications.value(
					u"resourceSubscriptions"_q).toArray()) {
				const auto subscribed = value.toString();
				if (uri == subscribed
					|| uri.startsWith(subscribed + u"/"_q)
					|| uri.startsWith(subscribed + u"?"_q)) {
					return true;
				}
			}
		}
		return false;
	}

	void removeClient(QTcpSocket *socket) {
		const auto client = _clients.take(socket);
		if (client.counted) {
			--_activeCalls;
		}
		if (client.subscription) {
			--_activeSubscriptions;
		}
		if (client.serial) {
			const auto owner = QPointer<HttpServer>(_owner);
			const auto serial = client.serial;
			QMetaObject::invokeMethod(_owner, [owner, serial] {
				if (owner) {
					owner->cancel(serial);
				}
			});
		}
		socket->deleteLater();
	}

	void beginSse(QTcpSocket *socket) {
		socket->write(
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/event-stream\r\n"
			"Cache-Control: no-cache\r\n"
			"Connection: keep-alive\r\n\r\n");
	}

	void writeSse(QTcpSocket *socket, const QJsonObject &object) {
		writeSseBytes(
			socket,
			QJsonDocument(object).toJson(QJsonDocument::Compact));
	}

	void writeSseBytes(QTcpSocket *socket, const QByteArray &body) {
		const auto i = _clients.find(socket);
		if (i == _clients.end()) {
			return;
		}
		socket->write(
			"id: " + QByteArray::number(++i->eventId) + "\r\n"
			"data: " + body + "\r\n\r\n");
	}

	void writeHttp(
			QTcpSocket *socket,
			int status,
			const QByteArray &contentType,
			const QByteArray &body,
			const QByteArray &extraHeaders = {}) {
		const auto response = QByteArray("HTTP/1.1 ")
			+ StatusText(status) + "\r\n"
			+ "Content-Type: " + contentType + "\r\n"
			+ extraHeaders
			+ "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
			+ "Connection: close\r\n\r\n"
			+ body;
		socket->write(response);
		socket->disconnectFromHost();
	}

	const not_null<HttpServer*> _owner;
	std::unique_ptr<QTcpServer> _server;
	QHash<QTcpSocket*, Client> _clients;
	QByteArray _bearerToken;
	QString _errorString;
	quint64 _nextSerial = 0;
	int _activeCalls = 0;
	int _activeSubscriptions = 0;

};

HttpServer::HttpServer(Dispatcher *dispatcher)
: _dispatcher(dispatcher)
, _thread(std::make_unique<QThread>())
, _worker(new Worker(this)) {
	Expects(_dispatcher != nullptr);
	_worker->moveToThread(_thread.get());
	QObject::connect(
		_thread.get(),
		&QThread::finished,
		_worker,
		&QObject::deleteLater);
	_thread->start();
}

HttpServer::~HttpServer() {
	stop();
	if (_worker && _thread->isRunning()) {
		QMetaObject::invokeMethod(
			_worker,
			[worker = _worker] { worker->shutdown(); },
			Qt::BlockingQueuedConnection);
		_thread->quit();
		_thread->wait();
	}
	_worker = nullptr;
}

bool HttpServer::listen(quint16 port) {
	if (!_worker || !_thread->isRunning()) {
		_errorString = u"MCP worker thread is not running."_q;
		return false;
	}
	auto result = false;
	auto actual = quint16(0);
	auto error = QString();
	QMetaObject::invokeMethod(
		_worker,
		[&] {
			result = _worker->listen(port);
			actual = _worker->port();
			error = _worker->errorString();
		},
		Qt::BlockingQueuedConnection);
	_errorString = std::move(error);
	if (result) {
		_port = actual;
	}
	return result;
}

void HttpServer::stop() {
	const auto calls = _calls;
	_calls.clear();
	for (const auto &cancellation : calls) {
		cancellation->cancel();
	}
	if (_worker && _thread->isRunning()) {
		QMetaObject::invokeMethod(
			_worker,
			[worker = _worker] { worker->stop(); },
			Qt::BlockingQueuedConnection);
	}
	_port = 0;
}

void HttpServer::setBearerToken(QByteArray token) {
	const auto calls = _calls;
	_calls.clear();
	for (const auto &cancellation : calls) {
		cancellation->cancel();
	}
	_bearerToken = token;
	if (_worker && _thread->isRunning()) {
		QMetaObject::invokeMethod(
			_worker,
			[worker = _worker, token = std::move(token)]() mutable {
				worker->setBearerToken(std::move(token));
			},
			Qt::BlockingQueuedConnection);
	}
}

quint16 HttpServer::port() const {
	return _port;
}

QString HttpServer::errorString() const {
	return _errorString;
}

void HttpServer::publish(QString method, QJsonObject params) {
	if (!_worker || !_thread->isRunning()) {
		return;
	}
	const auto worker = QPointer<Worker>(_worker);
	QMetaObject::invokeMethod(_worker, [
		worker,
		method = std::move(method),
		params = std::move(params)
	]() mutable {
		if (worker) {
			worker->publish(std::move(method), std::move(params));
		}
	});
}

void HttpServer::dispatch(quint64 serial, Request request) {
	auto cancellation = std::make_shared<Cancellation>();
	_calls.insert(serial, cancellation);
	const auto weak = QPointer<HttpServer>(this);
	_dispatcher->handle(
		request,
		[weak, serial](QJsonObject response) {
			if (weak) {
				weak->finish(serial, std::move(response));
			}
		},
		std::move(cancellation));
}

void HttpServer::finish(quint64 serial, QJsonObject response) {
	if (!_calls.remove(serial) || !_worker || !_thread->isRunning()) {
		return;
	}
	const auto worker = QPointer<Worker>(_worker);
	QMetaObject::invokeMethod(_worker, [
		worker,
		serial,
		response = std::move(response)
	]() mutable {
		if (worker) {
			worker->complete(serial, std::move(response));
		}
	});
}

void HttpServer::cancel(quint64 serial) {
	const auto cancellation = _calls.take(serial);
	if (cancellation) {
		cancellation->cancel();
	}
}

} // namespace Core::Mcp
