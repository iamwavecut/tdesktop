/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mcp/mcp_protocol.h"
#include "core/mcp/mcp_http_parser.h"
#include "core/mcp/mcp_dispatcher.h"
#include "core/mcp/mcp_http_server.h"
#include "core/mcp/mcp_tl_registry.h"
#include "core/mcp/mcp_tl_codec.h"

#include "mcp_schema.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QEventLoop>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <variant>

namespace {

using Core::Mcp::ProtocolError;
using Core::Mcp::Request;

[[noreturn]] void Fail(const QString &message) {
	qCritical().noquote() << message;
	std::exit(1);
}

void Require(bool condition, const QString &message) {
	if (!condition) {
		Fail(message);
	}
}

[[nodiscard]] QJsonObject ValidMeta() {
	return {
		{ u"io.modelcontextprotocol/protocolVersion"_q,
			u"2026-07-28"_q },
		{ u"io.modelcontextprotocol/clientInfo"_q, QJsonObject{
			{ u"name"_q, u"test-client"_q },
			{ u"version"_q, u"1.0"_q },
		} },
		{ u"io.modelcontextprotocol/clientCapabilities"_q,
			QJsonObject() },
	};
}

void TestValidRequestParses() {
	const auto bytes = QJsonDocument(QJsonObject{
		{ u"jsonrpc"_q, u"2.0"_q },
		{ u"id"_q, u"request-1"_q },
		{ u"method"_q, u"server/discover"_q },
		{ u"params"_q, QJsonObject{
			{ u"_meta"_q, ValidMeta() },
		} },
	}).toJson(QJsonDocument::Compact);
	const auto parsed = Core::Mcp::ParseRequest(bytes);
	Require(std::holds_alternative<Request>(parsed),
		u"valid MCP request was rejected"_q);
	const auto &request = std::get<Request>(parsed);
	Require(request.id == QJsonValue(u"request-1"_q),
		u"request id was not preserved"_q);
	Require(request.method == u"server/discover"_q,
		u"request method was not preserved"_q);
}

void TestMissingMetadataIsRejected() {
	const auto bytes = QJsonDocument(QJsonObject{
		{ u"jsonrpc"_q, u"2.0"_q },
		{ u"id"_q, 7 },
		{ u"method"_q, u"tools/list"_q },
		{ u"params"_q, QJsonObject() },
	}).toJson(QJsonDocument::Compact);
	const auto parsed = Core::Mcp::ParseRequest(bytes);
	Require(std::holds_alternative<ProtocolError>(parsed),
		u"request without MCP metadata was accepted"_q);
	const auto &error = std::get<ProtocolError>(parsed);
	Require(error.code == -32602,
		u"missing metadata returned the wrong JSON-RPC code"_q);
	Require(error.id == QJsonValue(7),
		u"metadata error lost the request id"_q);
}

void TestClientInfoMetadataIsOptional() {
	auto meta = ValidMeta();
	meta.remove(u"io.modelcontextprotocol/clientInfo"_q);
	const auto bytes = QJsonDocument(QJsonObject{
		{ u"jsonrpc"_q, u"2.0"_q },
		{ u"id"_q, u"request-without-client-info"_q },
		{ u"method"_q, u"server/discover"_q },
		{ u"params"_q, QJsonObject{ { u"_meta"_q, meta } } },
	}).toJson(QJsonDocument::Compact);
	Require(std::holds_alternative<Request>(Core::Mcp::ParseRequest(bytes)),
		u"request without optional clientInfo was rejected"_q);
}

void TestDiscoverResultAdvertisesCurrentProtocol() {
	const auto result = Core::Mcp::DiscoverResult(
		u"Forkgram"_q,
		u"7.1.3"_q);
	Require(result.value(u"resultType"_q) == u"complete"_q,
		u"discover result is not complete"_q);
	Require(result.value(u"supportedVersions"_q).toArray()
		== QJsonArray{ u"2026-07-28"_q },
		u"discover result advertised the wrong protocol"_q);
	const auto capabilities = result.value(u"capabilities"_q).toObject();
	Require(capabilities.contains(u"tools"_q),
		u"discover result omitted tools capability"_q);
	Require(capabilities.contains(u"resources"_q),
		u"discover result omitted resources capability"_q);
	const auto meta = result.value(u"_meta"_q).toObject();
	const auto info = meta.value(
		u"io.modelcontextprotocol/serverInfo"_q).toObject();
	Require(info.value(u"name"_q) == u"Forkgram"_q,
		u"discover result omitted server name"_q);
	Require(info.value(u"version"_q) == u"7.1.3"_q,
		u"discover result omitted server version"_q);
}

void TestHttpParserWaitsForCompleteBody() {
	const auto body = QByteArray(R"({"jsonrpc":"2.0"})");
	const auto head = QByteArray("POST /mcp HTTP/1.1\r\n")
		+ "Host: 127.0.0.1:45678\r\n"
		+ "Content-Type: application/json\r\n"
		+ "Accept: application/json, text/event-stream\r\n"
		+ "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
	const auto partial = Core::Mcp::ParseHttpRequest(head, 45678);
	Require(partial.state == Core::Mcp::HttpParseState::NeedMore,
		u"HTTP parser accepted an incomplete body"_q);
	const auto complete = Core::Mcp::ParseHttpRequest(head + body, 45678);
	Require(complete.state == Core::Mcp::HttpParseState::Complete,
		u"HTTP parser rejected a complete MCP request"_q);
	Require(complete.request.method == QByteArray("POST"),
		u"HTTP parser lost the request method"_q);
	Require(complete.request.path == QByteArray("/mcp"),
		u"HTTP parser lost the request path"_q);
	Require(complete.request.body == body,
		u"HTTP parser changed the request body"_q);
	Require(complete.consumed == head.size() + body.size(),
		u"HTTP parser returned the wrong consumed length"_q);
}

void TestHttpParserRejectsUnsupportedMethods() {
	const auto request = QByteArray(
		"GET /mcp HTTP/1.1\r\n"
		"Host: 127.0.0.1:45678\r\n"
		"Content-Length: 0\r\n\r\n");
	const auto parsed = Core::Mcp::ParseHttpRequest(request, 45678);
	Require(parsed.state == Core::Mcp::HttpParseState::Error,
		u"HTTP parser accepted GET"_q);
	Require(parsed.status == 405,
		u"HTTP parser returned the wrong status for GET"_q);
}

void TestHttpParserRejectsRemoteOrigin() {
	const auto request = QByteArray(
		"POST /mcp HTTP/1.1\r\n"
		"Host: 127.0.0.1:45678\r\n"
		"Origin: https://example.com\r\n"
		"Content-Type: application/json\r\n"
		"Accept: application/json, text/event-stream\r\n"
		"Content-Length: 2\r\n\r\n{}");
	const auto parsed = Core::Mcp::ParseHttpRequest(request, 45678);
	Require(parsed.state == Core::Mcp::HttpParseState::Error,
		u"HTTP parser accepted a remote Origin"_q);
	Require(parsed.status == 403,
		u"HTTP parser returned the wrong status for remote Origin"_q);
}

void TestHttpParserRequiresJsonAndEventStreamAccept() {
	const auto request = QByteArray(
		"POST /mcp HTTP/1.1\r\n"
		"Host: 127.0.0.1:45678\r\n"
		"Content-Type: text/plain\r\n"
		"Accept: application/json, text/event-stream\r\n"
		"Content-Length: 2\r\n\r\n{}");
	const auto parsed = Core::Mcp::ParseHttpRequest(request, 45678);
	Require(parsed.state == Core::Mcp::HttpParseState::Error,
		u"HTTP parser accepted a non-JSON request"_q);
	Require(parsed.status == 415,
		u"HTTP parser returned the wrong status for non-JSON content"_q);
	const auto jsonp = QByteArray(
		"POST /mcp HTTP/1.1\r\n"
		"Host: 127.0.0.1:45678\r\n"
		"Content-Type: application/jsonp\r\n"
		"Accept: application/json, text/event-stream\r\n"
		"Content-Length: 2\r\n\r\n{}"
	);
	const auto parsedJsonp = Core::Mcp::ParseHttpRequest(jsonp, 45678);
	Require(parsedJsonp.state == Core::Mcp::HttpParseState::Error
		&& parsedJsonp.status == 415,
		u"HTTP parser accepted application/jsonp"_q);
}

void TestHttpParserRequiresBothResponseTypes() {
	const auto request = QByteArray(
		"POST /mcp HTTP/1.1\r\n"
		"Host: 127.0.0.1:45678\r\n"
		"Content-Type: application/json\r\n"
		"Accept: application/json\r\n"
		"Content-Length: 2\r\n\r\n{}");
	const auto parsed = Core::Mcp::ParseHttpRequest(request, 45678);
	Require(parsed.state == Core::Mcp::HttpParseState::Error,
		u"HTTP parser accepted an incomplete Accept header"_q);
	Require(parsed.status == 406,
		u"HTTP parser returned the wrong status for Accept"_q);
}

void TestHttpParserRejectsTransferEncoding() {
	const auto request = QByteArray(
		"POST /mcp HTTP/1.1\r\n"
		"Host: 127.0.0.1:45678\r\n"
		"Content-Type: application/json\r\n"
		"Accept: application/json, text/event-stream\r\n"
		"Transfer-Encoding: chunked\r\n"
		"Content-Length: 2\r\n\r\n{}"
	);
	const auto parsed = Core::Mcp::ParseHttpRequest(request, 45678);
	Require(parsed.state == Core::Mcp::HttpParseState::Error,
		u"HTTP parser accepted Transfer-Encoding"_q);
	Require(parsed.status == 400,
		u"HTTP parser returned the wrong status for Transfer-Encoding"_q);
}

void TestHttpParserEnforcesSizeLimits() {
	const auto hugeHeader = Core::Mcp::ParseHttpRequest(
		QByteArray(Core::Mcp::kMaxHttpHeaderSize + 1, 'x'),
		45678);
	Require(hugeHeader.state == Core::Mcp::HttpParseState::Error
		&& hugeHeader.status == 431,
		u"HTTP parser did not enforce the header limit"_q);
	const auto hugeBody = QByteArray(
		"POST /mcp HTTP/1.1\r\n"
		"Host: 127.0.0.1:45678\r\n"
		"Content-Type: application/json\r\n"
		"Accept: application/json, text/event-stream\r\n"
		"Content-Length: 4194305\r\n\r\n");
	const auto parsedBody = Core::Mcp::ParseHttpRequest(hugeBody, 45678);
	Require(parsedBody.state == Core::Mcp::HttpParseState::Error
		&& parsedBody.status == 413,
		u"HTTP parser did not enforce the body limit"_q);
}

void TestDispatcherListsAndCallsTools() {
	auto dispatcher = Core::Mcp::Dispatcher(u"Forkgram"_q, u"7.1.3"_q);
	const auto schema = QJsonObject{
		{ u"type"_q, u"object"_q },
		{ u"additionalProperties"_q, false },
	};
	dispatcher.addTool({
		.name = u"telegram.zeta"_q,
		.description = u"Second tool"_q,
		.inputSchema = schema,
		.outputSchema = schema,
		.handler = [](const QJsonObject &, auto done) {
			done({
				.structuredContent = QJsonObject{
					{ u"called"_q, true },
				},
				.content = QJsonArray{
					QJsonObject{
						{ u"type"_q, u"image"_q },
						{ u"data"_q, u"AA=="_q },
						{ u"mimeType"_q, u"image/png"_q },
					},
				},
			});
		},
	});
	dispatcher.addTool({
		.name = u"telegram.alpha"_q,
		.description = u"First tool"_q,
		.inputSchema = schema,
		.outputSchema = schema,
		.handler = [](const QJsonObject &, auto done) { done({}); },
	});
	auto listed = QJsonObject();
	dispatcher.handle(Request{
		.id = 1,
		.method = u"tools/list"_q,
		.params = { { u"_meta"_q, ValidMeta() } },
	}, [&](QJsonObject response) {
		listed = std::move(response);
	});
	const auto tools = listed.value(u"result"_q).toObject()
		.value(u"tools"_q).toArray();
	Require(tools.size() == 2, u"tools/list returned the wrong count"_q);
	Require(tools[0].toObject().value(u"name"_q) == u"telegram.alpha"_q,
		u"tools/list is not deterministic"_q);
	Require(tools[1].toObject().value(u"name"_q) == u"telegram.zeta"_q,
		u"tools/list returned the wrong second tool"_q);
	auto called = QJsonObject();
	dispatcher.handle(Request{
		.id = u"call-1"_q,
		.method = u"tools/call"_q,
		.params = {
			{ u"name"_q, u"telegram.zeta"_q },
			{ u"arguments"_q, QJsonObject() },
			{ u"_meta"_q, ValidMeta() },
		},
	}, [&](QJsonObject response) {
		called = std::move(response);
	});
	const auto result = called.value(u"result"_q).toObject();
	Require(result.value(u"resultType"_q) == u"complete"_q,
		u"tools/call did not complete"_q);
	Require(result.value(u"structuredContent"_q).toObject()
		.value(u"called"_q).toBool(),
		u"tools/call lost structured content"_q);
	Require(result.value(u"content"_q).toArray()[0].toObject()
		.value(u"type"_q) == u"image"_q,
		u"tools/call replaced explicit image content"_q);
}

void TestDispatcherListsAndReadsResources() {
	auto dispatcher = Core::Mcp::Dispatcher(u"Forkgram"_q, u"7.1.3"_q);
	dispatcher.addResource({
		.uri = u"telegram://state"_q,
		.name = u"state"_q,
		.title = u"Forkgram state"_q,
		.description = u"Current state"_q,
		.mimeType = u"application/json"_q,
		.handler = [](auto done) {
			done({ QJsonObject{
				{ u"uri"_q, u"telegram://state"_q },
				{ u"mimeType"_q, u"application/json"_q },
				{ u"text"_q, u"{\"locked\":false}"_q },
			} });
		},
	});
	dispatcher.addResourceTemplate({
		.uriTemplate = u"telegram://schema/method/{name}"_q,
		.name = u"method-schema"_q,
		.title = u"Method schema"_q,
		.description = u"One method"_q,
		.mimeType = u"application/json"_q,
		.handler = [](const QString &uri, auto done) {
			done({ QJsonObject{
				{ u"uri"_q, uri },
				{ u"mimeType"_q, u"application/json"_q },
				{ u"text"_q, u"{\"method\":true}"_q },
			} });
		},
	});
	auto listed = QJsonObject();
	dispatcher.handle(Request{
		.id = 2,
		.method = u"resources/list"_q,
		.params = { { u"_meta"_q, ValidMeta() } },
	}, [&](QJsonObject response) {
		listed = std::move(response);
	});
	const auto resources = listed.value(u"result"_q).toObject()
		.value(u"resources"_q).toArray();
	Require(resources.size() == 1,
		u"resources/list returned the wrong count"_q);
	Require(resources[0].toObject().value(u"uri"_q)
		== u"telegram://state"_q,
		u"resources/list returned the wrong URI"_q);
	auto read = QJsonObject();
	dispatcher.handle(Request{
		.id = 3,
		.method = u"resources/read"_q,
		.params = {
			{ u"uri"_q, u"telegram://state"_q },
			{ u"_meta"_q, ValidMeta() },
		},
	}, [&](QJsonObject response) {
		read = std::move(response);
	});
	const auto contents = read.value(u"result"_q).toObject()
		.value(u"contents"_q).toArray();
	Require(contents.size() == 1,
		u"resources/read returned no state"_q);
	Require(contents[0].toObject().value(u"text"_q)
		== u"{\"locked\":false}"_q,
		u"resources/read changed the resource content"_q);
	auto templates = QJsonObject();
	dispatcher.handle(Request{
		.id = 4,
		.method = u"resources/templates/list"_q,
		.params = { { u"_meta"_q, ValidMeta() } },
	}, [&](QJsonObject response) {
		templates = std::move(response);
	});
	Require(templates.value(u"result"_q).toObject()
		.value(u"resourceTemplates"_q).toArray().size() == 1,
		u"resources/templates/list lost the method template"_q);
	auto templated = QJsonObject();
	dispatcher.handle(Request{
		.id = 5,
		.method = u"resources/read"_q,
		.params = {
			{ u"uri"_q, u"telegram://schema/method/help.getConfig"_q },
			{ u"_meta"_q, ValidMeta() },
		},
	}, [&](QJsonObject response) {
		templated = std::move(response);
	});
	Require(templated.value(u"result"_q).toObject()
		.value(u"contents"_q).toArray()[0].toObject()
		.value(u"text"_q) == u"{\"method\":true}"_q,
		u"resources/read did not resolve the method template"_q);
	auto missing = QJsonObject();
	dispatcher.addResourceTemplate({
		.uriTemplate = u"telegram://missing/{name}"_q,
		.name = u"missing"_q,
		.title = u"Missing resource"_q,
		.description = u"Always missing"_q,
		.mimeType = u"application/json"_q,
		.handler = [](const QString &, auto done) { done({}); },
	});
	dispatcher.handle(Request{
		.id = 6,
		.method = u"resources/read"_q,
		.params = {
			{ u"uri"_q, u"telegram://missing/value"_q },
			{ u"_meta"_q, ValidMeta() },
		},
	}, [&](QJsonObject response) {
		missing = std::move(response);
	});
	const auto missingError = missing.value(u"error"_q).toObject();
	Require(missingError.value(u"code"_q) == -32602,
		u"missing template resource returned the wrong error"_q);
	Require(missingError.value(u"data"_q).toObject().value(u"uri"_q)
		== u"telegram://missing/value"_q,
		u"missing template resource omitted error.data.uri"_q);
}

void TestDispatcherCancellationReachesTool() {
	auto dispatcher = Core::Mcp::Dispatcher(u"Forkgram"_q, u"7.1.3"_q);
	auto cancelled = false;
	dispatcher.addTool({
		.name = u"telegram.cancellable"_q,
		.description = u"Cancellable tool"_q,
		.inputSchema = QJsonObject{ { u"type"_q, u"object"_q } },
		.outputSchema = QJsonObject{ { u"type"_q, u"object"_q } },
		.cancellableHandler = [&](
				const QJsonObject &,
				const Core::Mcp::CancellationPtr &cancellation,
				auto) {
			cancellation->setHandler([&] { cancelled = true; });
		},
	});
	const auto cancellation = dispatcher.handle(Request{
		.id = u"cancel-me"_q,
		.method = u"tools/call"_q,
		.params = {
			{ u"name"_q, u"telegram.cancellable"_q },
			{ u"arguments"_q, QJsonObject() },
			{ u"_meta"_q, ValidMeta() },
		},
	}, [](QJsonObject) {});
	cancellation->cancel();
	Require(cancelled, u"tool cancellation handler was not called"_q);
}

void TestDispatcherValidatesToolInputSchema() {
	auto dispatcher = Core::Mcp::Dispatcher(u"Forkgram"_q, u"7.1.3"_q);
	auto called = false;
	dispatcher.addTool({
		.name = u"telegram.schema_test"_q,
		.description = u"Schema test"_q,
		.inputSchema = QJsonObject{
			{ u"type"_q, u"object"_q },
			{ u"properties"_q, QJsonObject{
				{ u"count"_q, QJsonObject{
					{ u"type"_q, u"integer"_q },
					{ u"minimum"_q, 1 },
				} },
			} },
			{ u"required"_q, QJsonArray{ u"count"_q } },
			{ u"additionalProperties"_q, false },
		},
		.outputSchema = QJsonObject{ { u"type"_q, u"object"_q } },
		.handler = [&](const QJsonObject &, auto done) {
			called = true;
			done({});
		},
	});
	auto response = QJsonObject();
	dispatcher.handle(Request{
		.id = 9,
		.method = u"tools/call"_q,
		.params = {
			{ u"name"_q, u"telegram.schema_test"_q },
			{ u"arguments"_q, QJsonObject{ { u"count"_q, u"one"_q } } },
			{ u"_meta"_q, ValidMeta() },
		},
	}, [&](QJsonObject value) {
		response = std::move(value);
	});
	Require(!called, u"tool handler received schema-invalid arguments"_q);
	Require(response.value(u"error"_q).toObject().value(u"code"_q) == -32602,
		u"schema-invalid tool arguments returned the wrong error"_q);
}

void TestDispatcherAllowsOmittedEmptyArguments() {
	auto dispatcher = Core::Mcp::Dispatcher(u"Forkgram"_q, u"7.1.3"_q);
	auto called = false;
	dispatcher.addTool({
		.name = u"telegram.no_arguments"_q,
		.description = u"No arguments"_q,
		.inputSchema = QJsonObject{
			{ u"type"_q, u"object"_q },
			{ u"additionalProperties"_q, false },
		},
		.outputSchema = QJsonObject{ { u"type"_q, u"object"_q } },
		.handler = [&](const QJsonObject &arguments, auto done) {
			called = arguments.isEmpty();
			done({});
		},
	});
	dispatcher.handle(Request{
		.id = 10,
		.method = u"tools/call"_q,
		.params = {
			{ u"name"_q, u"telegram.no_arguments"_q },
			{ u"_meta"_q, ValidMeta() },
		},
	}, [](QJsonObject) {});
	Require(called, u"tool call without optional arguments was rejected"_q);
}

[[nodiscard]] QByteArray HttpRequestBytes(
		quint16 port,
		const QByteArray &method,
		const QByteArray &body,
		const QByteArray &name = {},
		const QByteArray &version = "2026-07-28") {
	auto result = QByteArray("POST /mcp HTTP/1.1\r\n")
		+ "Host: 127.0.0.1:" + QByteArray::number(port) + "\r\n"
		+ "Content-Type: application/json\r\n"
		+ "Accept: application/json, text/event-stream\r\n"
		+ "MCP-Protocol-Version: " + version + "\r\n"
		+ "Mcp-Method: " + method + "\r\n";
	if (!name.isNull()) {
		result += "Mcp-Name: " + name + "\r\n";
	}
	return result
		+ "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n"
		+ body;
}

[[nodiscard]] QByteArray CodexHttpRequestBytes(
		quint16 port,
		const QByteArray &body,
		const QByteArray &version = {}) {
	auto result = QByteArray("POST /mcp HTTP/1.1\r\n")
		+ "Host: 127.0.0.1:" + QByteArray::number(port) + "\r\n"
		+ "Content-Type: application/json\r\n"
		+ "Accept: text/event-stream, application/json\r\n";
	if (!version.isEmpty()) {
		result += "MCP-Protocol-Version: " + version + "\r\n";
	}
	return result
		+ "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n"
		+ body;
}

[[nodiscard]] QByteArray SendRawClosedRequest(
		Core::Mcp::HttpServer &server,
		QByteArray request) {
	auto socket = QTcpSocket();
	auto response = QByteArray();
	auto loop = QEventLoop();
	auto timeout = QTimer();
	timeout.setSingleShot(true);
	QObject::connect(&socket, &QTcpSocket::readyRead, [&] {
		response.append(socket.readAll());
	});
	QObject::connect(
		&socket,
		&QTcpSocket::disconnected,
		&loop,
		&QEventLoop::quit);
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
	timeout.start(3000);
	socket.connectToHost(QHostAddress::LocalHost, server.port());
	socket.write(std::move(request));
	loop.exec();
	Require(timeout.isActive(), u"MCP HTTP request timed out"_q);
	return response;
}

[[nodiscard]] QByteArray SendClosedRequest(
		Core::Mcp::HttpServer &server,
		const QByteArray &method,
		const QByteArray &body,
		const QByteArray &name = QByteArray(),
		const QByteArray &version = "2026-07-28") {
	return SendRawClosedRequest(server, HttpRequestBytes(
		server.port(),
		method,
		body,
		name,
		version));
}

void TestHttpServerSupportsCodexClient() {
	auto dispatcher = Core::Mcp::Dispatcher(u"Forkgram"_q, u"7.1.3"_q);
	dispatcher.addTool({
		.name = u"telegram.client.state"_q,
		.description = u"Return current state"_q,
		.inputSchema = QJsonObject{
			{ u"type"_q, u"object"_q },
			{ u"additionalProperties"_q, false },
		},
		.outputSchema = QJsonObject{ { u"type"_q, u"object"_q } },
		.handler = [](const QJsonObject &, auto done) {
			done({ .structuredContent = QJsonObject{
				{ u"authenticated"_q, true },
			} });
		},
	});
	auto server = Core::Mcp::HttpServer(&dispatcher);
	Require(server.listen(0), u"Codex compatibility server did not listen"_q);

	const auto initialize = QByteArray(
		R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{"elicitation":{"form":{},"url":{}}},"clientInfo":{"name":"codex-mcp-client","title":"Codex","version":"0.151.0"}}})");
	const auto initialized = SendRawClosedRequest(
		server,
		CodexHttpRequestBytes(server.port(), initialize));
	Require(initialized.startsWith("HTTP/1.1 200 OK\r\n"),
		u"Codex initialize request failed"_q);
	Require(initialized.contains("\"protocolVersion\":\"2025-06-18\""),
		u"Codex initialize response returned the wrong version"_q);
	Require(initialized.contains("\"name\":\"Forkgram\""),
		u"Codex initialize response omitted server info"_q);

	const auto notification = QByteArray(
		R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
	const auto acknowledged = SendRawClosedRequest(
		server,
		CodexHttpRequestBytes(
			server.port(),
			notification,
			"2025-06-18"));
	Require(acknowledged.startsWith("HTTP/1.1 202 Accepted\r\n"),
		u"Codex initialized notification was not accepted"_q);

	const auto list = QByteArray(
		R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"progressToken":0}}})");
	const auto listed = SendRawClosedRequest(
		server,
		CodexHttpRequestBytes(server.port(), list, "2025-06-18"));
	Require(listed.startsWith("HTTP/1.1 200 OK\r\n"),
		u"Codex tools/list request failed"_q);
	Require(listed.contains("\"name\":\"telegram.client.state\""),
		u"Codex tools/list response omitted the registered tool"_q);

	const auto call = QByteArray(
		R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"_meta":{"callId":"call-1","progressToken":1},"name":"telegram.client.state","arguments":{}}})");
	const auto called = SendRawClosedRequest(
		server,
		CodexHttpRequestBytes(server.port(), call, "2025-06-18"));
	Require(called.startsWith("HTTP/1.1 200 OK\r\n"),
		u"Codex tools/call request failed"_q);
	Require(called.contains("\"authenticated\":true"),
		u"Codex tools/call response lost structured content"_q);
}

void TestHttpServerReportsUnsupportedVersion() {
	auto dispatcher = Core::Mcp::Dispatcher(u"Forkgram"_q, u"7.1.3"_q);
	auto server = Core::Mcp::HttpServer(&dispatcher);
	Require(server.listen(0), u"version test server did not listen"_q);
	auto meta = ValidMeta();
	meta.insert(
		u"io.modelcontextprotocol/protocolVersion"_q,
		u"v999.0.0"_q);
	const auto body = QJsonDocument(QJsonObject{
		{ u"jsonrpc"_q, u"2.0"_q },
		{ u"id"_q, 78 },
		{ u"method"_q, u"server/discover"_q },
		{ u"params"_q, QJsonObject{ { u"_meta"_q, meta } } },
	}).toJson(QJsonDocument::Compact);
	const auto response = SendClosedRequest(
		server,
		"server/discover",
		body,
		QByteArray(),
		"v999.0.0");
	Require(response.startsWith("HTTP/1.1 400 Bad Request\r\n"),
		u"unsupported version did not return HTTP 400"_q);
	Require(response.contains("\"code\":-32022"),
		u"unsupported version returned the wrong JSON-RPC code"_q);
	Require(response.contains("\"requested\":\"v999.0.0\""),
		u"unsupported version error omitted requested version"_q);
	Require(response.contains("\"supported\":[\"2026-07-28\"]"),
		u"unsupported version error omitted supported versions"_q);
}

void TestHttpServerUses404ForUnknownRpcMethod() {
	auto dispatcher = Core::Mcp::Dispatcher(u"Forkgram"_q, u"7.1.3"_q);
	auto server = Core::Mcp::HttpServer(&dispatcher);
	Require(server.listen(0), u"unknown method test server did not listen"_q);
	const auto body = QJsonDocument(QJsonObject{
		{ u"jsonrpc"_q, u"2.0"_q },
		{ u"id"_q, 79 },
		{ u"method"_q, u"ping"_q },
		{ u"params"_q, QJsonObject{ { u"_meta"_q, ValidMeta() } } },
	}).toJson(QJsonDocument::Compact);
	const auto response = SendClosedRequest(server, "ping", body);
	Require(response.startsWith("HTTP/1.1 404 Not Found\r\n"),
		u"unknown RPC method did not return HTTP 404"_q);
	Require(response.contains("\"code\":-32601"),
		u"unknown RPC method returned the wrong JSON-RPC code"_q);
}

void TestHttpServerRebindIsAtomic() {
	auto dispatcher = Core::Mcp::Dispatcher(u"Forkgram"_q, u"7.1.3"_q);
	auto server = Core::Mcp::HttpServer(&dispatcher);
	Require(server.listen(0), u"rebind test server did not listen"_q);
	const auto original = server.port();
	auto blocker = QTcpServer();
	Require(blocker.listen(QHostAddress::LocalHost, 0),
		u"rebind test blocker did not listen"_q);
	Require(!server.listen(blocker.serverPort()),
		u"server rebound to an occupied port"_q);
	Require(server.port() == original,
		u"failed rebind changed the active listener"_q);
	Require(server.listen(0), u"successful live rebind failed"_q);
	Require(server.port() != 0, u"successful rebind lost the listener"_q);
}

void TestHttpServerValidatesNameHeader() {
	auto dispatcher = Core::Mcp::Dispatcher(u"Forkgram"_q, u"7.1.3"_q);
	dispatcher.addTool({
		.name = u"telegram.header_test"_q,
		.description = u"Header test"_q,
		.inputSchema = QJsonObject{ { u"type"_q, u"object"_q } },
		.outputSchema = QJsonObject{ { u"type"_q, u"object"_q } },
		.handler = [](const QJsonObject &, auto done) { done({}); },
	});
	auto server = Core::Mcp::HttpServer(&dispatcher);
	Require(server.listen(0), u"header test server did not listen"_q);
	const auto body = QJsonDocument(QJsonObject{
		{ u"jsonrpc"_q, u"2.0"_q },
		{ u"id"_q, 77 },
		{ u"method"_q, u"tools/call"_q },
		{ u"params"_q, QJsonObject{
			{ u"name"_q, u"telegram.header_test"_q },
			{ u"arguments"_q, QJsonObject() },
			{ u"_meta"_q, ValidMeta() },
		} },
	}).toJson(QJsonDocument::Compact);
	const auto missing = SendClosedRequest(
		server,
		"tools/call",
		body,
		QByteArray());
	Require(missing.startsWith("HTTP/1.1 400 Bad Request\r\n"),
		u"server accepted a missing Mcp-Name header"_q);
	Require(missing.contains("\"code\":-32020"),
		u"name mismatch returned the wrong JSON-RPC code"_q);
	const auto valid = SendClosedRequest(
		server,
		"tools/call",
		body,
		"  telegram.header_test  ");
	Require(valid.startsWith("HTTP/1.1 200 OK\r\n"),
		u"server rejected optional whitespace around Mcp-Name"_q);
}

void TestHttpServerStreamsProgressOnMainThread() {
	auto dispatcher = Core::Mcp::Dispatcher(u"Forkgram"_q, u"7.1.3"_q);
	auto handlerThread = static_cast<QThread*>(nullptr);
	dispatcher.addTool({
		.name = u"telegram.progress_test"_q,
		.description = u"Progress test"_q,
		.inputSchema = QJsonObject{ { u"type"_q, u"object"_q } },
		.outputSchema = QJsonObject{ { u"type"_q, u"object"_q } },
		.handler = [&](const QJsonObject &, auto done) {
			handlerThread = QThread::currentThread();
			done({ .structuredContent = QJsonObject{
				{ u"finished"_q, true },
			} });
		},
	});
	auto server = Core::Mcp::HttpServer(&dispatcher);
	Require(server.listen(0), u"progress test server did not listen"_q);
	auto meta = ValidMeta();
	meta.insert(u"progressToken"_q, u"progress-1"_q);
	const auto body = QJsonDocument(QJsonObject{
		{ u"jsonrpc"_q, u"2.0"_q },
		{ u"id"_q, u"progress-call"_q },
		{ u"method"_q, u"tools/call"_q },
		{ u"params"_q, QJsonObject{
			{ u"name"_q, u"telegram.progress_test"_q },
			{ u"arguments"_q, QJsonObject() },
			{ u"_meta"_q, std::move(meta) },
		} },
	}).toJson(QJsonDocument::Compact);
	const auto response = SendClosedRequest(
		server,
		"tools/call",
		body,
		"telegram.progress_test");
	Require(response.startsWith("HTTP/1.1 200 OK\r\n"),
		u"progress request failed"_q);
	Require(response.contains("Content-Type: text/event-stream"),
		u"progress request did not use SSE"_q);
	Require(response.count("notifications/progress") == 3,
		u"progress request did not send start, midpoint and finish notifications"_q);
	Require(response.contains("\"id\":\"progress-call\""),
		u"progress stream omitted the final response"_q);
	Require(handlerThread == QCoreApplication::instance()->thread(),
		u"tool handler did not run on the main thread"_q);
}

void TestHttpDisconnectCancelsTool() {
	auto dispatcher = Core::Mcp::Dispatcher(u"Forkgram"_q, u"7.1.3"_q);
	auto cancelled = false;
	auto loop = QEventLoop();
	auto socket = QTcpSocket();
	dispatcher.addTool({
		.name = u"telegram.disconnect_test"_q,
		.description = u"Disconnect test"_q,
		.inputSchema = QJsonObject{ { u"type"_q, u"object"_q } },
		.outputSchema = QJsonObject{ { u"type"_q, u"object"_q } },
		.cancellableHandler = [&](
				const QJsonObject &,
				const Core::Mcp::CancellationPtr &cancellation,
				auto) {
			cancellation->setHandler([&] {
				cancelled = true;
				loop.quit();
			});
			socket.abort();
		},
	});
	auto server = Core::Mcp::HttpServer(&dispatcher);
	Require(server.listen(0), u"disconnect test server did not listen"_q);
	const auto body = QJsonDocument(QJsonObject{
		{ u"jsonrpc"_q, u"2.0"_q },
		{ u"id"_q, u"disconnect-call"_q },
		{ u"method"_q, u"tools/call"_q },
		{ u"params"_q, QJsonObject{
			{ u"name"_q, u"telegram.disconnect_test"_q },
			{ u"arguments"_q, QJsonObject() },
			{ u"_meta"_q, ValidMeta() },
		} },
	}).toJson(QJsonDocument::Compact);
	auto timeout = QTimer();
	timeout.setSingleShot(true);
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
	timeout.start(3000);
	socket.connectToHost(QHostAddress::LocalHost, server.port());
	socket.write(HttpRequestBytes(
		server.port(),
		"tools/call",
		body,
		"telegram.disconnect_test"));
	loop.exec();
	Require(timeout.isActive(), u"disconnect cancellation timed out"_q);
	Require(cancelled, u"disconnect did not cancel the active tool"_q);
}

void TestHttpSubscriptionsDeliverResourceUpdates() {
	auto dispatcher = Core::Mcp::Dispatcher(u"Forkgram"_q, u"7.1.3"_q);
	auto server = Core::Mcp::HttpServer(&dispatcher);
	Require(server.listen(0), u"subscription test server did not listen"_q);
	auto socket = QTcpSocket();
	auto response = QByteArray();
	auto published = false;
	auto loop = QEventLoop();
	auto timeout = QTimer();
	timeout.setSingleShot(true);
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
	QObject::connect(&socket, &QTcpSocket::readyRead, [&] {
		response.append(socket.readAll());
		if (!published
			&& response.contains("notifications/subscriptions/acknowledged")) {
			published = true;
			server.publish(
				u"notifications/resources/updated"_q,
				{ { u"uri"_q, u"telegram://updates?cursor=9"_q } });
		}
		if (response.contains("telegram://updates?cursor=9")) {
			loop.quit();
		}
	});
	timeout.start(3000);
	socket.connectToHost(QHostAddress::LocalHost, server.port());
	const auto body = QJsonDocument(QJsonObject{
		{ u"jsonrpc"_q, u"2.0"_q },
		{ u"id"_q, u"subscription-1"_q },
		{ u"method"_q, u"subscriptions/listen"_q },
		{ u"params"_q, QJsonObject{
			{ u"notifications"_q, QJsonObject{
				{ u"resourceSubscriptions"_q,
					QJsonArray{ u"telegram://updates"_q } },
			} },
			{ u"_meta"_q, ValidMeta() },
		} },
	}).toJson(QJsonDocument::Compact);
	socket.write(HttpRequestBytes(
		server.port(),
		"subscriptions/listen",
		body));
	loop.exec();
	socket.abort();
	Require(timeout.isActive(), u"subscription resource update timed out"_q);
	Require(response.contains("Content-Type: text/event-stream"),
		u"subscription did not use SSE"_q);
	Require(response.indexOf("notifications/subscriptions/acknowledged")
		< response.indexOf("notifications/resources/updated"),
		u"subscription update arrived before acknowledgement"_q);
	Require(response.count(
		"io.modelcontextprotocol/subscriptionId") >= 2,
		u"subscription messages omitted the subscription id"_q);
}

void TestSubscriptionsDoNotStarveOrdinaryCalls() {
	auto dispatcher = Core::Mcp::Dispatcher(u"Forkgram"_q, u"7.1.3"_q);
	auto server = Core::Mcp::HttpServer(&dispatcher);
	Require(server.listen(0), u"subscription capacity server did not listen"_q);
	constexpr auto kCount = 16;
	auto sockets = std::vector<std::unique_ptr<QTcpSocket>>();
	auto responses = std::vector<QByteArray>(kCount);
	auto acknowledged = std::vector<bool>(kCount);
	auto count = 0;
	auto loop = QEventLoop();
	auto timeout = QTimer();
	timeout.setSingleShot(true);
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
	for (auto index = 0; index != kCount; ++index) {
		auto socket = std::make_unique<QTcpSocket>();
		const auto raw = socket.get();
		QObject::connect(raw, &QTcpSocket::readyRead, [&, index, raw] {
			responses[index].append(raw->readAll());
			if (!acknowledged[index]
				&& responses[index].contains(
					"notifications/subscriptions/acknowledged")) {
				acknowledged[index] = true;
				if (++count == kCount) {
					loop.quit();
				}
			}
		});
		raw->connectToHost(QHostAddress::LocalHost, server.port());
		const auto body = QJsonDocument(QJsonObject{
			{ u"jsonrpc"_q, u"2.0"_q },
			{ u"id"_q, u"capacity-%1"_q.arg(index) },
			{ u"method"_q, u"subscriptions/listen"_q },
			{ u"params"_q, QJsonObject{
				{ u"notifications"_q, QJsonObject{
					{ u"resourceSubscriptions"_q,
						QJsonArray{ u"telegram://updates"_q } },
				} },
				{ u"_meta"_q, ValidMeta() },
			} },
		}).toJson(QJsonDocument::Compact);
		raw->write(HttpRequestBytes(
			server.port(),
			"subscriptions/listen",
			body));
		sockets.push_back(std::move(socket));
	}
	timeout.start(3000);
	loop.exec();
	Require(timeout.isActive() && count == kCount,
		u"subscription capacity setup timed out"_q);
	const auto discoverBody = QJsonDocument(QJsonObject{
		{ u"jsonrpc"_q, u"2.0"_q },
		{ u"id"_q, u"ordinary-after-subscriptions"_q },
		{ u"method"_q, u"server/discover"_q },
		{ u"params"_q, QJsonObject{ { u"_meta"_q, ValidMeta() } } },
	}).toJson(QJsonDocument::Compact);
	const auto ordinary = SendClosedRequest(
		server,
		"server/discover",
		discoverBody);
	Require(ordinary.startsWith("HTTP/1.1 200 OK\r\n"),
		u"subscriptions starved an ordinary MCP call"_q);
	const auto extraBody = QJsonDocument(QJsonObject{
		{ u"jsonrpc"_q, u"2.0"_q },
		{ u"id"_q, u"capacity-overflow"_q },
		{ u"method"_q, u"subscriptions/listen"_q },
		{ u"params"_q, QJsonObject{
			{ u"notifications"_q, QJsonObject() },
			{ u"_meta"_q, ValidMeta() },
		} },
	}).toJson(QJsonDocument::Compact);
	const auto overflow = SendClosedRequest(
		server,
		"subscriptions/listen",
		extraBody);
	Require(overflow.startsWith("HTTP/1.1 503 Service Unavailable\r\n"),
		u"subscription limit was not enforced"_q);
	for (const auto &socket : sockets) {
		socket->abort();
	}
}

void TestHttpServerDispatchesDiscover() {
	auto dispatcher = Core::Mcp::Dispatcher(u"Forkgram"_q, u"7.1.3"_q);
	auto server = Core::Mcp::HttpServer(&dispatcher);
	Require(server.listen(0), u"MCP HTTP server did not listen"_q);
	auto socket = QTcpSocket();
	auto response = QByteArray();
	auto loop = QEventLoop();
	QObject::connect(&socket, &QTcpSocket::readyRead, [&] {
		response.append(socket.readAll());
	});
	QObject::connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
	auto timeout = QTimer();
	timeout.setSingleShot(true);
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
	timeout.start(3000);
	socket.connectToHost(QHostAddress::LocalHost, server.port());
	const auto body = QJsonDocument(QJsonObject{
		{ u"jsonrpc"_q, u"2.0"_q },
		{ u"id"_q, 42 },
		{ u"method"_q, u"server/discover"_q },
		{ u"params"_q, QJsonObject{
			{ u"_meta"_q, ValidMeta() },
		} },
	}).toJson(QJsonDocument::Compact);
	const auto request = QByteArray("POST /mcp HTTP/1.1\r\n")
		+ "Host: 127.0.0.1:" + QByteArray::number(server.port()) + "\r\n"
		+ "Content-Type: application/json\r\n"
		+ "Accept: application/json, text/event-stream\r\n"
		+ "MCP-Protocol-Version: 2026-07-28\r\n"
		+ "Mcp-Method: server/discover\r\n"
		+ "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n"
		+ body;
	socket.write(request);
	loop.exec();
	Require(timeout.isActive(), u"MCP HTTP server timed out"_q);
	Require(response.startsWith("HTTP/1.1 200 OK\r\n"),
		u"MCP HTTP server returned a non-200 response"_q);
	const auto bodyOffset = response.indexOf("\r\n\r\n");
	Require(bodyOffset > 0, u"MCP HTTP response has no body"_q);
	const auto document = QJsonDocument::fromJson(response.mid(bodyOffset + 4));
	Require(document.object().value(u"id"_q) == 42,
		u"MCP HTTP response lost the request id"_q);
	Require(document.object().value(u"result"_q).toObject()
		.value(u"supportedVersions"_q).toArray()
		== QJsonArray{ u"2026-07-28"_q },
		u"MCP HTTP response lost the discover result"_q);
}

void TestTlRegistryLoadsGeneratedSchema() {
	const auto registry = Core::Mcp::TlRegistry(
		Core::Mcp::GeneratedSchemaJson());
	Require(registry.valid(), u"generated TL registry is invalid"_q);
	Require(registry.layer() == 229, u"TL registry loaded the wrong layer"_q);
	Require(registry.constructorCount() == 1658,
		u"TL registry lost constructors"_q);
	Require(registry.methodCount() == 813, u"TL registry lost methods"_q);
	const auto description = registry.describeMethod(u"messages.sendMessage"_q);
	Require(description.value(u"type"_q) == u"Updates"_q,
		u"TL registry returned the wrong result type"_q);
	Require(description.value(u"params"_q).toArray().size() == 23,
		u"TL registry returned the wrong sendMessage parameters"_q);
	Require(registry.describeMethod(u"missing.method"_q).isEmpty(),
		u"TL registry invented a missing method"_q);
}

void TestTlCodecEncodesAndDecodesWireValues() {
	const auto registry = Core::Mcp::TlRegistry(
		Core::Mcp::GeneratedSchemaJson());
	const auto codec = Core::Mcp::TlCodec(&registry);
	const auto config = codec.encodeMethod(u"help.getConfig"_q, {});
	Require(std::holds_alternative<Core::Mcp::TlEncodedRequest>(config),
		u"TL codec rejected help.getConfig"_q);
	Require(std::get<Core::Mcp::TlEncodedRequest>(config).bytes.toHex()
		== QByteArray("6b18f9c4"),
		u"TL codec encoded help.getConfig incorrectly"_q);
	const auto send = codec.encodeMethod(u"messages.sendMessage"_q, {
		{ u"peer"_q, QJsonObject{ { u"_"_q, u"inputPeerSelf"_q } } },
		{ u"message"_q, u"hi"_q },
		{ u"random_id"_q, u"42"_q },
	});
	Require(std::holds_alternative<Core::Mcp::TlEncodedRequest>(send),
		u"TL codec rejected minimal sendMessage"_q);
	Require(std::get<Core::Mcp::TlEncodedRequest>(send).bytes.toHex()
		== QByteArray("628ff4fe00000000c97ea07d026869002a00000000000000"),
		u"TL codec encoded sendMessage incorrectly"_q);
	const auto scheduled = codec.encodeMethod(u"messages.sendMessage"_q, {
		{ u"silent"_q, true },
		{ u"peer"_q, QJsonObject{ { u"_"_q, u"inputPeerSelf"_q } } },
		{ u"message"_q, u"hi"_q },
		{ u"random_id"_q, u"42"_q },
		{ u"schedule_date"_q, 123 },
	});
	Require(std::holds_alternative<Core::Mcp::TlEncodedRequest>(scheduled),
		u"TL codec rejected conditional flags"_q);
	Require(std::get<Core::Mcp::TlEncodedRequest>(scheduled).bytes.toHex()
		== QByteArray(
			"628ff4fe20040000c97ea07d026869002a000000000000007b000000"),
		u"TL codec encoded conditional flags incorrectly"_q);
	const auto removed = codec.encodeMethod(u"messages.deleteMessages"_q, {
		{ u"revoke"_q, true },
		{ u"id"_q, QJsonArray{ 1, 2 } },
	});
	Require(std::holds_alternative<Core::Mcp::TlEncodedRequest>(removed),
		u"TL codec rejected a Vector"_q);
	Require(std::get<Core::Mcp::TlEncodedRequest>(removed).bytes.toHex()
		== QByteArray("d2958ee50100000015c4b51c020000000100000002000000"),
		u"TL codec encoded a Vector incorrectly"_q);
	const auto generic = codec.encodeMethod(u"invokeAfterMsg"_q, {
		{ u"msg_id"_q, u"1"_q },
		{ u"query"_q, QJsonObject{
			{ u"_method"_q, u"help.getConfig"_q },
			{ u"params"_q, QJsonObject() },
		} },
	});
	Require(std::holds_alternative<Core::Mcp::TlEncodedRequest>(generic),
		u"TL codec rejected a generic query"_q);
	const auto &genericRequest = std::get<Core::Mcp::TlEncodedRequest>(generic);
	Require(genericRequest.bytes.toHex()
		== QByteArray("2d379fcb01000000000000006b18f9c4"),
		u"TL codec encoded a generic query incorrectly"_q);
	Require(genericRequest.resultType == u"Config"_q,
		u"TL codec did not resolve a generic result type"_q);
	const auto bytes = codec.encodeMethod(u"auth.importAuthorization"_q, {
		{ u"id"_q, u"1"_q },
		{ u"bytes"_q, u"AQI="_q },
	});
	Require(std::holds_alternative<Core::Mcp::TlEncodedRequest>(bytes),
		u"TL codec rejected bytes"_q);
	Require(std::get<Core::Mcp::TlEncodedRequest>(bytes).bytes.toHex()
		== QByteArray("ad7d7aa5010000000000000002010200"),
		u"TL codec encoded bytes incorrectly"_q);
	const auto invalidBytes = codec.encodeMethod(u"auth.importAuthorization"_q, {
		{ u"id"_q, u"1"_q },
		{ u"bytes"_q, u"not base64!"_q },
	});
	Require(std::holds_alternative<Core::Mcp::TlCodecError>(invalidBytes),
		u"TL codec accepted invalid base64"_q);
	const auto conferenceParams = [](const QString &key) {
		return QJsonObject{
			{ u"random_id"_q, 1 },
			{ u"public_key"_q, key },
			{ u"block"_q, u""_q },
			{ u"params"_q, QJsonObject{
				{ u"_"_q, u"dataJSON"_q },
				{ u"data"_q, u"{}"_q },
			} },
		};
	};
	const auto int256Max = codec.encodeMethod(
		u"phone.createConferenceCall"_q,
		conferenceParams(u"57896044618658097711785492504343953926634992332820282019728792003956564819967"_q));
	Require(std::holds_alternative<Core::Mcp::TlEncodedRequest>(int256Max),
		u"TL codec rejected signed int256 maximum"_q);
	const auto int256TooLarge = codec.encodeMethod(
		u"phone.createConferenceCall"_q,
		conferenceParams(u"57896044618658097711785492504343953926634992332820282019728792003956564819968"_q));
	Require(std::holds_alternative<Core::Mcp::TlCodecError>(int256TooLarge),
		u"TL codec accepted int256 above the signed maximum"_q);
	const auto int256Min = codec.encodeMethod(
		u"phone.createConferenceCall"_q,
		conferenceParams(u"-57896044618658097711785492504343953926634992332820282019728792003956564819968"_q));
	Require(std::holds_alternative<Core::Mcp::TlEncodedRequest>(int256Min),
		u"TL codec rejected signed int256 minimum"_q);
	const auto int256TooSmall = codec.encodeMethod(
		u"phone.createConferenceCall"_q,
		conferenceParams(u"-57896044618658097711785492504343953926634992332820282019728792003956564819969"_q));
	Require(std::holds_alternative<Core::Mcp::TlCodecError>(int256TooSmall),
		u"TL codec accepted int256 below the signed minimum"_q);
	const auto decoded = codec.decode(
		u"Bool"_q,
		QByteArray::fromHex("b5757299"));
	Require(std::holds_alternative<QJsonValue>(decoded),
		u"TL codec rejected boxed Bool"_q);
	Require(std::get<QJsonValue>(decoded).toBool(),
		u"TL codec decoded boolTrue as false"_q);
	const auto int128 = codec.decode(
		u"int128"_q,
		QByteArray::fromHex("01000000000000000000000000000000"));
	Require(std::holds_alternative<QJsonValue>(int128)
		&& std::get<QJsonValue>(int128).toString() == u"1"_q,
		u"TL codec decoded int128 incorrectly"_q);
	const auto int256 = codec.decode(u"int256"_q, QByteArray(32, char(0xFF)));
	Require(std::holds_alternative<QJsonValue>(int256)
		&& std::get<QJsonValue>(int256).toString() == u"-1"_q,
		u"TL codec decoded int256 incorrectly"_q);
	const auto flags2 = codec.decode(
		u"Chat"_q,
		QByteArray::fromHex(
			"54e9ef65"
			"00000000"
			"00001000"
			"0100000000000000"
			"01780000"
			"1c01c137"
			"02000000"));
	if (const auto error = std::get_if<Core::Mcp::TlCodecError>(&flags2)) {
		Fail(u"TL codec rejected a flags2 constructor: "_q + error->message);
	}
	const auto flags2Object = std::get<QJsonValue>(flags2).toObject();
	Require(flags2Object.value(u"_"_q) == u"community"_q
		&& flags2Object.value(u"collapsed_in_dialogs"_q).toBool(),
		u"TL codec decoded flags2 incorrectly"_q);
}

} // namespace

int main(int argc, char *argv[]) {
	const auto app = QCoreApplication(argc, argv);
	TestValidRequestParses();
	TestMissingMetadataIsRejected();
	TestClientInfoMetadataIsOptional();
	TestDiscoverResultAdvertisesCurrentProtocol();
	TestHttpParserWaitsForCompleteBody();
	TestHttpParserRejectsUnsupportedMethods();
	TestHttpParserRejectsRemoteOrigin();
	TestHttpParserRequiresJsonAndEventStreamAccept();
	TestHttpParserRequiresBothResponseTypes();
	TestHttpParserRejectsTransferEncoding();
	TestHttpParserEnforcesSizeLimits();
	TestDispatcherListsAndCallsTools();
	TestDispatcherListsAndReadsResources();
	TestDispatcherCancellationReachesTool();
	TestDispatcherValidatesToolInputSchema();
	TestDispatcherAllowsOmittedEmptyArguments();
	TestHttpServerSupportsCodexClient();
	TestHttpServerValidatesNameHeader();
	TestHttpServerReportsUnsupportedVersion();
	TestHttpServerUses404ForUnknownRpcMethod();
	TestHttpServerRebindIsAtomic();
	TestHttpServerStreamsProgressOnMainThread();
	TestHttpDisconnectCancelsTool();
	TestHttpSubscriptionsDeliverResourceUpdates();
	TestSubscriptionsDoNotStarveOrdinaryCalls();
	TestHttpServerDispatchesDiscover();
	TestTlRegistryLoadsGeneratedSchema();
	TestTlCodecEncodesAndDecodesWireValues();
	return 0;
}
