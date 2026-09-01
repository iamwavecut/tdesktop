/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>

#include <memory>

class QThread;

namespace Core::Mcp {

class Dispatcher;
class Cancellation;
struct Request;

class HttpServer final : public QObject {
public:
	explicit HttpServer(Dispatcher *dispatcher);
	~HttpServer();

	[[nodiscard]] bool listen(quint16 port);
	[[nodiscard]] quint16 port() const;
	[[nodiscard]] QString errorString() const;
	void publish(QString method, QJsonObject params);

private:
	class Worker;

	void dispatch(quint64 serial, Request request);
	void finish(quint64 serial, QJsonObject response);
	void cancel(quint64 serial);

	Dispatcher *_dispatcher = nullptr;
	std::unique_ptr<QThread> _thread;
	Worker *_worker = nullptr;
	QHash<quint64, std::shared_ptr<Cancellation>> _calls;
	quint16 _port = 0;
	QString _errorString;

};

} // namespace Core::Mcp
