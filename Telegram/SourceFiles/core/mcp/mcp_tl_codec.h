/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

#include <variant>

namespace Core::Mcp {

class TlRegistry;

struct TlCodecError {
	QString message;
};

struct TlEncodedRequest {
	QByteArray bytes;
	QString resultType;
};

using TlEncodeResult = std::variant<TlEncodedRequest, TlCodecError>;
using TlDecodeResult = std::variant<QJsonValue, TlCodecError>;

class TlCodec final {
public:
	explicit TlCodec(const TlRegistry *registry);

	[[nodiscard]] TlEncodeResult encodeMethod(
		const QString &method,
		const QJsonObject &params) const;
	[[nodiscard]] TlDecodeResult decode(
		const QString &type,
		const QByteArray &bytes) const;

private:
	const TlRegistry *_registry = nullptr;

};

} // namespace Core::Mcp
