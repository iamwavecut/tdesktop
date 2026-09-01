/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mcp/mcp_protocol.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>

namespace Core::Mcp {
namespace {

[[nodiscard]] ProtocolError Error(
		QJsonValue id,
		int code,
		QString message) {
	return {
		.id = std::move(id),
		.code = code,
		.message = std::move(message),
	};
}

[[nodiscard]] bool ValidId(const QJsonValue &id) {
	return id.isString() || id.isDouble();
}

} // namespace

ParsedRequest ParseRequest(
		const QByteArray &bytes,
		const QByteArray &protocolVersion) {
	auto parseError = QJsonParseError();
	const auto document = QJsonDocument::fromJson(bytes, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		return Error({}, -32700, u"Parse error"_q);
	}
	const auto object = document.object();
	const auto id = object.value(u"id"_q);
	const auto methodValue = object.value(u"method"_q);
	if (object.value(u"jsonrpc"_q) != u"2.0"_q
		|| !methodValue.isString()
		|| methodValue.toString().isEmpty()) {
		return Error(id, -32600, u"Invalid Request"_q);
	}
	const auto method = methodValue.toString();
	const auto compatibility = (method == u"initialize"_q)
		|| (protocolVersion == kCompatibilityProtocolVersion);
	const auto notification = id.isUndefined();
	const auto paramsValue = object.value(u"params"_q);
	const auto initialized = compatibility
		&& notification
		&& (method == u"notifications/initialized"_q);
	if ((!notification && !ValidId(id))
		|| (notification
			&& (!compatibility || !method.startsWith(u"notifications/"_q)))
		|| (!paramsValue.isObject()
			&& !(initialized && paramsValue.isUndefined()))) {
		return Error(id, -32600, u"Invalid Request"_q);
	}
	const auto params = paramsValue.toObject();
	if (method == u"initialize"_q) {
		if (!params.value(u"protocolVersion"_q).isString()
			|| !params.value(u"capabilities"_q).isObject()
			|| !params.value(u"clientInfo"_q).isObject()) {
			return Error(id, -32602, u"Invalid initialize request"_q);
		}
		return Request{
			.id = id,
			.method = method,
			.params = params,
			.dialect = ProtocolDialect::Compatibility,
		};
	} else if (compatibility) {
		return Request{
			.id = id,
			.method = method,
			.params = params,
			.dialect = ProtocolDialect::Compatibility,
		};
	}
	const auto metaValue = params.value(u"_meta"_q);
	if (!metaValue.isObject()) {
		return Error(id, -32602, u"Missing MCP request metadata"_q);
	}
	const auto meta = metaValue.toObject();
	const auto version = meta.value(
		u"io.modelcontextprotocol/protocolVersion"_q);
	const auto clientInfo = meta.value(
		u"io.modelcontextprotocol/clientInfo"_q);
	if (!version.isString()
		|| version.toString().isEmpty()
		|| (!clientInfo.isUndefined() && !clientInfo.isObject())
		|| !meta.value(
			u"io.modelcontextprotocol/clientCapabilities"_q
		).isObject()) {
		return Error(id, -32602, u"Invalid MCP request metadata"_q);
	}
	return Request{
		.id = id,
		.method = method,
		.params = params,
	};
}

QJsonObject DiscoverResult(const QString &name, const QString &version) {
	return {
		{ u"resultType"_q, u"complete"_q },
		{ u"supportedVersions"_q, QJsonArray{
			QString::fromLatin1(kProtocolVersion),
		} },
		{ u"capabilities"_q, QJsonObject{
			{ u"tools"_q, QJsonObject{
				{ u"listChanged"_q, false },
			} },
			{ u"resources"_q, QJsonObject{
				{ u"listChanged"_q, false },
				{ u"subscribe"_q, true },
			} },
		} },
		{ u"_meta"_q, QJsonObject{
			{ u"io.modelcontextprotocol/serverInfo"_q, QJsonObject{
				{ u"name"_q, name },
				{ u"version"_q, version },
			} },
		} },
		{ u"instructions"_q,
			u"Control the active Forkgram session through Telegram tools."_q },
		{ u"ttlMs"_q, 3600000 },
		{ u"cacheScope"_q, u"public"_q },
	};
}

QJsonObject InitializeResult(const QString &name, const QString &version) {
	return {
		{ u"protocolVersion"_q,
			QString::fromLatin1(kCompatibilityProtocolVersion) },
		{ u"capabilities"_q, QJsonObject{
			{ u"tools"_q, QJsonObject{
				{ u"listChanged"_q, false },
			} },
			{ u"resources"_q, QJsonObject{
				{ u"listChanged"_q, false },
				{ u"subscribe"_q, false },
			} },
		} },
		{ u"serverInfo"_q, QJsonObject{
			{ u"name"_q, name },
			{ u"version"_q, version },
		} },
		{ u"instructions"_q,
			u"Control the active Forkgram session through Telegram tools."_q },
	};
}

} // namespace Core::Mcp
