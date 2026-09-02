/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mcp/mcp_access_policy.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

#include <algorithm>

namespace Core::Mcp {

AccessPolicyState DefaultAccessPolicyState(bool existingMcpProfile) {
	return {
		.enabled = existingMcpProfile,
		.authenticationEnabled = !existingMcpProfile,
	};
}

AccessPolicy::AccessPolicy(
		std::vector<ToolInfo> tools,
		AccessPolicyState state)
: _tools(std::move(tools))
, _bearerToken(std::move(state.bearerToken))
, _enabled(state.enabled)
, _authenticationEnabled(state.authenticationEnabled) {
	std::ranges::sort(_tools, {}, &ToolInfo::name);
	for (const auto &tool : _tools) {
		_knownTools.insert(tool.name);
	}
	const auto document = QJsonDocument::fromJson(state.disabledTools);
	if (!document.isArray()) {
		return;
	}
	for (const auto &value : document.array()) {
		if (value.isString() && _knownTools.contains(value.toString())) {
			_disabledTools.insert(value.toString());
		}
	}
}

bool AccessPolicy::enabled() const {
	return _enabled;
}

bool AccessPolicy::authenticationEnabled() const {
	return _authenticationEnabled;
}

QByteArray AccessPolicy::bearerToken() const {
	return _bearerToken;
}

const std::vector<ToolInfo> &AccessPolicy::tools() const {
	return _tools;
}

bool AccessPolicy::toolEnabled(const QString &name) const {
	return _knownTools.contains(name) && !_disabledTools.contains(name);
}

int AccessPolicy::enabledToolCount() const {
	return int(_tools.size()) - _disabledTools.size();
}

ToolCategoryState AccessPolicy::categoryState(const QString &category) const {
	auto total = 0;
	auto enabled = 0;
	for (const auto &tool : _tools) {
		if (tool.category != category) {
			continue;
		}
		++total;
		if (toolEnabled(tool.name)) {
			++enabled;
		}
	}
	return !enabled
		? ToolCategoryState::None
		: (enabled == total)
		? ToolCategoryState::All
		: ToolCategoryState::Partial;
}

QByteArray AccessPolicy::serializedDisabledTools() const {
	auto result = QJsonArray();
	for (const auto &tool : _tools) {
		if (_disabledTools.contains(tool.name)) {
			result.append(tool.name);
		}
	}
	return QJsonDocument(result).toJson(QJsonDocument::Compact);
}

bool AccessPolicy::setEnabled(bool enabled) {
	if (_enabled == enabled) {
		return false;
	}
	_enabled = enabled;
	return true;
}

bool AccessPolicy::setAuthenticationEnabled(bool enabled) {
	if (_authenticationEnabled == enabled) {
		return false;
	}
	_authenticationEnabled = enabled;
	return true;
}

bool AccessPolicy::setBearerToken(QByteArray token) {
	if (_bearerToken == token) {
		return false;
	}
	_bearerToken = std::move(token);
	return true;
}

bool AccessPolicy::setToolEnabled(const QString &name, bool enabled) {
	if (!_knownTools.contains(name) || toolEnabled(name) == enabled) {
		return false;
	}
	if (enabled) {
		_disabledTools.remove(name);
	} else {
		_disabledTools.insert(name);
	}
	return true;
}

bool AccessPolicy::setCategoryEnabled(
		const QString &category,
		bool enabled) {
	auto changed = false;
	for (const auto &tool : _tools) {
		if (tool.category == category) {
			changed = setToolEnabled(tool.name, enabled) || changed;
		}
	}
	return changed;
}

} // namespace Core::Mcp
