/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mcp/mcp_dispatcher.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

#include <algorithm>
#include <cmath>

namespace Core::Mcp {
namespace {

[[nodiscard]] QJsonObject Result(
		const QJsonValue &id,
		QJsonObject result) {
	return {
		{ u"jsonrpc"_q, u"2.0"_q },
		{ u"id"_q, id },
		{ u"result"_q, std::move(result) },
	};
}

[[nodiscard]] QJsonObject Error(
		const QJsonValue &id,
		int code,
		QString message,
		QJsonObject data = {}) {
	auto error = QJsonObject{
		{ u"code"_q, code },
		{ u"message"_q, std::move(message) },
	};
	if (!data.isEmpty()) {
		error.insert(u"data"_q, std::move(data));
	}
	return {
		{ u"jsonrpc"_q, u"2.0"_q },
		{ u"id"_q, id },
		{ u"error"_q, std::move(error) },
	};
}

[[nodiscard]] QString StructuredText(const QJsonValue &value) {
	if (value.isObject()) {
		return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(
			QJsonDocument::Compact));
	} else if (value.isArray()) {
		return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(
			QJsonDocument::Compact));
	} else if (value.isString()) {
		return value.toString();
	} else if (value.isBool()) {
		return value.toBool() ? u"true"_q : u"false"_q;
	} else if (value.isDouble()) {
		return QString::number(value.toDouble(), 'g', 16);
	}
	return u"null"_q;
}

[[nodiscard]] bool MatchesType(
		const QJsonValue &value,
		const QString &type) {
	if (type == u"object"_q) {
		return value.isObject();
	} else if (type == u"array"_q) {
		return value.isArray();
	} else if (type == u"string"_q) {
		return value.isString();
	} else if (type == u"boolean"_q) {
		return value.isBool();
	} else if (type == u"number"_q) {
		return value.isDouble() && std::isfinite(value.toDouble());
	} else if (type == u"integer"_q) {
		return value.isDouble()
			&& std::isfinite(value.toDouble())
			&& std::trunc(value.toDouble()) == value.toDouble();
	} else if (type == u"null"_q) {
		return value.isNull();
	}
	return false;
}

[[nodiscard]] QString ValidateSchema(
		const QJsonObject &schema,
		const QJsonValue &value,
		const QString &path = u"arguments"_q) {
	const auto type = schema.value(u"type"_q);
	if (type.isString()) {
		if (!MatchesType(value, type.toString())) {
			return path + u" has the wrong type"_q;
		}
	} else if (type.isArray()) {
		auto matches = false;
		for (const auto &entry : type.toArray()) {
			if (entry.isString() && MatchesType(value, entry.toString())) {
				matches = true;
				break;
			}
		}
		if (!matches) {
			return path + u" has the wrong type"_q;
		}
	}
	if (schema.contains(u"const"_q) && value != schema.value(u"const"_q)) {
		return path + u" does not match const"_q;
	}
	if (const auto values = schema.value(u"enum"_q); values.isArray()) {
		auto found = false;
		for (const auto &entry : values.toArray()) {
			if (entry == value) {
				found = true;
				break;
			}
		}
		if (!found) {
			return path + u" is not an allowed value"_q;
		}
	}
	if (value.isObject()) {
		const auto object = value.toObject();
		const auto properties = schema.value(u"properties"_q).toObject();
		for (const auto &entry : schema.value(u"required"_q).toArray()) {
			const auto name = entry.toString();
			if (!object.contains(name)) {
				return path + u"."_q + name + u" is required"_q;
			}
		}
		if (schema.value(u"additionalProperties"_q) == false) {
			for (auto i = object.begin(); i != object.end(); ++i) {
				if (!properties.contains(i.key())) {
					return path + u"."_q + i.key() + u" is unknown"_q;
				}
			}
		}
		for (auto i = properties.begin(); i != properties.end(); ++i) {
			if (!object.contains(i.key()) || !i.value().isObject()) {
				continue;
			}
			if (const auto error = ValidateSchema(
					i.value().toObject(),
					object.value(i.key()),
					path + u"."_q + i.key()); !error.isEmpty()) {
				return error;
			}
		}
	} else if (value.isArray()) {
		const auto array = value.toArray();
		if (schema.contains(u"minItems"_q)
			&& array.size() < schema.value(u"minItems"_q).toInt()) {
			return path + u" has too few items"_q;
		} else if (schema.contains(u"maxItems"_q)
			&& array.size() > schema.value(u"maxItems"_q).toInt()) {
			return path + u" has too many items"_q;
		}
		const auto items = schema.value(u"items"_q);
		if (items.isObject()) {
			for (auto i = 0; i != array.size(); ++i) {
				if (const auto error = ValidateSchema(
						items.toObject(),
						array[i],
						path + u"[%1]"_q.arg(i)); !error.isEmpty()) {
					return error;
				}
			}
		}
	} else if (value.isString()) {
		const auto size = value.toString().size();
		if (schema.contains(u"minLength"_q)
			&& size < schema.value(u"minLength"_q).toInt()) {
			return path + u" is too short"_q;
		} else if (schema.contains(u"maxLength"_q)
			&& size > schema.value(u"maxLength"_q).toInt()) {
			return path + u" is too long"_q;
		}
	} else if (value.isDouble()) {
		const auto number = value.toDouble();
		if (schema.contains(u"minimum"_q)
			&& number < schema.value(u"minimum"_q).toDouble()) {
			return path + u" is below minimum"_q;
		} else if (schema.contains(u"maximum"_q)
			&& number > schema.value(u"maximum"_q).toDouble()) {
			return path + u" is above maximum"_q;
		}
	}
	return QString();
}

} // namespace

void Cancellation::setHandler(Handler handler) {
	if (_cancelled) {
		handler();
	} else {
		_handler = std::move(handler);
	}
}

void Cancellation::cancel() {
	if (_cancelled) {
		return;
	}
	_cancelled = true;
	if (_handler) {
		auto handler = std::move(_handler);
		handler();
	}
}

bool Cancellation::cancelled() const {
	return _cancelled;
}

Dispatcher::Dispatcher(QString name, QString version)
: _name(std::move(name))
, _version(std::move(version)) {
}

void Dispatcher::addTool(Tool tool) {
	const auto i = std::lower_bound(
		begin(_tools),
		end(_tools),
		tool.name,
		[](const Tool &entry, const QString &name) {
			return entry.name < name;
		});
	if (i != end(_tools) && i->name == tool.name) {
		*i = std::move(tool);
	} else {
		_tools.insert(i, std::move(tool));
	}
}

void Dispatcher::addResource(Resource resource) {
	const auto i = std::lower_bound(
		begin(_resources),
		end(_resources),
		resource.uri,
		[](const Resource &entry, const QString &uri) {
			return entry.uri < uri;
		});
	if (i != end(_resources) && i->uri == resource.uri) {
		*i = std::move(resource);
	} else {
		_resources.insert(i, std::move(resource));
	}
}

void Dispatcher::addResourceTemplate(ResourceTemplate resourceTemplate) {
	const auto i = std::lower_bound(
		begin(_resourceTemplates),
		end(_resourceTemplates),
		resourceTemplate.uriTemplate,
		[](const ResourceTemplate &entry, const QString &uri) {
			return entry.uriTemplate < uri;
		});
	if (i != end(_resourceTemplates)
		&& i->uriTemplate == resourceTemplate.uriTemplate) {
		*i = std::move(resourceTemplate);
	} else {
		_resourceTemplates.insert(i, std::move(resourceTemplate));
	}
}

void Dispatcher::addPrompt(Prompt prompt) {
	const auto i = std::lower_bound(
		begin(_prompts),
		end(_prompts),
		prompt.name,
		[](const Prompt &entry, const QString &name) {
			return entry.name < name;
		});
	if (i != end(_prompts) && i->name == prompt.name) {
		*i = std::move(prompt);
	} else {
		_prompts.insert(i, std::move(prompt));
	}
}

void Dispatcher::setCompletionHandler(JsonHandler handler) {
	_completionHandler = std::move(handler);
}

CancellationPtr Dispatcher::handle(
		const Request &request,
		Completion done,
		CancellationPtr cancellation) const {
	if (!cancellation) {
		cancellation = std::make_shared<Cancellation>();
	}
	if (request.method == u"initialize"_q) {
		auto result = InitializeResult(_name, _version);
		auto capabilities = result.value(u"capabilities"_q).toObject();
		if (!_prompts.empty()) {
			capabilities.insert(u"prompts"_q, QJsonObject{
				{ u"listChanged"_q, false },
			});
		}
		if (_completionHandler) {
			capabilities.insert(u"completions"_q, QJsonObject());
		}
		result.insert(u"capabilities"_q, std::move(capabilities));
		done(Result(request.id, std::move(result)));
	} else if (request.method == u"server/discover"_q) {
		auto result = DiscoverResult(_name, _version);
		auto capabilities = result.value(u"capabilities"_q).toObject();
		if (!_prompts.empty()) {
			capabilities.insert(u"prompts"_q, QJsonObject{
				{ u"listChanged"_q, false },
			});
		}
		if (_completionHandler) {
			capabilities.insert(u"completions"_q, QJsonObject());
		}
		result.insert(u"capabilities"_q, std::move(capabilities));
		done(Result(request.id, std::move(result)));
	} else if (request.method == u"tools/list"_q) {
		done(listTools(request));
	} else if (request.method == u"tools/call"_q) {
		callTool(request, cancellation, std::move(done));
	} else if (request.method == u"resources/list"_q) {
		done(listResources(request));
	} else if (request.method == u"resources/templates/list"_q) {
		done(listResourceTemplates(request));
	} else if (request.method == u"resources/read"_q) {
		readResource(request, std::move(done));
	} else if (request.method == u"prompts/list"_q && !_prompts.empty()) {
		done(listPrompts(request));
	} else if (request.method == u"prompts/get"_q && !_prompts.empty()) {
		getPrompt(request, std::move(done));
	} else if (request.method == u"completion/complete"_q) {
		complete(request, std::move(done));
	} else {
		done(Error(request.id, -32601, u"Method not found"_q));
	}
	return cancellation;
}

QJsonObject Dispatcher::listPrompts(const Request &request) const {
	auto prompts = QJsonArray();
	for (const auto &prompt : _prompts) {
		auto value = QJsonObject{
			{ u"name"_q, prompt.name },
			{ u"description"_q, prompt.description },
		};
		if (!prompt.arguments.isEmpty()) {
			value.insert(u"arguments"_q, prompt.arguments);
		}
		prompts.append(std::move(value));
	}
	return Result(request.id, {
		{ u"resultType"_q, u"complete"_q },
		{ u"prompts"_q, std::move(prompts) },
		{ u"ttlMs"_q, 300000 },
		{ u"cacheScope"_q, u"public"_q },
	});
}

void Dispatcher::getPrompt(const Request &request, Completion done) const {
	const auto name = request.params.value(u"name"_q).toString();
	const auto i = std::lower_bound(
		begin(_prompts),
		end(_prompts),
		name,
		[](const Prompt &entry, const QString &value) {
			return entry.name < value;
		});
	if (name.isEmpty() || i == end(_prompts) || i->name != name) {
		done(Error(request.id, -32602, u"Unknown prompt: "_q + name));
		return;
	}
	const auto id = request.id;
	i->handler(request.params, [id, done = std::move(done)](
			QJsonObject value) mutable {
		if (!value.contains(u"resultType"_q)) {
			value.insert(u"resultType"_q, u"complete"_q);
		}
		done(Result(id, std::move(value)));
	});
}

void Dispatcher::complete(const Request &request, Completion done) const {
	if (!_completionHandler) {
		done(Error(request.id, -32601, u"Method not found"_q));
		return;
	}
	const auto id = request.id;
	_completionHandler(request.params, [id, done = std::move(done)](
			QJsonObject value) mutable {
		if (!value.contains(u"resultType"_q)) {
			value.insert(u"resultType"_q, u"complete"_q);
		}
		done(Result(id, std::move(value)));
	});
}

QJsonObject Dispatcher::listResourceTemplates(const Request &request) const {
	auto templates = QJsonArray();
	for (const auto &entry : _resourceTemplates) {
		templates.append(QJsonObject{
			{ u"uriTemplate"_q, entry.uriTemplate },
			{ u"name"_q, entry.name },
			{ u"title"_q, entry.title },
			{ u"description"_q, entry.description },
			{ u"mimeType"_q, entry.mimeType },
		});
	}
	return Result(request.id, {
		{ u"resultType"_q, u"complete"_q },
		{ u"resourceTemplates"_q, std::move(templates) },
		{ u"ttlMs"_q, 300000 },
		{ u"cacheScope"_q, u"public"_q },
	});
}

QJsonObject Dispatcher::listResources(const Request &request) const {
	auto resources = QJsonArray();
	for (const auto &resource : _resources) {
		resources.append(QJsonObject{
			{ u"uri"_q, resource.uri },
			{ u"name"_q, resource.name },
			{ u"title"_q, resource.title },
			{ u"description"_q, resource.description },
			{ u"mimeType"_q, resource.mimeType },
		});
	}
	return Result(request.id, {
		{ u"resultType"_q, u"complete"_q },
		{ u"resources"_q, std::move(resources) },
		{ u"ttlMs"_q, 300000 },
		{ u"cacheScope"_q, u"private"_q },
	});
}

void Dispatcher::readResource(const Request &request, Completion done) const {
	const auto uriValue = request.params.value(u"uri"_q);
	if (!uriValue.isString()) {
		done(Error(request.id, -32602, u"Invalid resource URI"_q));
		return;
	}
	const auto uri = uriValue.toString();
	const auto i = std::lower_bound(
		begin(_resources),
		end(_resources),
		uri,
		[](const Resource &entry, const QString &value) {
			return entry.uri < value;
		});
	if (i == end(_resources) || i->uri != uri) {
		for (const auto &entry : _resourceTemplates) {
			const auto open = entry.uriTemplate.indexOf('{');
			const auto close = entry.uriTemplate.indexOf('}', open + 1);
			if (open < 0 || close < 0) {
				continue;
			}
			const auto prefix = entry.uriTemplate.left(open);
			const auto suffix = entry.uriTemplate.mid(close + 1);
			if (!uri.startsWith(prefix)
				|| !uri.endsWith(suffix)
				|| uri.size() <= prefix.size() + suffix.size()) {
				continue;
			}
			const auto id = request.id;
			const auto requestedUri = uri;
			entry.handler(uri, [
					id,
					requestedUri,
					done = std::move(done)](
					QJsonArray contents) mutable {
				if (contents.isEmpty()) {
					done(Error(
						id,
						-32602,
						u"Resource not found"_q,
						{ { u"uri"_q, requestedUri } }));
					return;
				}
				done(Result(id, {
					{ u"resultType"_q, u"complete"_q },
					{ u"contents"_q, std::move(contents) },
					{ u"ttlMs"_q, 0 },
					{ u"cacheScope"_q, u"private"_q },
				}));
			});
			return;
		}
		done(Error(request.id, -32602, u"Resource not found"_q));
		return;
	}
	const auto id = request.id;
	const auto handler = i->handler;
	handler([id, done = std::move(done)](QJsonArray contents) mutable {
		done(Result(id, {
			{ u"resultType"_q, u"complete"_q },
			{ u"contents"_q, std::move(contents) },
			{ u"ttlMs"_q, 0 },
			{ u"cacheScope"_q, u"private"_q },
		}));
	});
}

QJsonObject Dispatcher::listTools(const Request &request) const {
	auto tools = QJsonArray();
	for (const auto &tool : _tools) {
		tools.append(QJsonObject{
			{ u"name"_q, tool.name },
			{ u"description"_q, tool.description },
			{ u"inputSchema"_q, tool.inputSchema },
			{ u"outputSchema"_q, tool.outputSchema },
		});
	}
	return Result(request.id, {
		{ u"resultType"_q, u"complete"_q },
		{ u"tools"_q, std::move(tools) },
		{ u"ttlMs"_q, 300000 },
		{ u"cacheScope"_q, u"public"_q },
	});
}

void Dispatcher::callTool(
		const Request &request,
		const CancellationPtr &cancellation,
		Completion done) const {
	const auto nameValue = request.params.value(u"name"_q);
	const auto argumentsValue = request.params.value(u"arguments"_q);
	if (!nameValue.isString()
		|| (!argumentsValue.isUndefined() && !argumentsValue.isObject())) {
		done(Error(request.id, -32602, u"Invalid tool call"_q));
		return;
	}
	const auto name = nameValue.toString();
	const auto i = std::lower_bound(
		begin(_tools),
		end(_tools),
		name,
		[](const Tool &entry, const QString &value) {
			return entry.name < value;
		});
	if (i == end(_tools) || i->name != name) {
		done(Error(
			request.id,
			-32602,
			u"Unknown tool: "_q + name));
		return;
	}
	if (!i->handler && !i->cancellableHandler && !i->advancedHandler) {
		done(Error(request.id, -32603, u"Tool has no handler"_q));
		return;
	}
	if (!i->requiredClientCapability.isEmpty()) {
		const auto capabilities = request.params.value(u"_meta"_q).toObject()
			.value(u"io.modelcontextprotocol/clientCapabilities"_q).toObject();
		if (!capabilities.contains(i->requiredClientCapability)) {
			done(Error(
				request.id,
				-32021,
				u"Missing required client capability"_q,
				{ { u"requiredCapabilities"_q, QJsonObject{
					{ i->requiredClientCapability, QJsonObject() },
				} } }));
			return;
		}
	}
	const auto arguments = argumentsValue.isUndefined()
		? QJsonObject()
		: argumentsValue.toObject();
	if (const auto schemaError = ValidateSchema(i->inputSchema, arguments);
			!schemaError.isEmpty()) {
		done(Error(
			request.id,
			-32602,
			u"Invalid tool arguments: "_q + schemaError));
		return;
	}
	const auto id = request.id;
	const auto finish = [id, done = std::move(done)](
			ToolResult result) mutable {
		if (result.errorCode) {
			done(Error(
				id,
				result.errorCode,
				result.errorMessage.isEmpty()
					? u"Tool request failed"_q
					: std::move(result.errorMessage),
				std::move(result.errorData)));
			return;
		}
		if (!result.result.isEmpty()) {
			if (!result.result.contains(u"resultType"_q)) {
				result.result.insert(u"resultType"_q, u"complete"_q);
			}
			done(Result(id, std::move(result.result)));
			return;
		}
		auto content = std::move(result.content);
		if (content.isEmpty()) {
			content.append(QJsonObject{
				{ u"type"_q, u"text"_q },
				{ u"text"_q, StructuredText(result.structuredContent) },
			});
		}
		auto value = QJsonObject{
			{ u"resultType"_q, u"complete"_q },
			{ u"content"_q, std::move(content) },
			{ u"structuredContent"_q, result.structuredContent },
		};
		if (result.isError) {
			value.insert(u"isError"_q, true);
		}
		done(Result(id, std::move(value)));
	};
	if (i->advancedHandler) {
		i->advancedHandler(request.params, cancellation, std::move(finish));
	} else if (i->cancellableHandler) {
		i->cancellableHandler(
			arguments,
			cancellation,
			std::move(finish));
	} else if (i->handler) {
		i->handler(arguments, std::move(finish));
	}
}

} // namespace Core::Mcp
