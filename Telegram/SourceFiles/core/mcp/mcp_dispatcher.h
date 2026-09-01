/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/mcp/mcp_protocol.h"

#include <QtCore/QJsonArray>

#include <functional>
#include <memory>
#include <vector>

namespace Core::Mcp {

struct ToolResult {
	QJsonValue structuredContent = QJsonObject();
	QJsonArray content;
	QJsonObject result;
	int errorCode = 0;
	QString errorMessage;
	QJsonObject errorData;
	bool isError = false;
};

using ToolCompletion = std::function<void(ToolResult)>;
using ToolHandler = std::function<void(const QJsonObject&, ToolCompletion)>;

class Cancellation final {
public:
	using Handler = std::function<void()>;

	void setHandler(Handler handler);
	void cancel();
	[[nodiscard]] bool cancelled() const;

private:
	Handler _handler;
	bool _cancelled = false;

};

using CancellationPtr = std::shared_ptr<Cancellation>;
using CancellableToolHandler = std::function<void(
	const QJsonObject&,
	const CancellationPtr&,
	ToolCompletion)>;
using AdvancedToolHandler = std::function<void(
	const QJsonObject&,
	const CancellationPtr&,
	ToolCompletion)>;

struct Tool {
	QString name;
	QString description;
	QJsonObject inputSchema;
	QJsonObject outputSchema;
	ToolHandler handler;
	CancellableToolHandler cancellableHandler;
	AdvancedToolHandler advancedHandler;
	QString requiredClientCapability;
};

using JsonCompletion = std::function<void(QJsonObject)>;
using JsonHandler = std::function<void(const QJsonObject&, JsonCompletion)>;

struct Prompt {
	QString name;
	QString description;
	QJsonArray arguments;
	JsonHandler handler;
};

using ResourceCompletion = std::function<void(QJsonArray)>;
using ResourceHandler = std::function<void(ResourceCompletion)>;

struct Resource {
	QString uri;
	QString name;
	QString title;
	QString description;
	QString mimeType;
	ResourceHandler handler;
};

using ResourceTemplateHandler = std::function<void(
	const QString&,
	ResourceCompletion)>;

struct ResourceTemplate {
	QString uriTemplate;
	QString name;
	QString title;
	QString description;
	QString mimeType;
	ResourceTemplateHandler handler;
};

class Dispatcher final {
public:
	using Completion = std::function<void(QJsonObject)>;

	Dispatcher(QString name, QString version);

	void addTool(Tool tool);
	void addResource(Resource resource);
	void addResourceTemplate(ResourceTemplate resourceTemplate);
	void addPrompt(Prompt prompt);
	void setCompletionHandler(JsonHandler handler);
	CancellationPtr handle(
		const Request &request,
		Completion done,
		CancellationPtr cancellation = nullptr) const;

private:
	[[nodiscard]] QJsonObject listTools(const Request &request) const;
	void callTool(
		const Request &request,
		const CancellationPtr &cancellation,
		Completion done) const;
	[[nodiscard]] QJsonObject listResources(const Request &request) const;
	[[nodiscard]] QJsonObject listResourceTemplates(
		const Request &request) const;
	void readResource(const Request &request, Completion done) const;
	[[nodiscard]] QJsonObject listPrompts(const Request &request) const;
	void getPrompt(const Request &request, Completion done) const;
	void complete(const Request &request, Completion done) const;

	QString _name;
	QString _version;
	std::vector<Tool> _tools;
	std::vector<Resource> _resources;
	std::vector<ResourceTemplate> _resourceTemplates;
	std::vector<Prompt> _prompts;
	JsonHandler _completionHandler;

};

} // namespace Core::Mcp
