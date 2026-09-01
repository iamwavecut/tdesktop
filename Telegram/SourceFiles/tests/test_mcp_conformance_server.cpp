/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mcp/mcp_dispatcher.h"
#include "core/mcp/mcp_http_server.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>

#include <cstdio>

namespace {

using Core::Mcp::CancellationPtr;
using Core::Mcp::Dispatcher;
using Core::Mcp::ToolCompletion;
using Core::Mcp::ToolResult;

[[nodiscard]] QJsonObject EmptySchema() {
	return {
		{ u"type"_q, u"object"_q },
		{ u"additionalProperties"_q, false },
	};
}

[[nodiscard]] QJsonObject ObjectSchema() {
	return { { u"type"_q, u"object"_q } };
}

[[nodiscard]] QJsonObject TextContent(const QString &text) {
	return {
		{ u"type"_q, u"text"_q },
		{ u"text"_q, text },
	};
}

[[nodiscard]] QJsonObject InputRequest(
		const QString &method,
		QJsonObject params) {
	return {
		{ u"method"_q, method },
		{ u"params"_q, std::move(params) },
	};
}

[[nodiscard]] QJsonObject ElicitationRequest(
		const QString &message,
		const QString &property,
		const QString &type) {
	return InputRequest(u"elicitation/create"_q, {
		{ u"message"_q, message },
		{ u"requestedSchema"_q, QJsonObject{
			{ u"type"_q, u"object"_q },
			{ u"properties"_q, QJsonObject{
				{ property, QJsonObject{ { u"type"_q, type } } },
			} },
			{ u"required"_q, QJsonArray{ property } },
		} },
	});
}

[[nodiscard]] QJsonObject SamplingRequest(const QString &text) {
	return InputRequest(u"sampling/createMessage"_q, {
		{ u"messages"_q, QJsonArray{ QJsonObject{
			{ u"role"_q, u"user"_q },
			{ u"content"_q, TextContent(text) },
		} } },
		{ u"maxTokens"_q, 100 },
	});
}

[[nodiscard]] ToolResult CompleteText(QString text) {
	return {
		.content = QJsonArray{ TextContent(text) },
	};
}

[[nodiscard]] ToolResult InputRequired(
		QJsonObject requests,
		QString state = {}) {
	auto result = QJsonObject{
		{ u"resultType"_q, u"input_required"_q },
		{ u"inputRequests"_q, std::move(requests) },
	};
	if (!state.isEmpty()) {
		result.insert(u"requestState"_q, std::move(state));
	}
	return { .result = std::move(result) };
}

void AddContentTool(
		Dispatcher &dispatcher,
		QString name,
		QJsonArray content,
		bool isError = false) {
	dispatcher.addTool({
		.name = std::move(name),
		.description = u"MCP conformance fixture tool."_q,
		.inputSchema = EmptySchema(),
		.outputSchema = ObjectSchema(),
		.handler = [content = std::move(content), isError](
				const QJsonObject &,
				ToolCompletion done) {
			done({
				.content = content,
				.isError = isError,
			});
		},
	});
}

void RegisterContentTools(Dispatcher &dispatcher) {
	const auto image = QJsonObject{
		{ u"type"_q, u"image"_q },
		{ u"data"_q,
			u"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="_q },
		{ u"mimeType"_q, u"image/png"_q },
	};
	const auto audio = QJsonObject{
		{ u"type"_q, u"audio"_q },
		{ u"data"_q, u"UklGRg=="_q },
		{ u"mimeType"_q, u"audio/wav"_q },
	};
	const auto resource = QJsonObject{
		{ u"type"_q, u"resource"_q },
		{ u"resource"_q, QJsonObject{
			{ u"uri"_q, u"test://embedded-resource"_q },
			{ u"mimeType"_q, u"application/json"_q },
			{ u"text"_q, u"{\"test\":true}"_q },
		} },
	};
	AddContentTool(dispatcher, u"test_simple_text"_q, {
		TextContent(u"This is a simple text response for testing."_q),
	});
	AddContentTool(dispatcher, u"test_image_content"_q, { image });
	AddContentTool(dispatcher, u"test_audio_content"_q, { audio });
	AddContentTool(dispatcher, u"test_embedded_resource"_q, { resource });
	AddContentTool(dispatcher, u"test_multiple_content_types"_q, {
		TextContent(u"Multiple content types test:"_q),
		image,
		resource,
	});
	AddContentTool(dispatcher, u"test_error_handling"_q, {
		TextContent(u"This tool intentionally returns an error for testing"_q),
	}, true);
	AddContentTool(dispatcher, u"test_tool_with_progress"_q, {
		TextContent(u"Progress completed."_q),
	});
	AddContentTool(dispatcher, u"test_streaming_elicitation"_q, {
		TextContent(u"No independent server request was emitted."_q),
	});
	AddContentTool(dispatcher, u"test_logging_tool"_q, {
		TextContent(u"No log message was emitted."_q),
	});
	dispatcher.addTool({
		.name = u"test_missing_capability"_q,
		.description = u"Requires client sampling support."_q,
		.inputSchema = EmptySchema(),
		.outputSchema = ObjectSchema(),
		.handler = [](const QJsonObject &, ToolCompletion done) {
			done(CompleteText(u"sampling available"_q));
		},
		.requiredClientCapability = u"sampling"_q,
	});
}

void RegisterResources(Dispatcher &dispatcher) {
	dispatcher.addResource({
		.uri = u"test://static-text"_q,
		.name = u"static-text"_q,
		.title = u"Static text"_q,
		.description = u"Conformance text resource."_q,
		.mimeType = u"text/plain"_q,
		.handler = [](auto done) {
			done({ QJsonObject{
				{ u"uri"_q, u"test://static-text"_q },
				{ u"mimeType"_q, u"text/plain"_q },
				{ u"text"_q,
					u"This is the content of the static text resource."_q },
			} });
		},
	});
	dispatcher.addResource({
		.uri = u"test://static-binary"_q,
		.name = u"static-binary"_q,
		.title = u"Static binary"_q,
		.description = u"Conformance binary resource."_q,
		.mimeType = u"image/png"_q,
		.handler = [](auto done) {
			done({ QJsonObject{
				{ u"uri"_q, u"test://static-binary"_q },
				{ u"mimeType"_q, u"image/png"_q },
				{ u"blob"_q,
					u"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="_q },
			} });
		},
	});
	dispatcher.addResourceTemplate({
		.uriTemplate = u"test://template/{id}/data"_q,
		.name = u"template-data"_q,
		.title = u"Template data"_q,
		.description = u"Conformance template resource."_q,
		.mimeType = u"text/plain"_q,
		.handler = [](const QString &uri, auto done) {
			done({ QJsonObject{
				{ u"uri"_q, uri },
				{ u"mimeType"_q, u"text/plain"_q },
				{ u"text"_q, u"Template content for "_q + uri },
			} });
		},
	});
}

void RegisterPrompts(Dispatcher &dispatcher) {
	const auto argument = [](const QString &name, const QString &description) {
		return QJsonObject{
			{ u"name"_q, name },
			{ u"description"_q, description },
			{ u"required"_q, true },
		};
	};
	dispatcher.addPrompt({
		.name = u"test_simple_prompt"_q,
		.description = u"Simple conformance prompt."_q,
		.handler = [](const QJsonObject &, auto done) {
			done({ { u"messages"_q, QJsonArray{ QJsonObject{
				{ u"role"_q, u"user"_q },
				{ u"content"_q, TextContent(
					u"This is a simple prompt for testing."_q) },
			} } } });
		},
	});
	dispatcher.addPrompt({
		.name = u"test_prompt_with_arguments"_q,
		.description = u"Parameterized conformance prompt."_q,
		.arguments = {
			argument(u"arg1"_q, u"First test argument."_q),
			argument(u"arg2"_q, u"Second test argument."_q),
		},
		.handler = [](const QJsonObject &params, auto done) {
			const auto values = params.value(u"arguments"_q).toObject();
			done({ { u"messages"_q, QJsonArray{ QJsonObject{
				{ u"role"_q, u"user"_q },
				{ u"content"_q, TextContent(
					u"Prompt with arguments: arg1='%1', arg2='%2'"_q
						.arg(values.value(u"arg1"_q).toString())
						.arg(values.value(u"arg2"_q).toString())) },
			} } } });
		},
	});
	dispatcher.addPrompt({
		.name = u"test_prompt_with_embedded_resource"_q,
		.description = u"Embedded resource conformance prompt."_q,
		.arguments = {
			argument(u"resourceUri"_q, u"Resource URI."_q),
		},
		.handler = [](const QJsonObject &params, auto done) {
			const auto uri = params.value(u"arguments"_q).toObject().value(
				u"resourceUri"_q).toString();
			done({ { u"messages"_q, QJsonArray{
				QJsonObject{
					{ u"role"_q, u"user"_q },
					{ u"content"_q, QJsonObject{
						{ u"type"_q, u"resource"_q },
						{ u"resource"_q, QJsonObject{
							{ u"uri"_q, uri },
							{ u"mimeType"_q, u"text/plain"_q },
							{ u"text"_q,
								u"Embedded resource content for testing."_q },
						} },
					} },
				},
				QJsonObject{
					{ u"role"_q, u"user"_q },
					{ u"content"_q, TextContent(
						u"Please process the embedded resource above."_q) },
				},
			} } });
		},
	});
	dispatcher.addPrompt({
		.name = u"test_prompt_with_image"_q,
		.description = u"Image conformance prompt."_q,
		.handler = [](const QJsonObject &, auto done) {
			done({ { u"messages"_q, QJsonArray{
				QJsonObject{
					{ u"role"_q, u"user"_q },
					{ u"content"_q, QJsonObject{
						{ u"type"_q, u"image"_q },
						{ u"data"_q,
							u"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="_q },
						{ u"mimeType"_q, u"image/png"_q },
					} },
				},
				QJsonObject{
					{ u"role"_q, u"user"_q },
					{ u"content"_q,
						TextContent(u"Please analyze the image above."_q) },
				},
			} } });
		},
	});
	dispatcher.addPrompt({
		.name = u"test_input_required_result_prompt"_q,
		.description = u"Input-required conformance prompt."_q,
		.handler = [](const QJsonObject &params, auto done) {
			if (!params.value(u"inputResponses"_q).toObject().isEmpty()) {
				done({ { u"messages"_q, QJsonArray{ QJsonObject{
					{ u"role"_q, u"user"_q },
					{ u"content"_q, TextContent(
						u"Prompt with supplied context."_q) },
				} } } });
			} else {
				done({
					{ u"resultType"_q, u"input_required"_q },
					{ u"inputRequests"_q, QJsonObject{
						{ u"user_context"_q, ElicitationRequest(
							u"What context should the prompt use?"_q,
							u"context"_q,
							u"string"_q) },
					} },
				});
			}
		},
	});
}

void RegisterInputRequiredTools(Dispatcher &dispatcher) {
	const auto add = [&](QString name, Core::Mcp::AdvancedToolHandler handler) {
		dispatcher.addTool({
			.name = std::move(name),
			.description = u"InputRequiredResult conformance fixture."_q,
			.inputSchema = EmptySchema(),
			.outputSchema = ObjectSchema(),
			.advancedHandler = std::move(handler),
		});
	};
	add(u"test_input_required_result_elicitation"_q, [](
			const QJsonObject &params,
			const CancellationPtr &,
			ToolCompletion done) {
		if (params.value(u"inputResponses"_q).toObject().contains(
				u"user_name"_q)) {
			done(CompleteText(u"Hello, Alice!"_q));
		} else {
			done(InputRequired({
				{ u"user_name"_q, ElicitationRequest(
					u"What is your name?"_q,
					u"name"_q,
					u"string"_q) },
			}));
		}
	});
	add(u"test_input_required_result_sampling"_q, [](
			const QJsonObject &params,
			const CancellationPtr &,
			ToolCompletion done) {
		if (params.value(u"inputResponses"_q).toObject().contains(
				u"capital_question"_q)) {
			done(CompleteText(u"The capital of France is Paris."_q));
		} else {
			done(InputRequired({
				{ u"capital_question"_q, SamplingRequest(
					u"What is the capital of France?"_q) },
			}));
		}
	});
	add(u"test_input_required_result_list_roots"_q, [](
			const QJsonObject &params,
			const CancellationPtr &,
			ToolCompletion done) {
		if (params.value(u"inputResponses"_q).toObject().contains(
				u"client_roots"_q)) {
			done(CompleteText(u"Client roots received."_q));
		} else {
			done(InputRequired({
				{ u"client_roots"_q, InputRequest(
					u"roots/list"_q,
					{}) },
			}));
		}
	});
	add(u"test_input_required_result_request_state"_q, [](
			const QJsonObject &params,
			const CancellationPtr &,
			ToolCompletion done) {
		if (params.value(u"requestState"_q) == u"request-state-ok"_q
			&& !params.value(u"inputResponses"_q).toObject().isEmpty()) {
			done(CompleteText(u"state-ok"_q));
		} else {
			done(InputRequired({
				{ u"confirm"_q, ElicitationRequest(
					u"Please confirm"_q,
					u"ok"_q,
					u"boolean"_q) },
			}, u"request-state-ok"_q));
		}
	});
	add(u"test_input_required_result_multiple_inputs"_q, [](
			const QJsonObject &params,
			const CancellationPtr &,
			ToolCompletion done) {
		if (params.value(u"requestState"_q) == u"multiple-state"_q
			&& params.value(u"inputResponses"_q).toObject().size() >= 3) {
			done(CompleteText(u"All inputs received."_q));
		} else {
			done(InputRequired({
				{ u"user_name"_q, ElicitationRequest(
					u"What is your name?"_q,
					u"name"_q,
					u"string"_q) },
				{ u"greeting"_q, SamplingRequest(u"Generate a greeting"_q) },
				{ u"client_roots"_q, InputRequest(u"roots/list"_q, {}) },
			}, u"multiple-state"_q));
		}
	});
	add(u"test_input_required_result_multi_round"_q, [](
			const QJsonObject &params,
			const CancellationPtr &,
			ToolCompletion done) {
		const auto state = params.value(u"requestState"_q).toString();
		if (state == u"round-2"_q) {
			done(CompleteText(u"Multi-round complete."_q));
		} else if (state == u"round-1"_q) {
			done(InputRequired({
				{ u"step2"_q, ElicitationRequest(
					u"Step 2: What is your favorite color?"_q,
					u"color"_q,
					u"string"_q) },
			}, u"round-2"_q));
		} else {
			done(InputRequired({
				{ u"step1"_q, ElicitationRequest(
					u"Step 1: What is your name?"_q,
					u"name"_q,
					u"string"_q) },
			}, u"round-1"_q));
		}
	});
	add(u"test_input_required_result_tampered_state"_q, [](
			const QJsonObject &params,
			const CancellationPtr &,
			ToolCompletion done) {
		if (params.value(u"inputResponses"_q).toObject().isEmpty()) {
			done(InputRequired({
				{ u"confirm"_q, ElicitationRequest(
					u"Please confirm"_q,
					u"ok"_q,
					u"boolean"_q) },
			}, u"signed-state"_q));
		} else if (params.value(u"requestState"_q) != u"signed-state"_q) {
			done({
				.errorCode = -32602,
				.errorMessage = u"Invalid requestState signature"_q,
			});
		} else {
			done(CompleteText(u"State accepted."_q));
		}
	});
	add(u"test_input_required_result_capabilities"_q, [](
			const QJsonObject &params,
			const CancellationPtr &,
			ToolCompletion done) {
		const auto capabilities = params.value(u"_meta"_q).toObject().value(
			u"io.modelcontextprotocol/clientCapabilities"_q).toObject();
		auto requests = QJsonObject();
		if (capabilities.contains(u"sampling"_q)) {
			requests.insert(
				u"sampling"_q,
				SamplingRequest(u"Generate a capability response."_q));
		}
		if (capabilities.contains(u"elicitation"_q)) {
			requests.insert(
				u"elicitation"_q,
				ElicitationRequest(
					u"Provide a capability response."_q,
					u"value"_q,
					u"string"_q));
		}
		done(InputRequired(std::move(requests)));
	});
}

} // namespace

int main(int argc, char *argv[]) {
	const auto app = QCoreApplication(argc, argv);
	auto dispatcher = Dispatcher(u"Forkgram conformance fixture"_q, u"1.0"_q);
	RegisterContentTools(dispatcher);
	RegisterResources(dispatcher);
	RegisterPrompts(dispatcher);
	RegisterInputRequiredTools(dispatcher);
	dispatcher.setCompletionHandler([](const QJsonObject &, auto done) {
		done({ { u"completion"_q, QJsonObject{
			{ u"values"_q, QJsonArray() },
			{ u"total"_q, 0 },
			{ u"hasMore"_q, false },
		} } });
	});
	auto portOk = false;
	const auto requested = (argc > 1)
		? QString::fromLocal8Bit(argv[1]).toInt(&portOk)
		: 0;
	if (argc > 1 && (!portOk || requested < 1024 || requested > 65535)) {
		return 2;
	}
	auto server = Core::Mcp::HttpServer(&dispatcher);
	if (!server.listen(requested)) {
		return 3;
	}
	std::fprintf(stdout, "http://127.0.0.1:%d/mcp\n", int(server.port()));
	std::fflush(stdout);
	return app.exec();
}
