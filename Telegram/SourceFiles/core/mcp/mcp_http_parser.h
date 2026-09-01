/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QMap>

namespace Core::Mcp {

inline constexpr auto kMaxHttpHeaderSize = 64 * 1024;
inline constexpr auto kMaxHttpBodySize = 4 * 1024 * 1024;

enum class HttpParseState {
	NeedMore,
	Complete,
	Error,
};

struct HttpRequest {
	QByteArray method;
	QByteArray path;
	QMap<QByteArray, QByteArray> headers;
	QByteArray body;
};

struct HttpParseResult {
	HttpParseState state = HttpParseState::NeedMore;
	HttpRequest request;
	qsizetype consumed = 0;
	int status = 0;
	QByteArray message;
};

[[nodiscard]] HttpParseResult ParseHttpRequest(
	const QByteArray &input,
	quint16 port);

} // namespace Core::Mcp
