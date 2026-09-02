/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/mcp/mcp_dispatcher.h"

#include <QtCore/QByteArray>
#include <QtCore/QSet>

namespace Core::Mcp {

enum class ToolCategoryState {
	None,
	Partial,
	All,
};

struct AccessPolicyState {
	bool enabled = false;
	bool authenticationEnabled = true;
	QByteArray bearerToken;
	QByteArray disabledTools;
};

[[nodiscard]] AccessPolicyState DefaultAccessPolicyState(
	bool existingMcpProfile);

class AccessPolicy final {
public:
	AccessPolicy(std::vector<ToolInfo> tools, AccessPolicyState state);

	[[nodiscard]] bool enabled() const;
	[[nodiscard]] bool authenticationEnabled() const;
	[[nodiscard]] QByteArray bearerToken() const;
	[[nodiscard]] const std::vector<ToolInfo> &tools() const;
	[[nodiscard]] bool toolEnabled(const QString &name) const;
	[[nodiscard]] int enabledToolCount() const;
	[[nodiscard]] ToolCategoryState categoryState(
		const QString &category) const;
	[[nodiscard]] QByteArray serializedDisabledTools() const;

	bool setEnabled(bool enabled);
	bool setAuthenticationEnabled(bool enabled);
	bool setBearerToken(QByteArray token);
	bool setToolEnabled(const QString &name, bool enabled);
	bool setCategoryEnabled(const QString &category, bool enabled);

private:
	std::vector<ToolInfo> _tools;
	QSet<QString> _knownTools;
	QSet<QString> _disabledTools;
	QByteArray _bearerToken;
	bool _enabled = false;
	bool _authenticationEnabled = true;

};

} // namespace Core::Mcp
