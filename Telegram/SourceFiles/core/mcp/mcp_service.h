/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/mcp/mcp_access_policy.h"
#include "core/mcp/mcp_dispatcher.h"
#include "core/mcp/mcp_http_server.h"
#include "core/mcp/mcp_tl_registry.h"
#include "core/mcp/mcp_tl_codec.h"
#include "core/mcp/mcp_ui_driver.h"

#include "base/weak_ptr.h"

#include <rpl/rpl.h>

#include <QtCore/QSet>

#include <deque>

namespace Main {
class Session;
} // namespace Main

namespace Core {

class Application;

namespace Mcp {

class Service final : public base::has_weak_ptr {
public:
	explicit Service(not_null<Application*> application);

	[[nodiscard]] bool start();
	[[nodiscard]] bool enabled() const;
	[[nodiscard]] bool setEnabled(bool enabled);
	[[nodiscard]] bool rebind(quint16 port);
	[[nodiscard]] bool authenticationEnabled() const;
	void setAuthenticationEnabled(bool enabled);
	[[nodiscard]] QString bearerTokenForCopy() const;
	[[nodiscard]] QString regenerateBearerToken();
	[[nodiscard]] const std::vector<ToolInfo> &tools() const;
	[[nodiscard]] bool toolEnabled(const QString &name) const;
	[[nodiscard]] int enabledToolCount() const;
	[[nodiscard]] int totalToolCount() const;
	[[nodiscard]] ToolCategoryState categoryState(
		const QString &category) const;
	void setToolEnabled(const QString &name, bool enabled);
	void setCategoryEnabled(const QString &category, bool enabled);
	[[nodiscard]] rpl::producer<> configurationChanges() const;
	[[nodiscard]] bool available() const;
	[[nodiscard]] quint16 port() const;
	[[nodiscard]] quint16 configuredPort() const;
	[[nodiscard]] QString endpoint() const;
	[[nodiscard]] QString errorString() const;

private:
	struct UpdateEvent {
		quint64 cursor = 0;
		QString type;
		QJsonObject data;
	};
	struct UpdateWaiter {
		quint64 id = 0;
		quint64 cursor = 0;
		int limit = 0;
		QString accountId;
		QSet<QString> filters;
		ToolCompletion done;
	};

	void registerTools();
	void saveToolPolicy();
	void notifyConfigurationChanged();
	void watchUpdates();
	void watchSession(Main::Session *session);
	void pushUpdate(QString type, QJsonObject data);
	[[nodiscard]] QJsonArray updatesAfter(
		quint64 cursor,
		int limit,
		const QString &accountId,
		const QSet<QString> &filters) const;
	void flushUpdateWaiters();
	void finishUpdateWaiter(quint64 id);
	void cancelUpdateWaiter(quint64 id);
	void watchUpdateWaiterSession(
		quint64 id,
		base::weak_ptr<Main::Session> session);
	[[nodiscard]] ToolResult clientState() const;

	const not_null<Application*> _application;
	Dispatcher _dispatcher;
	TlRegistry _tlRegistry;
	TlCodec _tlCodec;
	UiDriver _uiDriver;
	std::unique_ptr<AccessPolicy> _accessPolicy;
	quint16 _configuredPort = 0;
	std::deque<UpdateEvent> _updates;
	std::vector<UpdateWaiter> _updateWaiters;
	quint64 _nextUpdateCursor = 0;
	quint64 _nextWaiterId = 0;
	rpl::lifetime _activeSessionLifetime;
	rpl::lifetime _updatesLifetime;
	rpl::event_stream<> _configurationChanges;
	HttpServer _server;

};

} // namespace Mcp
} // namespace Core
