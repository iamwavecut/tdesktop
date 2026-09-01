/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

#include <variant>

namespace Core::Mcp {

inline constexpr auto kProtocolVersion = "2026-07-28";
inline constexpr auto kCompatibilityProtocolVersion = "2025-06-18";

enum class ProtocolDialect {
	Current,
	Compatibility,
};

struct Request {
	QJsonValue id;
	QString method;
	QJsonObject params;
	ProtocolDialect dialect = ProtocolDialect::Current;
};

struct ProtocolError {
	QJsonValue id;
	int code = 0;
	QString message;
};

using ParsedRequest = std::variant<Request, ProtocolError>;

[[nodiscard]] ParsedRequest ParseRequest(
	const QByteArray &bytes,
	const QByteArray &protocolVersion = {});
[[nodiscard]] QJsonObject DiscoverResult(
	const QString &name,
	const QString &version);
[[nodiscard]] QJsonObject InitializeResult(
	const QString &name,
	const QString &version);

} // namespace Core::Mcp
