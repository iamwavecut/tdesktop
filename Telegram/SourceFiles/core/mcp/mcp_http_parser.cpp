/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mcp/mcp_http_parser.h"

#include "base/basic_types.h"

#include <QtCore/QUrl>

namespace Core::Mcp {
namespace {

[[nodiscard]] HttpParseResult Error(int status, QByteArray message) {
	return {
		.state = HttpParseState::Error,
		.status = status,
		.message = std::move(message),
	};
}

[[nodiscard]] bool HeaderHasValue(
		const QByteArray &header,
		const QByteArray &value) {
	for (const auto &part : header.toLower().split(',')) {
		if (part.trimmed() == value) {
			return true;
		}
	}
	return false;
}

} // namespace

HttpParseResult ParseHttpRequest(const QByteArray &input, quint16 port) {
	const auto headerEnd = input.indexOf("\r\n\r\n");
	if (headerEnd < 0) {
		return (input.size() <= kMaxHttpHeaderSize)
			? HttpParseResult()
			: Error(431, "Request Header Fields Too Large");
	} else if (headerEnd > kMaxHttpHeaderSize) {
		return Error(431, "Request Header Fields Too Large");
	}
	const auto lines = input.left(headerEnd).split('\n');
	if (lines.empty()) {
		return Error(400, "Bad Request");
	}
	const auto requestLine = lines.front().trimmed().split(' ');
	if (requestLine.size() != 3 || requestLine[2] != "HTTP/1.1") {
		return Error(400, "Bad Request");
	} else if (requestLine[0] != "POST") {
		return Error(405, "Method Not Allowed");
	} else if (requestLine[1] != "/mcp") {
		return Error(404, "Not Found");
	}
	auto headers = QMap<QByteArray, QByteArray>();
	for (auto i = 1; i != lines.size(); ++i) {
		const auto line = lines[i].trimmed();
		const auto colon = line.indexOf(':');
		if (colon <= 0) {
			return Error(400, "Bad Request");
		}
		const auto name = line.left(colon).trimmed().toLower();
		if (name.isEmpty() || headers.contains(name)) {
			return Error(400, "Bad Request");
		}
		headers.insert(name, line.mid(colon + 1).trimmed());
	}
	const auto expectedHost = QByteArray("127.0.0.1:")
		+ QByteArray::number(port);
	if (headers.value("host") != expectedHost) {
		return Error(403, "Forbidden");
	}
	if (const auto origin = headers.value("origin"); !origin.isEmpty()) {
		const auto parsed = QUrl::fromEncoded(origin);
		const auto host = parsed.host().toLower();
		if (!parsed.isValid()
			|| (host != u"127.0.0.1"_q && host != u"localhost"_q)) {
			return Error(403, "Forbidden");
		}
	}
	const auto contentType = headers.value("content-type").toLower().split(
		';').front().trimmed();
	if (contentType != "application/json") {
		return Error(415, "Unsupported Media Type");
	}
	if (headers.contains("transfer-encoding")) {
		return Error(400, "Bad Request");
	}
	const auto accept = headers.value("accept");
	if (!HeaderHasValue(accept, "application/json")
		|| !HeaderHasValue(accept, "text/event-stream")) {
		return Error(406, "Not Acceptable");
	}
	auto lengthOk = false;
	const auto contentLength = headers.value("content-length").toInt(&lengthOk);
	if (!lengthOk || contentLength < 0) {
		return Error(400, "Bad Request");
	} else if (contentLength > kMaxHttpBodySize) {
		return Error(413, "Content Too Large");
	}
	const auto bodyOffset = headerEnd + 4;
	if (input.size() < bodyOffset + contentLength) {
		return {};
	}
	return {
		.state = HttpParseState::Complete,
		.request = {
			.method = requestLine[0],
			.path = requestLine[1],
			.headers = std::move(headers),
			.body = input.mid(bodyOffset, contentLength),
		},
		.consumed = bodyOffset + contentLength,
	};
}

} // namespace Core::Mcp
