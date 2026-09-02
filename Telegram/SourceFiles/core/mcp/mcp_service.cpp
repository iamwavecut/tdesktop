/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mcp/mcp_service.h"

#include "api/api_common.h"
#include "api/api_editing.h"
#include "apiwrap.h"
#include "base/random.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/version.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_document.h"
#include "data/data_drafts.h"
#include "data/data_file_origin.h"
#include "data/data_histories.h"
#include "data/data_media_types.h"
#include "data/data_message_reaction_id.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "data/data_user.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/details/mtproto_serialized_request.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_response.h"
#include "storage/file_upload.h"
#include "storage/localimageloader.h"
#include "ui/chat/attach/attach_prepare.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include "mcp_schema.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

#include <algorithm>
#include <cstring>

namespace Core::Mcp {
namespace {

constexpr auto kPortKey = std::string_view("mcp.port");
constexpr auto kEnabledKey = std::string_view("mcp.enabled");
constexpr auto kAuthEnabledKey = std::string_view("mcp.auth.enabled");
constexpr auto kAuthTokenKey = std::string_view("mcp.auth.token");
constexpr auto kDisabledToolsKey = std::string_view("mcp.tools.disabled");
constexpr auto kMinPort = 1024;
constexpr auto kSessionWatchInterval = crl::time(100);

[[nodiscard]] QByteArray GenerateBearerToken() {
	auto result = QByteArray(32, '\0');
	base::RandomFill(result.data(), result.size());
	return result;
}

[[nodiscard]] QJsonObject EmptyObjectSchema() {
	return {
		{ u"type"_q, u"object"_q },
		{ u"additionalProperties"_q, false },
	};
}

[[nodiscard]] QJsonObject ObjectSchema() {
	return { { u"type"_q, u"object"_q } };
}

[[nodiscard]] QJsonObject StateSchema() {
	return {
		{ u"type"_q, u"object"_q },
		{ u"properties"_q, QJsonObject{
			{ u"launched"_q, QJsonObject{ { u"type"_q, u"boolean"_q } } },
			{ u"locked"_q, QJsonObject{ { u"type"_q, u"boolean"_q } } },
			{ u"authenticated"_q,
				QJsonObject{ { u"type"_q, u"boolean"_q } } },
			{ u"mcp_available"_q,
				QJsonObject{ { u"type"_q, u"boolean"_q } } },
			{ u"mcp_enabled"_q,
				QJsonObject{ { u"type"_q, u"boolean"_q } } },
			{ u"authentication_required"_q,
				QJsonObject{ { u"type"_q, u"boolean"_q } } },
			{ u"enabled_tool_count"_q,
				QJsonObject{ { u"type"_q, u"integer"_q } } },
			{ u"total_tool_count"_q,
				QJsonObject{ { u"type"_q, u"integer"_q } } },
			{ u"account_id"_q, QJsonObject{
				{ u"type"_q, QJsonArray{ u"string"_q, u"null"_q } },
			} },
			{ u"endpoint"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
		} },
		{ u"required"_q, QJsonArray{
			u"launched"_q,
			u"locked"_q,
			u"authenticated"_q,
			u"mcp_available"_q,
			u"mcp_enabled"_q,
			u"authentication_required"_q,
			u"enabled_tool_count"_q,
			u"total_tool_count"_q,
			u"account_id"_q,
			u"endpoint"_q,
		} },
		{ u"additionalProperties"_q, false },
	};
}

[[nodiscard]] ToolResult ToolError(QString type, QString description) {
	return {
		.structuredContent = QJsonObject{
			{ u"error"_q, QJsonObject{
				{ u"type"_q, std::move(type) },
				{ u"description"_q, std::move(description) },
			} },
		},
		.isError = true,
	};
}

[[nodiscard]] QJsonObject StringArgumentSchema(
		const QString &name,
		const QString &description) {
	return {
		{ u"type"_q, u"object"_q },
		{ u"properties"_q, QJsonObject{
			{ name, QJsonObject{
				{ u"type"_q, u"string"_q },
				{ u"description"_q, description },
			} },
		} },
		{ u"required"_q, QJsonArray{ name } },
		{ u"additionalProperties"_q, false },
	};
}

[[nodiscard]] Main::Session *RequireSession(
		not_null<Application*> application,
		const ToolCompletion &done) {
	if (application->passcodeLocked()) {
		done(ToolError(u"APP_LOCKED"_q, u"Forkgram is locked."_q));
		return nullptr;
	}
	const auto session = application->maybePrimarySession();
	if (!session) {
		done(ToolError(
			u"SESSION_GONE"_q,
			u"Forkgram has no active authenticated session."_q));
	}
	return session;
}

[[nodiscard]] PeerData *ResolveLoadedPeer(
		not_null<Main::Session*> session,
		const QJsonObject &arguments) {
	auto ok = false;
	const auto raw = arguments.value(u"peer_id"_q).toString().toULongLong(&ok);
	return (ok && raw)
		? session->data().peerLoaded(PeerId(raw))
		: nullptr;
}

[[nodiscard]] QJsonObject PeerJson(
		not_null<PeerData*> peer,
		History *history = nullptr) {
	return {
		{ u"peer_id"_q, QString::number(peer->id.value) },
		{ u"name"_q, peer->name() },
		{ u"username"_q, peer->username() },
		{ u"kind"_q, peer->isUser()
			? u"user"_q
			: peer->isChat()
			? u"group"_q
			: peer->isChannel()
			? u"channel"_q
			: u"unknown"_q },
		{ u"self"_q, peer->isSelf() },
		{ u"unread_count"_q, history ? history->unreadCount() : 0 },
	};
}

[[nodiscard]] QJsonObject MessageJson(not_null<HistoryItem*> item) {
	return {
		{ u"account_id"_q,
			QString::number(item->history()->session().uniqueId()) },
		{ u"peer_id"_q, QString::number(item->history()->peer->id.value) },
		{ u"message_id"_q, QString::number(item->id.bare) },
		{ u"date"_q, int(item->date()) },
		{ u"outgoing"_q, item->out() },
		{ u"pinned"_q, item->isPinned() },
		{ u"author_peer_id"_q, QString::number(item->from()->id.value) },
		{ u"text"_q, item->originalText().text },
		{ u"has_media"_q, (item->media() != nullptr) },
	};
}

[[nodiscard]] QJsonObject PeerInputSchema(bool withLimit = false) {
	auto properties = QJsonObject{
		{ u"peer_id"_q, QJsonObject{
			{ u"type"_q, u"string"_q },
			{ u"description"_q, u"Internal peer identifier as a decimal string."_q },
		} },
	};
	if (withLimit) {
		properties.insert(u"limit"_q, QJsonObject{
			{ u"type"_q, u"integer"_q },
			{ u"minimum"_q, 1 },
			{ u"maximum"_q, 200 },
		});
	}
	return {
		{ u"type"_q, u"object"_q },
		{ u"properties"_q, std::move(properties) },
		{ u"required"_q, QJsonArray{ u"peer_id"_q } },
		{ u"additionalProperties"_q, false },
	};
}

[[nodiscard]] QJsonObject MessageInputSchema(bool withText = false) {
	auto schema = PeerInputSchema();
	auto properties = schema.value(u"properties"_q).toObject();
	properties.insert(u"message_id"_q, QJsonObject{
		{ u"type"_q, u"string"_q },
	});
	auto required = schema.value(u"required"_q).toArray();
	required.append(u"message_id"_q);
	if (withText) {
		properties.insert(u"text"_q, QJsonObject{
			{ u"type"_q, u"string"_q },
		});
		required.append(u"text"_q);
	}
	schema.insert(u"properties"_q, std::move(properties));
	schema.insert(u"required"_q, std::move(required));
	return schema;
}

[[nodiscard]] MTP::details::SerializedRequest SerializedRequest(
		const QByteArray &bytes) {
	Expects(!bytes.isEmpty() && !(bytes.size() % sizeof(mtpPrime)));

	const auto count = uint32(bytes.size() / sizeof(mtpPrime));
	auto result = MTP::details::SerializedRequest::Prepare(count);
	result->resize(MTP::details::SerializedRequest::kMessageBodyPosition + count);
	std::memcpy(
		result->data() + MTP::details::SerializedRequest::kMessageBodyPosition,
		bytes.constData(),
		bytes.size());
	return result;
}

[[nodiscard]] ToolResult RpcToolError(const MTP::Error &error) {
	auto floodWait = 0;
	if (MTP::IsFloodError(error)) {
		const auto suffix = error.type().lastIndexOf('_');
		if (suffix >= 0) {
			floodWait = error.type().mid(suffix + 1).toInt();
		}
	}
	return {
		.structuredContent = QJsonObject{
			{ u"error"_q, QJsonObject{
				{ u"code"_q, error.code() },
				{ u"type"_q, error.type() },
				{ u"description"_q, error.description() },
				{ u"temporary"_q, MTP::IsTemporaryError(error) },
				{ u"flood_wait_seconds"_q, floodWait },
			} },
		},
		.isError = true,
	};
}

struct RawCallState {
	ToolCompletion done;
	Fn<void()> cancel;
	mtpRequestId requestId = 0;
	bool finished = false;
};

struct MtpToolState {
	ToolCompletion done;
	Fn<void()> cancel;
	mtpRequestId requestId = 0;
	bool finished = false;
};

template <typename State>
void WatchSessionGone(
		not_null<Application*> application,
		base::weak_ptr<Main::Session> session,
		std::shared_ptr<State> state) {
	QTimer::singleShot(kSessionWatchInterval, application, [=] {
		if (state->finished) {
			return;
		} else if (!session) {
			state->finished = true;
			if (state->cancel) {
				state->cancel();
			}
			if (state->done) {
				state->done(ToolError(
					u"SESSION_GONE"_q,
					u"The captured Telegram session ended."_q));
			}
			return;
		} else if (application->passcodeLocked()) {
			state->finished = true;
			if (state->cancel) {
				state->cancel();
			}
			if (state->done) {
				state->done(ToolError(
					u"APP_LOCKED"_q,
					u"Forkgram was locked during the request."_q));
			}
			return;
		}
		WatchSessionGone(application, session, std::move(state));
	});
}

template <typename State>
void SetApiCancellation(
		base::weak_ptr<Main::Session> session,
		const std::shared_ptr<State> &state) {
	const auto requestId = state->requestId;
	state->cancel = [session, requestId] {
		if (const auto current = session.get()) {
			current->api().request(requestId).cancel();
		}
	};
}

[[nodiscard]] HistoryItem *ResolveLoadedMessage(
		not_null<Main::Session*> session,
		const QJsonObject &arguments) {
	const auto peer = ResolveLoadedPeer(session, arguments);
	auto idOk = false;
	const auto messageId = MsgId(arguments.value(
		u"message_id"_q).toString().toLongLong(&idOk));
	return (peer && idOk && messageId.bare != 0)
		? session->data().message(peer, messageId)
		: nullptr;
}

[[nodiscard]] DocumentData *ResolveLoadedDocument(
		not_null<Main::Session*> session,
		const QJsonObject &arguments,
		HistoryItem **item = nullptr) {
	const auto result = ResolveLoadedMessage(session, arguments);
	if (item) {
		*item = result;
	}
	return (result && result->media())
		? result->media()->document()
		: nullptr;
}

[[nodiscard]] QJsonObject DocumentJson(
		not_null<Main::Session*> session,
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document) {
	return {
		{ u"account_id"_q, QString::number(session->uniqueId()) },
		{ u"peer_id"_q, QString::number(item->history()->peer->id.value) },
		{ u"message_id"_q, QString::number(item->id.bare) },
		{ u"document_id"_q, QString::number(document->id) },
		{ u"filename"_q, document->filename() },
		{ u"size"_q, QString::number(document->size) },
		{ u"offset"_q, QString::number(document->loadOffset()) },
		{ u"progress"_q, document->progress() },
		{ u"loading"_q, document->loading() },
		{ u"uploading"_q, document->uploading() },
		{ u"cancelled"_q, document->cancelled() },
		{ u"path"_q, document->filepath(true) },
	};
}

} // namespace

Service::Service(not_null<Application*> application)
: _application(application)
, _dispatcher(u"Forkgram"_q, QString::fromLatin1(AppVersionStr))
, _tlRegistry(GeneratedSchemaJson())
, _tlCodec(&_tlRegistry)
, _uiDriver(application, &_dispatcher)
, _server(&_dispatcher) {
	registerTools();
	_dispatcher.setToolFilter([=](const QString &name) {
		return !_accessPolicy || _accessPolicy->toolEnabled(name);
	});
	watchUpdates();
}

bool Service::start() {
	auto savedOk = false;
	const auto savedData = _application->settings().readPref<QByteArray>(
		kPortKey);
	auto saved = savedData.toInt(&savedOk);
	if (!savedOk || (saved && (saved < kMinPort || saved > 65535))) {
		saved = 0;
	}
	_configuredPort = saved;
	const auto defaults = DefaultAccessPolicyState(saved > 0);
	_accessPolicy = std::make_unique<AccessPolicy>(
		_dispatcher.toolCatalog(),
		AccessPolicyState{
			.enabled = _application->settings().readPref<bool>(
				kEnabledKey,
				defaults.enabled),
			.authenticationEnabled = _application->settings().readPref<bool>(
				kAuthEnabledKey,
				defaults.authenticationEnabled),
			.bearerToken = _application->settings().readPref<QByteArray>(
				kAuthTokenKey),
			.disabledTools = _application->settings().readPref<QByteArray>(
				kDisabledToolsKey),
		});
	_application->settings().writePref<bool>(
		kEnabledKey,
		_accessPolicy->enabled());
	_application->settings().writePref<bool>(
		kAuthEnabledKey,
		_accessPolicy->authenticationEnabled());
	if (_accessPolicy->authenticationEnabled()
		&& _accessPolicy->bearerToken().size() != 32) {
		_accessPolicy->setBearerToken(GenerateBearerToken());
		_application->settings().writePref<QByteArray>(
			kAuthTokenKey,
			_accessPolicy->bearerToken());
	}
	_application->saveSettings();
	_server.setBearerToken(_accessPolicy->authenticationEnabled()
		? _accessPolicy->bearerToken()
		: QByteArray());
	if (!_accessPolicy->enabled()) {
		return true;
	}
	if (!_server.listen(saved)) {
		return false;
	}
	_configuredPort = _server.port();
	if (!saved) {
		_application->settings().writePref<QByteArray>(
			kPortKey,
			QByteArray::number(_configuredPort));
		_application->saveSettings();
	}
	return true;
}

bool Service::enabled() const {
	return _accessPolicy && _accessPolicy->enabled();
}

bool Service::setEnabled(bool enabled) {
	Expects(_accessPolicy != nullptr);

	if (!enabled) {
		_server.stop();
		if (_accessPolicy->setEnabled(false)) {
			_application->settings().writePref<bool>(kEnabledKey, false);
			_application->saveSettings();
			notifyConfigurationChanged();
		}
		return true;
	} else if (available()) {
		return true;
	}
	_server.setBearerToken(authenticationEnabled()
		? _accessPolicy->bearerToken()
		: QByteArray());
	if (!_server.listen(_configuredPort)) {
		return false;
	}
	_configuredPort = _server.port();
	_application->settings().writePref<QByteArray>(
		kPortKey,
		QByteArray::number(_configuredPort));
	if (_accessPolicy->setEnabled(true)) {
		_application->settings().writePref<bool>(kEnabledKey, true);
	}
	_application->saveSettings();
	notifyConfigurationChanged();
	return true;
}

bool Service::rebind(quint16 port) {
	if (port < kMinPort) {
		return false;
	}
	if (port == _configuredPort && (!enabled() || available())) {
		return true;
	}
	if (enabled() && !_server.listen(port)) {
		return false;
	}
	_configuredPort = port;
	_application->settings().writePref<QByteArray>(
		kPortKey,
		QByteArray::number(port));
	_application->saveSettings();
	notifyConfigurationChanged();
	return true;
}

bool Service::authenticationEnabled() const {
	return _accessPolicy && _accessPolicy->authenticationEnabled();
}

void Service::setAuthenticationEnabled(bool enabled) {
	Expects(_accessPolicy != nullptr);

	if (!_accessPolicy->setAuthenticationEnabled(enabled)) {
		return;
	}
	if (enabled && _accessPolicy->bearerToken().size() != 32) {
		_accessPolicy->setBearerToken(GenerateBearerToken());
		_application->settings().writePref<QByteArray>(
			kAuthTokenKey,
			_accessPolicy->bearerToken());
	}
	_server.setBearerToken(enabled
		? _accessPolicy->bearerToken()
		: QByteArray());
	_application->settings().writePref<bool>(kAuthEnabledKey, enabled);
	_application->saveSettings();
	notifyConfigurationChanged();
}

QString Service::bearerTokenForCopy() const {
	return _accessPolicy
		? QString::fromLatin1(_accessPolicy->bearerToken().toBase64(
			QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))
		: QString();
}

QString Service::regenerateBearerToken() {
	Expects(_accessPolicy != nullptr);

	auto token = GenerateBearerToken();
	while (token == _accessPolicy->bearerToken()) {
		token = GenerateBearerToken();
	}
	_accessPolicy->setBearerToken(token);
	_application->settings().writePref<QByteArray>(kAuthTokenKey, token);
	_server.setBearerToken(authenticationEnabled() ? token : QByteArray());
	_application->saveSettings();
	notifyConfigurationChanged();
	return bearerTokenForCopy();
}

const std::vector<ToolInfo> &Service::tools() const {
	Expects(_accessPolicy != nullptr);

	return _accessPolicy->tools();
}

bool Service::toolEnabled(const QString &name) const {
	return _accessPolicy && _accessPolicy->toolEnabled(name);
}

int Service::enabledToolCount() const {
	return _accessPolicy ? _accessPolicy->enabledToolCount() : 0;
}

int Service::totalToolCount() const {
	return _accessPolicy ? int(_accessPolicy->tools().size()) : 0;
}

ToolCategoryState Service::categoryState(const QString &category) const {
	return _accessPolicy
		? _accessPolicy->categoryState(category)
		: ToolCategoryState::None;
}

void Service::setToolEnabled(const QString &name, bool enabled) {
	Expects(_accessPolicy != nullptr);

	if (_accessPolicy->setToolEnabled(name, enabled)) {
		saveToolPolicy();
	}
}

void Service::setCategoryEnabled(const QString &category, bool enabled) {
	Expects(_accessPolicy != nullptr);

	if (_accessPolicy->setCategoryEnabled(category, enabled)) {
		saveToolPolicy();
	}
}

rpl::producer<> Service::configurationChanges() const {
	return _configurationChanges.events();
}

void Service::saveToolPolicy() {
	_application->settings().writePref<QByteArray>(
		kDisabledToolsKey,
		_accessPolicy->serializedDisabledTools());
	_application->saveSettings();
	_server.publish(u"notifications/tools/list_changed"_q, {});
	notifyConfigurationChanged();
}

void Service::notifyConfigurationChanged() {
	_configurationChanges.fire({});
}

bool Service::available() const {
	return _server.port() != 0;
}

quint16 Service::port() const {
	return _server.port();
}

quint16 Service::configuredPort() const {
	return _configuredPort;
}

QString Service::endpoint() const {
	return _configuredPort
		? u"http://127.0.0.1:%1/mcp"_q.arg(_configuredPort)
		: QString();
}

QString Service::errorString() const {
	return _server.errorString();
}

void Service::registerTools() {
	_dispatcher.addTool({
		.name = u"telegram.client.state"_q,
		.description = u"Return the current Forkgram and active account state."_q,
		.inputSchema = EmptyObjectSchema(),
		.outputSchema = StateSchema(),
		.handler = [=](const QJsonObject &, ToolCompletion done) {
			done(clientState());
		},
	});
	_dispatcher.addResource({
		.uri = u"telegram://state"_q,
		.name = u"state"_q,
		.title = u"Forkgram state"_q,
		.description = u"Current Forkgram and active account state."_q,
		.mimeType = u"application/json"_q,
		.handler = [=](ResourceCompletion done) {
			const auto state = clientState().structuredContent.toObject();
			done({ QJsonObject{
				{ u"uri"_q, u"telegram://state"_q },
				{ u"mimeType"_q, u"application/json"_q },
				{ u"text"_q, QString::fromUtf8(QJsonDocument(state).toJson(
					QJsonDocument::Compact)) },
			} });
		},
	});
	const auto updateResource = [=](
			const QString &uri,
			quint64 cursor,
			ResourceCompletion done) {
		const auto session = _application->maybePrimarySession();
		auto value = QJsonObject();
		if (_application->passcodeLocked()) {
			value.insert(u"error"_q, QJsonObject{
				{ u"type"_q, u"APP_LOCKED"_q },
				{ u"description"_q, u"Forkgram is locked."_q },
			});
		} else if (!session) {
			value.insert(u"error"_q, QJsonObject{
				{ u"type"_q, u"SESSION_GONE"_q },
				{ u"description"_q,
					u"Forkgram has no active authenticated session."_q },
			});
		} else {
			const auto accountId = QString::number(session->uniqueId());
			value.insert(u"account_id"_q, accountId);
			value.insert(
				u"events"_q,
				updatesAfter(cursor, 100, accountId, {}));
			value.insert(
				u"next_cursor"_q,
				QString::number(_nextUpdateCursor));
		}
		done({ QJsonObject{
			{ u"uri"_q, uri },
			{ u"mimeType"_q, u"application/json"_q },
			{ u"text"_q, QString::fromUtf8(QJsonDocument(value).toJson(
				QJsonDocument::Compact)) },
		} });
	};
	_dispatcher.addResource({
		.uri = u"telegram://updates"_q,
		.name = u"updates"_q,
		.title = u"Forkgram updates"_q,
		.description = u"Recent active-session update events."_q,
		.mimeType = u"application/json"_q,
		.handler = [=](ResourceCompletion done) {
			updateResource(u"telegram://updates"_q, 0, std::move(done));
		},
	});
	_dispatcher.addResourceTemplate({
		.uriTemplate = u"telegram://updates{?cursor}"_q,
		.name = u"updates-after"_q,
		.title = u"Forkgram updates after a cursor"_q,
		.description = u"Active-session update events after a cursor."_q,
		.mimeType = u"application/json"_q,
		.handler = [=](const QString &uri, ResourceCompletion done) {
			auto cursorOk = false;
			const auto cursor = QUrlQuery(QUrl(uri)).queryItemValue(
				u"cursor"_q).toULongLong(&cursorOk);
			if (!cursorOk) {
				done({});
				return;
			}
			updateResource(uri, cursor, std::move(done));
		},
	});
	_dispatcher.addResourceTemplate({
		.uriTemplate = u"telegram://chat/{peer_id}"_q,
		.name = u"chat"_q,
		.title = u"Forkgram chat"_q,
		.description = u"One peer loaded in the active session."_q,
		.mimeType = u"application/json"_q,
		.handler = [=](const QString &uri, ResourceCompletion done) {
			if (_application->passcodeLocked()) {
				done({});
				return;
			}
			const auto session = _application->maybePrimarySession();
			const auto prefix = u"telegram://chat/"_q;
			auto ok = false;
			const auto id = QUrl::fromPercentEncoding(
				uri.mid(prefix.size()).toUtf8()).toULongLong(&ok);
			const auto peer = (session && ok)
				? session->data().peerLoaded(PeerId(id))
				: nullptr;
			if (!peer) {
				done({});
				return;
			}
			const auto value = QJsonObject{
				{ u"account_id"_q, QString::number(session->uniqueId()) },
				{ u"chat"_q, PeerJson(
					peer,
					session->data().historyLoaded(peer)) },
			};
			done({ QJsonObject{
				{ u"uri"_q, uri },
				{ u"mimeType"_q, u"application/json"_q },
				{ u"text"_q, QString::fromUtf8(QJsonDocument(value).toJson(
					QJsonDocument::Compact)) },
			} });
		},
	});
	const auto itemResource = [=](
			const QString &uri,
			const QString &prefix,
			bool file,
			ResourceCompletion done) {
		if (_application->passcodeLocked()) {
			done({});
			return;
		}
		const auto session = _application->maybePrimarySession();
		const auto parts = QUrl::fromPercentEncoding(
			uri.mid(prefix.size()).toUtf8()).split(':');
		auto peerOk = false;
		auto messageOk = false;
		const auto peerId = (parts.size() == 2)
			? parts[0].toULongLong(&peerOk)
			: 0;
		const auto messageId = (parts.size() == 2)
			? parts[1].toLongLong(&messageOk)
			: 0;
		const auto peer = (session && peerOk)
			? session->data().peerLoaded(PeerId(peerId))
			: nullptr;
		const auto item = (peer && messageOk)
			? session->data().message(peer, MsgId(messageId))
			: nullptr;
		const auto document = (item && item->media())
			? item->media()->document()
			: nullptr;
		if (!item || (file && !document)) {
			done({});
			return;
		}
		const auto value = file
			? DocumentJson(session, item, document)
			: QJsonObject{
				{ u"account_id"_q, QString::number(session->uniqueId()) },
				{ u"message"_q, MessageJson(item) },
			};
		done({ QJsonObject{
			{ u"uri"_q, uri },
			{ u"mimeType"_q, u"application/json"_q },
			{ u"text"_q, QString::fromUtf8(QJsonDocument(value).toJson(
				QJsonDocument::Compact)) },
		} });
	};
	for (const auto file : { false, true }) {
		const auto prefix = file
			? u"telegram://file/"_q
			: u"telegram://message/"_q;
		_dispatcher.addResourceTemplate({
			.uriTemplate = prefix + u"{id}"_q,
			.name = file ? u"file"_q : u"message"_q,
			.title = file ? u"Forkgram file"_q : u"Forkgram message"_q,
			.description = file
				? u"File state for one loaded message document."_q
				: u"One message loaded in the active session."_q,
			.mimeType = u"application/json"_q,
			.handler = [=](const QString &uri, ResourceCompletion done) {
				itemResource(uri, prefix, file, std::move(done));
			},
		});
	}
	_dispatcher.addResourceTemplate({
		.uriTemplate = u"telegram://schema/method/{name}"_q,
		.name = u"method-schema"_q,
		.title = u"Telegram method schema"_q,
		.description = u"Descriptor for one method in the compiled TL layer."_q,
		.mimeType = u"application/json"_q,
		.handler = [=](const QString &uri, ResourceCompletion done) {
			const auto prefix = u"telegram://schema/method/"_q;
			const auto method = QUrl::fromPercentEncoding(
				uri.mid(prefix.size()).toUtf8());
			auto description = _tlRegistry.describeMethod(method);
			if (description.isEmpty()) {
				done({});
				return;
			}
			description.insert(u"layer"_q, _tlRegistry.layer());
			done({ QJsonObject{
				{ u"uri"_q, uri },
				{ u"mimeType"_q, u"application/json"_q },
				{ u"text"_q, QString::fromUtf8(QJsonDocument(
					description).toJson(QJsonDocument::Compact)) },
			} });
		},
	});
	_dispatcher.addResource({
		.uri = u"telegram://schema/api"_q,
		.name = u"api-schema"_q,
		.title = u"Telegram layer API schema"_q,
		.description = u"Complete TL method and constructor descriptor schema."_q,
		.mimeType = u"application/json"_q,
		.handler = [=](ResourceCompletion done) {
			done({ QJsonObject{
				{ u"uri"_q, u"telegram://schema/api"_q },
				{ u"mimeType"_q, u"application/json"_q },
				{ u"text"_q, QString::fromUtf8(GeneratedSchemaJson()) },
			} });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.raw.describe"_q,
		.description = u"Describe one method from Forkgram's compiled TL layer."_q,
		.inputSchema = StringArgumentSchema(
			u"method"_q,
			u"TL method name, for example messages.sendMessage."_q),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto method = arguments.value(u"method"_q).toString();
			auto description = _tlRegistry.describeMethod(method);
			if (description.isEmpty()) {
				done(ToolError(
					u"METHOD_NOT_FOUND"_q,
					u"No method with this name exists in the compiled layer."_q));
				return;
			}
			description.insert(u"layer"_q, _tlRegistry.layer());
			done({ .structuredContent = std::move(description) });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.raw.invoke"_q,
		.description = u"Invoke any method from Forkgram's compiled TL layer."_q,
		.inputSchema = {
			{ u"type"_q, u"object"_q },
			{ u"properties"_q, QJsonObject{
				{ u"method"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"params"_q, QJsonObject{ { u"type"_q, u"object"_q } } },
				{ u"dc_id"_q, QJsonObject{ { u"type"_q, u"integer"_q } } },
				{ u"timeout_ms"_q, QJsonObject{
					{ u"type"_q, u"integer"_q },
					{ u"minimum"_q, 1000 },
					{ u"maximum"_q, 300000 },
				} },
			} },
			{ u"required"_q, QJsonArray{ u"method"_q, u"params"_q } },
			{ u"additionalProperties"_q, false },
		},
		.outputSchema = ObjectSchema(),
		.cancellableHandler = [=](
				const QJsonObject &arguments,
				const CancellationPtr &cancellation,
				ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			if (!session) {
				return;
			}
			const auto method = arguments.value(u"method"_q).toString();
			const auto encoded = _tlCodec.encodeMethod(
				method,
				arguments.value(u"params"_q).toObject());
			if (const auto error = std::get_if<TlCodecError>(&encoded)) {
				done(ToolError(u"TL_ENCODE_FAILED"_q, error->message));
				return;
			}
			const auto &request = std::get<TlEncodedRequest>(encoded);
			const auto timeout = std::clamp(
				arguments.value(u"timeout_ms"_q).toInt(60000),
				1000,
				300000);
			const auto dcId = arguments.value(u"dc_id"_q).toInt(0);
			const auto state = std::make_shared<RawCallState>();
			state->done = std::move(done);
			state->requestId = MTP::details::GetNextRequestId();
			const auto sessionWeak = base::make_weak(session);
			WatchSessionGone(_application, sessionWeak, state);
			const auto rawRequestId = state->requestId;
			state->cancel = [sessionWeak, rawRequestId] {
				if (const auto current = sessionWeak.get()) {
					current->mtp().cancel(rawRequestId);
				}
			};
			const auto serviceWeak = base::make_weak(this);
			const auto resultType = request.resultType;
			session->mtp().sendSerialized(
				state->requestId,
				SerializedRequest(request.bytes),
				MTP::ResponseHandler{
					.done = [=](const MTP::Response &response) mutable {
						if (state->finished) {
							return true;
						}
						state->finished = true;
						const auto service = serviceWeak.get();
						const auto current = sessionWeak.get();
						if (!service || !current) {
							state->done(ToolError(
								u"SESSION_GONE"_q,
								u"The active session ended during the request."_q));
							return true;
						}
						const auto bytes = QByteArray(
							reinterpret_cast<const char*>(response.reply.constData()),
							response.reply.size() * sizeof(mtpPrime));
						const auto decoded = service->_tlCodec.decode(
							resultType,
							bytes);
						if (const auto error = std::get_if<TlCodecError>(&decoded)) {
							state->done(ToolError(
								u"TL_DECODE_FAILED"_q,
								error->message));
							return true;
						}
						auto cacheApplied = false;
						if (resultType == u"Updates"_q) {
							auto updates = MTPUpdates();
							auto from = response.reply.constData();
							if (updates.read(
									from,
									from + response.reply.size())) {
								current->api().applyUpdates(updates);
								cacheApplied = true;
							}
						}
						state->done({ .structuredContent = QJsonObject{
							{ u"account_id"_q,
								QString::number(current->uniqueId()) },
							{ u"method"_q, method },
							{ u"request_id"_q,
								QString::number(response.requestId) },
							{ u"result_type"_q, resultType },
							{ u"cache_applied"_q, cacheApplied },
							{ u"result"_q, std::get<QJsonValue>(decoded) },
						} });
						return true;
					},
					.fail = [=](
							const MTP::Error &error,
							const MTP::Response &) mutable {
						if (!state->finished) {
							state->finished = true;
							state->done(RpcToolError(error));
						}
						return true;
					},
				},
				dcId,
				0,
				0);
			cancellation->setHandler([=] {
				if (state->finished) {
					return;
				}
				state->finished = true;
				if (const auto current = sessionWeak.get()) {
					current->mtp().cancel(state->requestId);
				}
				state->done = nullptr;
			});
			QTimer::singleShot(timeout, _application, [=] {
				if (state->finished) {
					return;
				}
				state->finished = true;
				if (const auto current = sessionWeak.get()) {
					current->mtp().cancel(state->requestId);
				}
				state->done(ToolError(
					u"REQUEST_TIMEOUT"_q,
					u"Telegram did not answer before the timeout."_q));
			});
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.client.list_accounts"_q,
		.description = u"List accounts loaded in this Forkgram instance."_q,
		.inputSchema = EmptyObjectSchema(),
		.outputSchema = {
			{ u"type"_q, u"object"_q },
			{ u"properties"_q, QJsonObject{
				{ u"accounts"_q, QJsonObject{ { u"type"_q, u"array"_q } } },
			} },
			{ u"required"_q, QJsonArray{ u"accounts"_q } },
		},
		.handler = [=](const QJsonObject &, ToolCompletion done) {
			if (_application->passcodeLocked()) {
				done(ToolError(u"APP_LOCKED"_q, u"Forkgram is locked."_q));
				return;
			}
			auto accounts = QJsonArray();
			const auto primary = _application->maybePrimarySession();
			const auto started = _application->domain().started();
			const auto active = started
				? &_application->domain().active()
				: nullptr;
			for (const auto &[index, account] : _application->domain().accounts()) {
				const auto session = account->maybeSession();
				accounts.append(QJsonObject{
					{ u"index"_q, index },
					{ u"authenticated"_q, (session != nullptr) },
					{ u"active"_q, (account.get() == active) },
					{ u"account_id"_q, session
						? QJsonValue(QString::number(session->uniqueId()))
						: QJsonValue(QJsonValue::Null) },
				});
			}
			done({ .structuredContent = QJsonObject{
				{ u"account_id"_q, primary
					? QJsonValue(QString::number(primary->uniqueId()))
					: QJsonValue(QJsonValue::Null) },
				{ u"accounts"_q, std::move(accounts) },
			} });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.client.activate_account"_q,
		.description = u"Activate an account and its Forkgram window."_q,
		.inputSchema = StringArgumentSchema(
			u"account_id"_q,
			u"Account identifier returned by list_accounts."_q),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			if (_application->passcodeLocked()) {
				done(ToolError(u"APP_LOCKED"_q, u"Forkgram is locked."_q));
				return;
			}
			const auto wanted = arguments.value(u"account_id"_q).toString();
			for (const auto &[index, account] : _application->domain().accounts()) {
				if (const auto session = account->maybeSession();
						session
						&& QString::number(session->uniqueId()) == wanted) {
					_application->domain().activate(account.get());
					done({ .structuredContent = QJsonObject{
						{ u"account_id"_q, wanted },
						{ u"active"_q, true },
					} });
					return;
				}
			}
			done(ToolError(
				u"ACCOUNT_NOT_FOUND"_q,
				u"No loaded account has this identifier."_q));
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.client.open_uri"_q,
		.description = u"Open a tg:// or other Forkgram local URI."_q,
		.inputSchema = StringArgumentSchema(
			u"uri"_q,
			u"URI to open in Forkgram."_q),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			if (_application->passcodeLocked()) {
				done(ToolError(u"APP_LOCKED"_q, u"Forkgram is locked."_q));
				return;
			}
			const auto uri = arguments.value(u"uri"_q).toString();
			const auto session = _application->maybePrimarySession();
			if (!_application->openLocalUrl(uri, {})) {
				done(ToolError(
					u"URI_NOT_HANDLED"_q,
					u"Forkgram did not handle this URI."_q));
				return;
			}
			done({ .structuredContent = QJsonObject{
				{ u"account_id"_q, session
					? QJsonValue(QString::number(session->uniqueId()))
					: QJsonValue(QJsonValue::Null) },
				{ u"uri"_q, uri },
				{ u"opened"_q, true },
			} });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.peers.resolve"_q,
		.description = u"Resolve a loaded peer id or Telegram username."_q,
		.inputSchema = StringArgumentSchema(
			u"identifier"_q,
			u"Decimal peer id or username, with an optional leading @."_q),
		.outputSchema = ObjectSchema(),
		.cancellableHandler = [=](
				const QJsonObject &arguments,
				const CancellationPtr &cancellation,
				ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			if (!session) {
				return;
			}
			auto identifier = arguments.value(u"identifier"_q).toString().trimmed();
			if (identifier.startsWith('@')) {
				identifier.remove(0, 1);
			}
			if (identifier.compare(u"self"_q, Qt::CaseInsensitive) == 0
				|| identifier.compare(u"saved"_q, Qt::CaseInsensitive) == 0) {
				const auto self = session->user();
				done({ .structuredContent = QJsonObject{
					{ u"account_id"_q, QString::number(session->uniqueId()) },
					{ u"peer"_q, PeerJson(
						self,
						session->data().historyLoaded(self)) },
				} });
				return;
			}
			auto numericOk = false;
			const auto numeric = identifier.toULongLong(&numericOk);
			if (const auto loaded = numericOk
					? session->data().peerLoaded(PeerId(numeric))
					: session->data().peerByUsername(identifier)) {
				done({ .structuredContent = QJsonObject{
					{ u"account_id"_q, QString::number(session->uniqueId()) },
					{ u"peer"_q, PeerJson(
						loaded,
						session->data().historyLoaded(loaded)) },
				} });
				return;
			}
			if (numericOk || identifier.isEmpty()) {
				done(ToolError(u"PEER_NOT_FOUND"_q, u"Peer is not loaded."_q));
				return;
			}
			const auto state = std::make_shared<MtpToolState>();
			state->done = std::move(done);
			const auto sessionWeak = base::make_weak(session);
			WatchSessionGone(_application, sessionWeak, state);
			state->requestId = session->api().request(MTPcontacts_ResolveUsername(
				MTP_flags(0),
				MTP_string(identifier),
				MTP_string()
			)).done([=](const MTPcontacts_ResolvedPeer &result) mutable {
				if (state->finished) {
					return;
				}
				state->finished = true;
				const auto current = sessionWeak.get();
				if (!current) {
					state->done(ToolError(
						u"SESSION_GONE"_q,
						u"The active session ended during peer resolution."_q));
					return;
				}
				const auto &data = result.data();
				current->data().processUsers(data.vusers());
				current->data().processChats(data.vchats());
				const auto peer = current->data().peerLoaded(
					peerFromMTP(data.vpeer()));
				if (!peer) {
					state->done(ToolError(
						u"PEER_NOT_FOUND"_q,
						u"Telegram returned no usable peer."_q));
					return;
				}
				state->done({ .structuredContent = QJsonObject{
					{ u"account_id"_q, QString::number(current->uniqueId()) },
					{ u"peer"_q, PeerJson(
						peer,
						current->data().historyLoaded(peer)) },
				} });
			}).fail([=](const MTP::Error &error) mutable {
				if (!state->finished) {
					state->finished = true;
					state->done(RpcToolError(error));
				}
			}).send();
			SetApiCancellation(sessionWeak, state);
			cancellation->setHandler([=] {
				if (state->finished) {
					return;
				}
				state->finished = true;
				if (const auto current = sessionWeak.get()) {
					current->api().request(state->requestId).cancel();
				}
				state->done = nullptr;
			});
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.chats.list"_q,
		.description = u"List chats currently loaded in the active session."_q,
		.inputSchema = {
			{ u"type"_q, u"object"_q },
			{ u"properties"_q, QJsonObject{
				{ u"query"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"cursor"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"limit"_q, QJsonObject{
					{ u"type"_q, u"integer"_q },
					{ u"minimum"_q, 1 },
					{ u"maximum"_q, 200 },
				} },
			} },
			{ u"additionalProperties"_q, false },
		},
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			if (!session) {
				return;
			}
			const auto query = arguments.value(u"query"_q).toString().trimmed();
			const auto limit = std::clamp(
				arguments.value(u"limit"_q).toInt(50),
				1,
				200);
			auto offsetOk = false;
			auto offset = arguments.value(u"cursor"_q).toString().toInt(&offsetOk);
			if (!offsetOk || offset < 0) {
				offset = 0;
			}
			auto chats = QJsonArray();
			auto matched = 0;
			auto more = false;
			const auto list = session->data().chatsList()->indexed();
			for (const auto row : *list) {
				const auto history = row->history();
				if (!history) {
					continue;
				}
				const auto peer = history->peer.get();
				if (!query.isEmpty()
					&& !peer->name().contains(query, Qt::CaseInsensitive)
					&& !peer->username().contains(query, Qt::CaseInsensitive)) {
					continue;
				}
				if (matched++ < offset) {
					continue;
				} else if (chats.size() >= limit) {
					more = true;
					break;
				}
				chats.append(PeerJson(peer, history));
			}
			auto result = QJsonObject{
				{ u"account_id"_q, QString::number(session->uniqueId()) },
				{ u"loaded"_q, session->data().chatsList()->loaded() },
				{ u"chats"_q, std::move(chats) },
			};
			if (more) {
				result.insert(
					u"next_cursor"_q,
					QString::number(offset + limit));
			}
			done({ .structuredContent = std::move(result) });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.chats.get"_q,
		.description = u"Get a peer already loaded in the active session."_q,
		.inputSchema = PeerInputSchema(),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			if (!session) {
				return;
			}
			const auto peer = ResolveLoadedPeer(session, arguments);
			if (!peer) {
				done(ToolError(u"PEER_NOT_FOUND"_q, u"Peer is not loaded."_q));
				return;
			}
			done({ .structuredContent = QJsonObject{
				{ u"account_id"_q, QString::number(session->uniqueId()) },
				{ u"chat"_q, PeerJson(
					peer,
					session->data().historyLoaded(peer)) },
			} });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.chats.open"_q,
		.description = u"Open a loaded peer in the Forkgram UI."_q,
		.inputSchema = PeerInputSchema(),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			if (!session) {
				return;
			}
			const auto peer = ResolveLoadedPeer(session, arguments);
			const auto window = peer
				? _application->windowForShowingHistory(peer)
				: nullptr;
			const auto controller = window ? window->sessionController() : nullptr;
			if (!peer || !controller) {
				done(ToolError(
					u"PEER_NOT_FOUND"_q,
					u"No window can show this loaded peer."_q));
				return;
			}
			controller->showPeerHistory(peer);
			done({ .structuredContent = QJsonObject{
				{ u"account_id"_q, QString::number(session->uniqueId()) },
				{ u"peer_id"_q, QString::number(peer->id.value) },
				{ u"opened"_q, true },
			} });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.chats.create"_q,
		.description = u"Create a basic group from loaded users."_q,
		.inputSchema = {
			{ u"type"_q, u"object"_q },
			{ u"properties"_q, QJsonObject{
				{ u"title"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"user_peer_ids"_q, QJsonObject{
					{ u"type"_q, u"array"_q },
					{ u"items"_q,
						QJsonObject{ { u"type"_q, u"string"_q } } },
					{ u"minItems"_q, 1 },
				} },
			} },
			{ u"required"_q, QJsonArray{ u"title"_q, u"user_peer_ids"_q } },
			{ u"additionalProperties"_q, false },
		},
		.outputSchema = ObjectSchema(),
		.cancellableHandler = [=](
				const QJsonObject &arguments,
				const CancellationPtr &cancellation,
				ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			if (!session) {
				return;
			}
			const auto title = arguments.value(u"title"_q).toString().trimmed();
			auto users = QVector<MTPInputUser>();
			for (const auto &value : arguments.value(
					u"user_peer_ids"_q).toArray()) {
				auto ok = false;
				const auto id = value.toString().toULongLong(&ok);
				const auto peer = ok
					? session->data().peerLoaded(PeerId(id))
					: nullptr;
				const auto user = peer ? peer->asUser() : nullptr;
				if (user && !user->isSelf()) {
					users.push_back(user->inputUser());
				}
			}
			if (title.isEmpty() || users.empty()) {
				done(ToolError(
					u"INVALID_GROUP"_q,
					u"A title and at least one loaded user are required."_q));
				return;
			}
			const auto state = std::make_shared<MtpToolState>();
			state->done = std::move(done);
			const auto sessionWeak = base::make_weak(session);
			WatchSessionGone(_application, sessionWeak, state);
			state->requestId = session->api().request(MTPmessages_CreateChat(
				MTP_flags(0),
				MTP_vector<MTPInputUser>(users),
				MTP_string(title),
				MTP_int(0)
			)).done([=](const MTPmessages_InvitedUsers &result) mutable {
				if (state->finished) {
					return;
				}
				state->finished = true;
				if (const auto current = sessionWeak.get()) {
					current->api().applyUpdates(result.data().vupdates());
					state->done({ .structuredContent = QJsonObject{
						{ u"account_id"_q,
							QString::number(current->uniqueId()) },
						{ u"created"_q, true },
					} });
				} else {
					state->done(ToolError(
						u"SESSION_GONE"_q,
						u"The active session ended during group creation."_q));
				}
			}).fail([=](const MTP::Error &error) mutable {
				if (!state->finished) {
					state->finished = true;
					state->done(RpcToolError(error));
				}
			}).send();
			SetApiCancellation(sessionWeak, state);
			cancellation->setHandler([=] {
				if (state->finished) {
					return;
				}
				state->finished = true;
				if (const auto current = sessionWeak.get()) {
					current->api().request(state->requestId).cancel();
				}
				state->done = nullptr;
			});
		},
	});
	for (const auto join : { true, false }) {
		_dispatcher.addTool({
			.name = join
				? u"telegram.chats.join"_q
				: u"telegram.chats.leave"_q,
			.description = join
				? u"Join a loaded channel or supergroup."_q
				: u"Leave a loaded channel or supergroup."_q,
			.inputSchema = PeerInputSchema(),
			.outputSchema = ObjectSchema(),
			.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
				const auto session = RequireSession(_application, done);
				const auto peer = session
					? ResolveLoadedPeer(session, arguments)
					: nullptr;
				const auto channel = peer ? peer->asChannel() : nullptr;
				if (!session || !channel) {
					if (session) {
						done(ToolError(
							u"UNSUPPORTED_CHAT"_q,
							u"A loaded channel or supergroup is required."_q));
					}
					return;
				}
				if (join) {
					session->api().joinChannel(channel);
				} else {
					session->api().leaveChannel(channel);
				}
				done({ .structuredContent = QJsonObject{
					{ u"account_id"_q, QString::number(session->uniqueId()) },
					{ u"peer_id"_q, QString::number(peer->id.value) },
					{ u"operation"_q, join ? u"join"_q : u"leave"_q },
					{ u"queued"_q, true },
				} });
			},
		});
	}
	_dispatcher.addTool({
		.name = u"telegram.messages.list"_q,
		.description = u"List newest messages for a peer, fetching when needed."_q,
		.inputSchema = PeerInputSchema(true),
		.outputSchema = ObjectSchema(),
		.cancellableHandler = [=](
				const QJsonObject &arguments,
				const CancellationPtr &cancellation,
				ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			if (!session) {
				return;
			}
			const auto peer = ResolveLoadedPeer(session, arguments);
			const auto history = peer
				? session->data().history(peer)
				: nullptr;
			if (!history) {
				done(ToolError(
					u"PEER_NOT_FOUND"_q,
					u"This peer is not loaded."_q));
				return;
			}
			const auto limit = std::clamp(
				arguments.value(u"limit"_q).toInt(50),
				1,
				200);
			auto messages = QJsonArray();
			for (auto block = history->blocks.rbegin();
					block != history->blocks.rend() && messages.size() < limit;
					++block) {
				for (auto view = (*block)->messages.rbegin();
						view != (*block)->messages.rend()
							&& messages.size() < limit;
						++view) {
					messages.append(MessageJson((*view)->data()));
				}
			}
			if (!messages.isEmpty()) {
				done({ .structuredContent = QJsonObject{
					{ u"account_id"_q, QString::number(session->uniqueId()) },
					{ u"peer_id"_q, QString::number(peer->id.value) },
					{ u"loaded_at_top"_q, history->loadedAtTop() },
					{ u"loaded_at_bottom"_q, history->loadedAtBottom() },
					{ u"messages"_q, std::move(messages) },
				} });
				return;
			}
			const auto state = std::make_shared<MtpToolState>();
			state->done = std::move(done);
			const auto sessionWeak = base::make_weak(session);
			WatchSessionGone(_application, sessionWeak, state);
			const auto requestedPeerId = peer->id;
			state->requestId = session->api().request(MTPmessages_GetHistory(
				peer->input(),
				MTP_int(0),
				MTP_int(0),
				MTP_int(0),
				MTP_int(limit),
				MTP_int(0),
				MTP_int(0),
				MTP_long(0)
			)).done([=](const MTPmessages_Messages &result) mutable {
				if (state->finished) {
					return;
				}
				state->finished = true;
				const auto current = sessionWeak.get();
				const auto currentPeer = current
					? current->data().peerLoaded(requestedPeerId)
					: nullptr;
				if (!current || !currentPeer) {
					state->done(ToolError(
						u"SESSION_GONE"_q,
						u"The active session ended while loading history."_q));
					return;
				}
				const auto collect = [&](const auto &data) {
					current->data().processUsers(data.vusers());
					current->data().processChats(data.vchats());
					currentPeer->processTopics(data.vtopics());
					auto loaded = QJsonArray();
					for (const auto &message : data.vmessages().v) {
						const auto messagePeer = PeerFromMessage(message);
						if (!current->data().peerLoaded(messagePeer)
							|| !DateFromMessage(message)) {
							continue;
						}
						const auto item = current->data().addNewMessage(
							message,
							MessageFlags(),
							NewMessageType::Existing);
						loaded.append(MessageJson(item));
					}
					return loaded;
				};
				auto loaded = result.match(
					[&](const MTPDmessages_messages &data) {
						return collect(data);
					},
					[&](const MTPDmessages_messagesSlice &data) {
						return collect(data);
					},
					[&](const MTPDmessages_channelMessages &data) {
						return collect(data);
					},
					[](const MTPDmessages_messagesNotModified &) {
						return QJsonArray();
					});
				state->done({ .structuredContent = QJsonObject{
					{ u"account_id"_q, QString::number(current->uniqueId()) },
					{ u"peer_id"_q, QString::number(currentPeer->id.value) },
					{ u"server_fetched"_q, true },
					{ u"messages"_q, std::move(loaded) },
				} });
			}).fail([=](const MTP::Error &error) mutable {
				if (!state->finished) {
					state->finished = true;
					state->done(RpcToolError(error));
				}
			}).send();
			SetApiCancellation(sessionWeak, state);
			cancellation->setHandler([=] {
				if (state->finished) {
					return;
				}
				state->finished = true;
				if (const auto current = sessionWeak.get()) {
					current->api().request(state->requestId).cancel();
				}
				state->done = nullptr;
			});
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.messages.get"_q,
		.description = u"Get one loaded message by peer and message id."_q,
		.inputSchema = MessageInputSchema(),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			if (!session) {
				return;
			}
			const auto peer = ResolveLoadedPeer(session, arguments);
			auto idOk = false;
			const auto messageId = MsgId(arguments.value(
				u"message_id"_q).toString().toLongLong(&idOk));
			const auto item = (peer && idOk)
				? session->data().message(peer, messageId)
				: nullptr;
			if (!item) {
				done(ToolError(
					u"MESSAGE_NOT_FOUND"_q,
					u"Message is not loaded."_q));
				return;
			}
			done({ .structuredContent = QJsonObject{
				{ u"account_id"_q, QString::number(session->uniqueId()) },
				{ u"message"_q, MessageJson(item) },
			} });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.messages.search"_q,
		.description = u"Search messages in one active-session peer."_q,
		.inputSchema = {
			{ u"type"_q, u"object"_q },
			{ u"properties"_q, QJsonObject{
				{ u"peer_id"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"query"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"offset_id"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"limit"_q, QJsonObject{
					{ u"type"_q, u"integer"_q },
					{ u"minimum"_q, 1 },
					{ u"maximum"_q, 100 },
				} },
			} },
			{ u"required"_q, QJsonArray{ u"peer_id"_q, u"query"_q } },
			{ u"additionalProperties"_q, false },
		},
		.outputSchema = ObjectSchema(),
		.cancellableHandler = [=](
				const QJsonObject &arguments,
				const CancellationPtr &cancellation,
				ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			const auto peer = session
				? ResolveLoadedPeer(session, arguments)
				: nullptr;
			const auto query = arguments.value(u"query"_q).toString();
			if (!session || !peer || query.isEmpty()) {
				if (session) {
					done(ToolError(
						u"INVALID_SEARCH"_q,
						u"A loaded peer and non-empty query are required."_q));
				}
				return;
			}
			auto offsetOk = false;
			const auto offset = arguments.value(
				u"offset_id"_q).toString().toInt(&offsetOk);
			const auto limit = std::clamp(
				arguments.value(u"limit"_q).toInt(50),
				1,
				100);
			const auto state = std::make_shared<MtpToolState>();
			state->done = std::move(done);
			const auto sessionWeak = base::make_weak(session);
			WatchSessionGone(_application, sessionWeak, state);
			const auto requestedPeerId = peer->id;
			state->requestId = session->api().request(MTPmessages_Search(
				MTP_flags(0),
				peer->input(),
				MTP_string(query),
				MTP_inputPeerEmpty(),
				MTP_inputPeerEmpty(),
				MTP_vector<MTPReaction>(),
				MTP_int(0),
				MTP_inputMessagesFilterEmpty(),
				MTP_int(0),
				MTP_int(0),
				MTP_int(offsetOk ? offset : 0),
				MTP_int(0),
				MTP_int(limit),
				MTP_int(0),
				MTP_int(0),
				MTP_long(0)
			)).done([=](const MTPmessages_Messages &result) mutable {
				if (state->finished) {
					return;
				}
				state->finished = true;
				const auto current = sessionWeak.get();
				const auto currentPeer = current
					? current->data().peerLoaded(requestedPeerId)
					: nullptr;
				if (!current || !currentPeer) {
					state->done(ToolError(
						u"SESSION_GONE"_q,
						u"The active session ended during search."_q));
					return;
				}
				const auto collect = [&](const auto &data, int total) {
					current->data().processUsers(data.vusers());
					current->data().processChats(data.vchats());
					currentPeer->processTopics(data.vtopics());
					auto messages = QJsonArray();
					for (const auto &message : data.vmessages().v) {
						const auto messagePeer = PeerFromMessage(message);
						if (!current->data().peerLoaded(messagePeer)
							|| !DateFromMessage(message)) {
							continue;
						}
						const auto item = current->data().addNewMessage(
							message,
							MessageFlags(),
							NewMessageType::Existing);
						messages.append(MessageJson(item));
					}
					auto value = QJsonObject{
						{ u"account_id"_q,
							QString::number(current->uniqueId()) },
						{ u"peer_id"_q,
							QString::number(currentPeer->id.value) },
						{ u"total"_q, total },
						{ u"messages"_q, messages },
					};
					if (messages.size() == limit) {
						value.insert(
							u"next_offset_id"_q,
							messages.at(messages.size() - 1).toObject().value(
								u"message_id"_q));
					}
					return value;
				};
				auto value = result.match(
					[&](const MTPDmessages_messages &data) {
						return collect(data, data.vmessages().v.size());
					},
					[&](const MTPDmessages_messagesSlice &data) {
						return collect(data, data.vcount().v);
					},
					[&](const MTPDmessages_channelMessages &data) {
						return collect(data, data.vcount().v);
					},
					[&](const MTPDmessages_messagesNotModified &) {
						return QJsonObject{
							{ u"account_id"_q,
								QString::number(current->uniqueId()) },
							{ u"peer_id"_q,
								QString::number(currentPeer->id.value) },
							{ u"total"_q, 0 },
							{ u"messages"_q, QJsonArray() },
						};
					});
				state->done({ .structuredContent = std::move(value) });
			}).fail([=](const MTP::Error &error) mutable {
				if (!state->finished) {
					state->finished = true;
					state->done(RpcToolError(error));
				}
			}).send();
			SetApiCancellation(sessionWeak, state);
			cancellation->setHandler([=] {
				if (state->finished) {
					return;
				}
				state->finished = true;
				if (const auto current = sessionWeak.get()) {
					current->api().request(state->requestId).cancel();
				}
				state->done = nullptr;
			});
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.messages.edit"_q,
		.description = u"Edit the text of one loaded message."_q,
		.inputSchema = MessageInputSchema(true),
		.outputSchema = ObjectSchema(),
		.cancellableHandler = [=](
				const QJsonObject &arguments,
				const CancellationPtr &cancellation,
				ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			const auto item = session
				? ResolveLoadedMessage(session, arguments)
				: nullptr;
			const auto text = arguments.value(u"text"_q).toString();
			if (!session || !item || text.isEmpty()) {
				if (session) {
					done(ToolError(
						u"INVALID_MESSAGE"_q,
						u"A loaded message and non-empty text are required."_q));
				}
				return;
			}
			const auto state = std::make_shared<MtpToolState>();
			state->done = std::move(done);
			const auto sessionWeak = base::make_weak(session);
			WatchSessionGone(_application, sessionWeak, state);
			const auto fullId = item->fullId();
			state->requestId = Api::EditTextMessage(
				item,
				TextWithEntities{ text, {} },
				Data::WebPageDraft::FromItem(item),
				Api::SendOptions(),
				[=](mtpRequestId requestId) mutable {
					if (state->finished) {
						return;
					}
					state->finished = true;
					if (const auto current = sessionWeak.get()) {
						state->done({ .structuredContent = QJsonObject{
							{ u"account_id"_q,
								QString::number(current->uniqueId()) },
							{ u"peer_id"_q,
								QString::number(fullId.peer.value) },
							{ u"message_id"_q,
								QString::number(fullId.msg.bare) },
							{ u"request_id"_q,
								QString::number(requestId) },
							{ u"edited"_q, true },
						} });
					} else {
						state->done(ToolError(
							u"SESSION_GONE"_q,
							u"The active session ended during the edit."_q));
					}
				},
				[=](const QString &error, mtpRequestId) mutable {
					if (!state->finished) {
						state->finished = true;
						state->done(ToolError(
							error.isEmpty() ? u"EDIT_FAILED"_q : error,
							u"Telegram rejected the message edit."_q));
					}
				},
				item->media() && item->media()->hasSpoiler());
			SetApiCancellation(sessionWeak, state);
			cancellation->setHandler([=] {
				if (state->finished) {
					return;
				}
				state->finished = true;
				if (const auto current = sessionWeak.get()) {
					current->api().request(state->requestId).cancel();
				}
				state->done = nullptr;
			});
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.messages.send"_q,
		.description = u"Send a text message through the active client session."_q,
		.inputSchema = {
			{ u"type"_q, u"object"_q },
			{ u"properties"_q, QJsonObject{
				{ u"peer_id"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"text"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
			} },
			{ u"required"_q, QJsonArray{ u"peer_id"_q, u"text"_q } },
			{ u"additionalProperties"_q, false },
		},
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			if (!session) {
				return;
			}
			const auto peer = ResolveLoadedPeer(session, arguments);
			const auto text = arguments.value(u"text"_q).toString();
			if (!peer || text.isEmpty()) {
				done(ToolError(
					u"INVALID_MESSAGE"_q,
					u"A loaded peer and non-empty text are required."_q));
				return;
			}
			const auto history = session->data().history(peer);
			auto message = Api::MessageToSend(Api::SendAction(history));
			message.textWithTags = TextWithTags{ text, {} };
			session->api().sendMessage(std::move(message));
			const auto local = history->latestSendingMessage();
			done({ .structuredContent = QJsonObject{
				{ u"account_id"_q, QString::number(session->uniqueId()) },
				{ u"peer_id"_q, QString::number(peer->id.value) },
				{ u"queued"_q, true },
				{ u"local_message_id"_q, local
					? QJsonValue(QString::number(local->id.bare))
					: QJsonValue(QJsonValue::Null) },
			} });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.messages.delete"_q,
		.description = u"Delete loaded messages from a peer history."_q,
		.inputSchema = {
			{ u"type"_q, u"object"_q },
			{ u"properties"_q, QJsonObject{
				{ u"peer_id"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"message_ids"_q, QJsonObject{
					{ u"type"_q, u"array"_q },
					{ u"items"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
					{ u"minItems"_q, 1 },
				} },
				{ u"revoke"_q, QJsonObject{ { u"type"_q, u"boolean"_q } } },
			} },
			{ u"required"_q, QJsonArray{ u"peer_id"_q, u"message_ids"_q } },
			{ u"additionalProperties"_q, false },
		},
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			if (!session) {
				return;
			}
			const auto peer = ResolveLoadedPeer(session, arguments);
			const auto history = peer
				? session->data().historyLoaded(peer)
				: nullptr;
			auto ids = QVector<MTPint>();
			for (const auto &value : arguments.value(u"message_ids"_q).toArray()) {
				auto ok = false;
				const auto id = value.toString().toInt(&ok);
				if (ok && id > 0) {
					ids.push_back(MTP_int(id));
				}
			}
			if (!history || ids.empty()) {
				done(ToolError(
					u"INVALID_MESSAGES"_q,
					u"Loaded history and message ids are required."_q));
				return;
			}
			session->data().histories().deleteMessages(
				history,
				ids,
				arguments.value(u"revoke"_q).toBool(false));
			done({ .structuredContent = QJsonObject{
				{ u"account_id"_q, QString::number(session->uniqueId()) },
				{ u"peer_id"_q, QString::number(peer->id.value) },
				{ u"queued"_q, true },
				{ u"count"_q, ids.size() },
			} });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.messages.forward"_q,
		.description = u"Forward loaded messages to another loaded peer."_q,
		.inputSchema = {
			{ u"type"_q, u"object"_q },
			{ u"properties"_q, QJsonObject{
				{ u"from_peer_id"_q,
					QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"to_peer_id"_q,
					QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"message_ids"_q, QJsonObject{
					{ u"type"_q, u"array"_q },
					{ u"items"_q,
						QJsonObject{ { u"type"_q, u"string"_q } } },
					{ u"minItems"_q, 1 },
				} },
				{ u"drop_author"_q,
					QJsonObject{ { u"type"_q, u"boolean"_q } } },
			} },
			{ u"required"_q, QJsonArray{
				u"from_peer_id"_q,
				u"to_peer_id"_q,
				u"message_ids"_q,
			} },
			{ u"additionalProperties"_q, false },
		},
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			if (!session) {
				return;
			}
			auto fromOk = false;
			auto toOk = false;
			const auto fromId = arguments.value(
				u"from_peer_id"_q).toString().toULongLong(&fromOk);
			const auto toId = arguments.value(
				u"to_peer_id"_q).toString().toULongLong(&toOk);
			const auto from = fromOk
				? session->data().peerLoaded(PeerId(fromId))
				: nullptr;
			const auto to = toOk
				? session->data().peerLoaded(PeerId(toId))
				: nullptr;
			auto items = HistoryItemsList();
			if (from && to) {
				for (const auto &value : arguments.value(
						u"message_ids"_q).toArray()) {
					auto idOk = false;
					const auto id = MsgId(value.toString().toLongLong(&idOk));
					if (idOk) {
						if (const auto item = session->data().message(from, id)) {
							items.push_back(item);
						}
					}
				}
			}
			if (!from || !to || items.empty()) {
				done(ToolError(
					u"INVALID_MESSAGES"_q,
					u"Loaded source, destination and messages are required."_q));
				return;
			}
			const auto count = int(items.size());
			session->api().forwardMessages(
				Data::ResolvedForwardDraft{
					.items = std::move(items),
					.options = arguments.value(u"drop_author"_q).toBool()
						? Data::ForwardOptions::NoSenderNames
						: Data::ForwardOptions::PreserveInfo,
				},
				Api::SendAction(session->data().history(to)));
			done({ .structuredContent = QJsonObject{
				{ u"account_id"_q, QString::number(session->uniqueId()) },
				{ u"from_peer_id"_q, QString::number(from->id.value) },
				{ u"to_peer_id"_q, QString::number(to->id.value) },
				{ u"count"_q, count },
				{ u"queued"_q, true },
			} });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.messages.react"_q,
		.description = u"Toggle an emoji reaction on one loaded message."_q,
		.inputSchema = {
			{ u"type"_q, u"object"_q },
			{ u"properties"_q, QJsonObject{
				{ u"peer_id"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"message_id"_q,
					QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"reaction"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
			} },
			{ u"required"_q,
				QJsonArray{ u"peer_id"_q, u"message_id"_q, u"reaction"_q } },
			{ u"additionalProperties"_q, false },
		},
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			const auto item = session
				? ResolveLoadedMessage(session, arguments)
				: nullptr;
			const auto reaction = arguments.value(u"reaction"_q).toString();
			if (!session || !item || reaction.isEmpty()) {
				if (session) {
					done(ToolError(
						u"INVALID_REACTION"_q,
						u"A loaded message and emoji reaction are required."_q));
				}
				return;
			}
			item->toggleReaction(
				Data::ReactionId{ reaction },
				HistoryReactionSource::Selector);
			done({ .structuredContent = QJsonObject{
				{ u"account_id"_q, QString::number(session->uniqueId()) },
				{ u"peer_id"_q,
					QString::number(item->history()->peer->id.value) },
				{ u"message_id"_q, QString::number(item->id.bare) },
				{ u"reaction"_q, reaction },
				{ u"queued"_q, true },
			} });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.messages.pin"_q,
		.description = u"Pin or unpin one loaded message."_q,
		.inputSchema = {
			{ u"type"_q, u"object"_q },
			{ u"properties"_q, QJsonObject{
				{ u"peer_id"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"message_id"_q,
					QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"pinned"_q, QJsonObject{ { u"type"_q, u"boolean"_q } } },
				{ u"silent"_q, QJsonObject{ { u"type"_q, u"boolean"_q } } },
			} },
			{ u"required"_q,
				QJsonArray{ u"peer_id"_q, u"message_id"_q, u"pinned"_q } },
			{ u"additionalProperties"_q, false },
		},
		.outputSchema = ObjectSchema(),
		.cancellableHandler = [=](
				const QJsonObject &arguments,
				const CancellationPtr &cancellation,
				ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			const auto item = session
				? ResolveLoadedMessage(session, arguments)
				: nullptr;
			if (!session || !item) {
				if (session) {
					done(ToolError(
						u"MESSAGE_NOT_FOUND"_q,
						u"The message is not loaded."_q));
				}
				return;
			}
			using Flag = MTPmessages_UpdatePinnedMessage::Flag;
			auto flags = MTPmessages_UpdatePinnedMessage::Flags();
			if (!arguments.value(u"pinned"_q).toBool()) {
				flags |= Flag::f_unpin;
			}
			if (arguments.value(u"silent"_q).toBool()) {
				flags |= Flag::f_silent;
			}
			const auto state = std::make_shared<MtpToolState>();
			state->done = std::move(done);
			const auto sessionWeak = base::make_weak(session);
			WatchSessionGone(_application, sessionWeak, state);
			const auto fullId = item->fullId();
			const auto pinned = arguments.value(u"pinned"_q).toBool();
			state->requestId = session->api().request(
				MTPmessages_UpdatePinnedMessage(
					MTP_flags(flags),
					item->history()->peer->input(),
					MTP_int(item->id)
			)).done([=](const MTPUpdates &result) mutable {
				if (state->finished) {
					return;
				}
				state->finished = true;
				if (const auto current = sessionWeak.get()) {
					current->api().applyUpdates(result);
					state->done({ .structuredContent = QJsonObject{
						{ u"account_id"_q,
							QString::number(current->uniqueId()) },
						{ u"peer_id"_q,
							QString::number(fullId.peer.value) },
						{ u"message_id"_q,
							QString::number(fullId.msg.bare) },
						{ u"pinned"_q, pinned },
					} });
				} else {
					state->done(ToolError(
						u"SESSION_GONE"_q,
						u"The active session ended during pinning."_q));
				}
			}).fail([=](const MTP::Error &error) mutable {
				if (!state->finished) {
					state->finished = true;
					state->done(RpcToolError(error));
				}
			}).send();
			SetApiCancellation(sessionWeak, state);
			cancellation->setHandler([=] {
				if (state->finished) {
					return;
				}
				state->finished = true;
				if (const auto current = sessionWeak.get()) {
					current->api().request(state->requestId).cancel();
				}
				state->done = nullptr;
			});
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.messages.read"_q,
		.description = u"Mark a loaded history read through a message id."_q,
		.inputSchema = MessageInputSchema(),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			if (!session) {
				return;
			}
			const auto peer = ResolveLoadedPeer(session, arguments);
			const auto history = peer
				? session->data().historyLoaded(peer)
				: nullptr;
			auto idOk = false;
			const auto messageId = MsgId(arguments.value(
				u"message_id"_q).toString().toLongLong(&idOk));
			if (!history || !idOk || messageId.bare <= 0) {
				done(ToolError(
					u"INVALID_MESSAGE"_q,
					u"Loaded history and positive message id are required."_q));
				return;
			}
			session->data().histories().readInboxTill(history, messageId);
			done({ .structuredContent = QJsonObject{
				{ u"account_id"_q, QString::number(session->uniqueId()) },
				{ u"peer_id"_q, QString::number(peer->id.value) },
				{ u"read_through"_q, QString::number(messageId.bare) },
			} });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.files.upload"_q,
		.description = u"Queue a local file for sending to a loaded peer."_q,
		.inputSchema = {
			{ u"type"_q, u"object"_q },
			{ u"properties"_q, QJsonObject{
				{ u"peer_id"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"path"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"caption"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
			} },
			{ u"required"_q, QJsonArray{ u"peer_id"_q, u"path"_q } },
			{ u"additionalProperties"_q, false },
		},
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			const auto peer = session
				? ResolveLoadedPeer(session, arguments)
				: nullptr;
			const auto path = arguments.value(u"path"_q).toString();
			const auto info = QFileInfo(path);
			if (!session
				|| !peer
				|| !info.isAbsolute()
				|| !info.isFile()
				|| !info.isReadable()
				|| !info.size()) {
				if (session) {
					done(ToolError(
						u"INVALID_FILE"_q,
						u"An absolute path to a readable non-empty file is required."_q));
				}
				return;
			}
			auto list = Ui::PreparedList();
			list.files.emplace_back(path);
			list.files.front().caption = TextWithTags{
				arguments.value(u"caption"_q).toString(),
				{},
			};
			session->api().sendFiles(
				std::move(list),
				SendMediaType::File,
				nullptr,
				Api::SendAction(session->data().history(peer)));
			done({ .structuredContent = QJsonObject{
				{ u"account_id"_q, QString::number(session->uniqueId()) },
				{ u"peer_id"_q, QString::number(peer->id.value) },
				{ u"path"_q, info.absoluteFilePath() },
				{ u"size"_q, QString::number(info.size()) },
				{ u"queued"_q, true },
			} });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.files.download"_q,
		.description = u"Download a loaded message document to a local path."_q,
		.inputSchema = {
			{ u"type"_q, u"object"_q },
			{ u"properties"_q, QJsonObject{
				{ u"peer_id"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"message_id"_q,
					QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"path"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"overwrite"_q,
					QJsonObject{ { u"type"_q, u"boolean"_q } } },
			} },
			{ u"required"_q,
				QJsonArray{ u"peer_id"_q, u"message_id"_q, u"path"_q } },
			{ u"additionalProperties"_q, false },
		},
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			auto item = static_cast<HistoryItem*>(nullptr);
			const auto document = session
				? ResolveLoadedDocument(session, arguments, &item)
				: nullptr;
			const auto path = arguments.value(u"path"_q).toString();
			const auto target = QFileInfo(path);
			const auto parent = QFileInfo(target.dir().absolutePath());
			const auto overwrite = arguments.value(u"overwrite"_q).toBool();
			if (!session
				|| !document
				|| !item
				|| !target.isAbsolute()
				|| !parent.isDir()
				|| !parent.isWritable()
				|| (target.exists() && !target.isFile())) {
				if (session) {
					done(ToolError(
						u"INVALID_DOWNLOAD"_q,
						u"A loaded document and writable absolute target are "_q
						+ u"required; existing targets require overwrite=true."_q));
				}
				return;
			}
			if (!overwrite) {
				auto reservation = QFile(path);
				if (!reservation.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
					done(ToolError(
						u"TARGET_EXISTS"_q,
						u"The download target already exists."_q));
					return;
				}
				reservation.close();
			}
			document->save(Data::FileOrigin(item->fullId()), path);
			auto result = DocumentJson(session, item, document);
			result.insert(u"target_path"_q, target.absoluteFilePath());
			result.insert(u"queued"_q, document->loading());
			done({ .structuredContent = std::move(result) });
		},
	});
	for (const auto cancel : { false, true }) {
		_dispatcher.addTool({
			.name = cancel
				? u"telegram.files.cancel"_q
				: u"telegram.files.status"_q,
			.description = cancel
				? u"Cancel a loaded message upload or download."_q
				: u"Return upload/download state for a loaded message document."_q,
			.inputSchema = MessageInputSchema(),
			.outputSchema = ObjectSchema(),
			.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
				const auto session = RequireSession(_application, done);
				auto item = static_cast<HistoryItem*>(nullptr);
				const auto document = session
					? ResolveLoadedDocument(session, arguments, &item)
					: nullptr;
				if (!session || !document || !item) {
					if (session) {
						done(ToolError(
							u"DOCUMENT_NOT_FOUND"_q,
							u"The loaded message has no document."_q));
					}
					return;
				}
				if (cancel) {
					if (document->uploading()) {
						session->uploader().cancel(item->fullId());
					} else {
						document->cancel();
					}
				}
				auto result = DocumentJson(session, item, document);
				if (cancel) {
					result.insert(u"cancel_requested"_q, true);
				}
				done({ .structuredContent = std::move(result) });
			},
		});
	}
	_dispatcher.addTool({
		.name = u"telegram.contacts.list"_q,
		.description = u"List contacts currently loaded by the active session."_q,
		.inputSchema = EmptyObjectSchema(),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &, ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			if (!session) {
				return;
			}
			auto contacts = QJsonArray();
			for (const auto row : *session->data().contactsList()) {
				if (const auto history = row->history()) {
					contacts.append(PeerJson(history->peer));
				}
			}
			done({ .structuredContent = QJsonObject{
				{ u"account_id"_q, QString::number(session->uniqueId()) },
				{ u"loaded"_q, session->data().contactsLoaded().current() },
				{ u"contacts"_q, std::move(contacts) },
			} });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.members.list"_q,
		.description = u"List members currently cached for a loaded group."_q,
		.inputSchema = PeerInputSchema(true),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			const auto peer = session
				? ResolveLoadedPeer(session, arguments)
				: nullptr;
			const auto limit = std::clamp(
				arguments.value(u"limit"_q).toInt(100),
				1,
				200);
			if (!session || !peer || (!peer->isChat() && !peer->isMegagroup())) {
				if (session) {
					done(ToolError(
						u"INVALID_GROUP"_q,
						u"A loaded basic group or supergroup is required."_q));
				}
				return;
			}
			auto members = QJsonArray();
			auto cached = std::vector<not_null<UserData*>>();
			auto total = 0;
			auto loaded = false;
			if (const auto chat = peer->asChat()) {
				total = chat->count;
				loaded = !chat->participants.empty();
				cached.assign(
					begin(chat->participants),
					end(chat->participants));
			} else if (const auto channel = peer->asChannel(); channel->mgInfo) {
				total = channel->membersCount();
				loaded = channel->mgInfo->lastParticipantsStatus
					& MegagroupInfo::LastParticipantsOnceReceived;
				cached.assign(
					begin(channel->mgInfo->lastParticipants),
					end(channel->mgInfo->lastParticipants));
			}
			std::sort(begin(cached), end(cached), [](auto a, auto b) {
				return a->id.value < b->id.value;
			});
			for (const auto user : cached) {
				if (members.size() >= limit) {
					break;
				}
				members.append(PeerJson(user));
			}
			done({ .structuredContent = QJsonObject{
				{ u"account_id"_q, QString::number(session->uniqueId()) },
				{ u"peer_id"_q, QString::number(peer->id.value) },
				{ u"loaded"_q, loaded },
				{ u"total"_q, total },
				{ u"members"_q, std::move(members) },
			} });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.rights.get"_q,
		.description = u"Return the active account's cached management rights."_q,
		.inputSchema = PeerInputSchema(),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			const auto peer = session
				? ResolveLoadedPeer(session, arguments)
				: nullptr;
			if (!session || !peer) {
				if (session) {
					done(ToolError(u"PEER_NOT_FOUND"_q, u"Peer is not loaded."_q));
				}
				return;
			}
			const auto chat = peer->asChat();
			const auto channel = peer->asChannel();
			done({ .structuredContent = QJsonObject{
				{ u"account_id"_q, QString::number(session->uniqueId()) },
				{ u"peer_id"_q, QString::number(peer->id.value) },
				{ u"edit_information"_q, chat
					? chat->canEditInformation()
					: channel && channel->canEditInformation() },
				{ u"edit_permissions"_q, chat
					? chat->canEditPermissions()
					: channel && channel->canEditPermissions() },
				{ u"delete_messages"_q, chat
					? chat->canDeleteMessages()
					: channel && channel->canDeleteMessages() },
				{ u"add_members"_q, chat
					? chat->canAddMembers()
					: channel && channel->canAddMembers() },
				{ u"add_admins"_q, chat
					? chat->canAddAdmins()
					: channel && channel->canAddAdmins() },
				{ u"ban_members"_q, chat
					? chat->canBanMembers()
					: channel && channel->canBanMembers() },
			} });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.settings.list"_q,
		.description = u"List client settings exposed by the MCP semantic layer."_q,
		.inputSchema = EmptyObjectSchema(),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &, ToolCompletion done) {
			done({ .structuredContent = QJsonObject{
				{ u"settings"_q, QJsonArray{ QJsonObject{
					{ u"key"_q, u"mcp.port"_q },
					{ u"type"_q, u"integer"_q },
					{ u"minimum"_q, 1024 },
					{ u"maximum"_q, 65535 },
				} } },
			} });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.settings.get"_q,
		.description = u"Read one supported Forkgram client setting."_q,
		.inputSchema = StringArgumentSchema(u"key"_q, u"Setting key."_q),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto key = arguments.value(u"key"_q).toString();
			if (key != u"mcp.port"_q) {
				done(ToolError(
					u"SETTING_NOT_SUPPORTED"_q,
					u"This client setting is not exposed."_q));
				return;
			}
			done({ .structuredContent = QJsonObject{
				{ u"key"_q, key },
				{ u"value"_q, int(configuredPort()) },
			} });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.settings.set"_q,
		.description = u"Set one supported Forkgram client setting."_q,
		.inputSchema = {
			{ u"type"_q, u"object"_q },
			{ u"properties"_q, QJsonObject{
				{ u"key"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"value"_q, QJsonObject{
					{ u"type"_q, u"integer"_q },
					{ u"minimum"_q, 1024 },
					{ u"maximum"_q, 65535 },
				} },
			} },
			{ u"required"_q, QJsonArray{ u"key"_q, u"value"_q } },
			{ u"additionalProperties"_q, false },
		},
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto key = arguments.value(u"key"_q).toString();
			const auto value = arguments.value(u"value"_q).toInt();
			if (key != u"mcp.port"_q || value < 1024 || value > 65535) {
				done(ToolError(
					u"INVALID_SETTING"_q,
					u"Only mcp.port in the range 1024-65535 is supported."_q));
				return;
			}
			if (!rebind(value)) {
				done(ToolError(u"BIND_FAILED"_q, errorString()));
				return;
			}
			done({ .structuredContent = QJsonObject{
				{ u"key"_q, key },
				{ u"value"_q, value },
				{ u"endpoint"_q, endpoint() },
			} });
		},
	});
	_dispatcher.addTool({
		.name = u"telegram.updates.poll"_q,
		.description = u"Poll client updates after a monotonic cursor."_q,
		.inputSchema = {
			{ u"type"_q, u"object"_q },
			{ u"properties"_q, QJsonObject{
				{ u"cursor"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
				{ u"filters"_q, QJsonObject{
					{ u"type"_q, u"array"_q },
					{ u"items"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
				} },
				{ u"limit"_q, QJsonObject{
					{ u"type"_q, u"integer"_q },
					{ u"minimum"_q, 1 },
					{ u"maximum"_q, 100 },
				} },
				{ u"timeout_ms"_q, QJsonObject{
					{ u"type"_q, u"integer"_q },
					{ u"minimum"_q, 0 },
					{ u"maximum"_q, 30000 },
				} },
			} },
			{ u"additionalProperties"_q, false },
		},
		.outputSchema = ObjectSchema(),
		.cancellableHandler = [=](
				const QJsonObject &arguments,
				const CancellationPtr &cancellation,
				ToolCompletion done) {
			const auto session = RequireSession(_application, done);
			if (!session) {
				return;
			}
			const auto accountId = QString::number(session->uniqueId());
			auto cursorOk = false;
			const auto cursor = arguments.value(
				u"cursor"_q).toString().toULongLong(&cursorOk);
			const auto limit = std::clamp(
				arguments.value(u"limit"_q).toInt(50),
				1,
				100);
			const auto timeout = std::clamp(
				arguments.value(u"timeout_ms"_q).toInt(0),
				0,
				30000);
			auto filters = QSet<QString>();
			for (const auto &value : arguments.value(u"filters"_q).toArray()) {
				if (value.isString()) {
					filters.insert(value.toString());
				}
			}
			const auto after = cursorOk ? cursor : 0;
			auto events = updatesAfter(after, limit, accountId, filters);
			if (!events.isEmpty() || !timeout) {
				done({ .structuredContent = QJsonObject{
					{ u"account_id"_q, accountId },
					{ u"events"_q, std::move(events) },
					{ u"next_cursor"_q,
						QString::number(_nextUpdateCursor) },
				} });
				return;
			}
			const auto id = ++_nextWaiterId;
			_updateWaiters.push_back({
				.id = id,
				.cursor = after,
				.limit = limit,
				.accountId = accountId,
				.filters = std::move(filters),
				.done = std::move(done),
			});
			const auto weak = base::make_weak(this);
			watchUpdateWaiterSession(id, base::make_weak(session));
			cancellation->setHandler([=] {
				if (const auto service = weak.get()) {
					service->cancelUpdateWaiter(id);
				}
			});
			QTimer::singleShot(timeout, _application, [=] {
				if (const auto service = weak.get()) {
					service->finishUpdateWaiter(id);
				}
			});
		},
	});
}

void Service::watchUpdates() {
	_application->passcodeLockValue(
	) | rpl::on_next([=](bool locked) {
		pushUpdate(u"lock_changed"_q, {
			{ u"locked"_q, locked },
		});
	}, _updatesLifetime);
	_application->domain().activeSessionValue(
	) | rpl::on_next([=](Main::Session *session) {
		watchSession(session);
	}, _updatesLifetime);
}

void Service::watchSession(Main::Session *session) {
	_activeSessionLifetime.destroy();
	pushUpdate(u"active_session_changed"_q, {
		{ u"account_id"_q, session
			? QJsonValue(QString::number(session->uniqueId()))
			: QJsonValue(QJsonValue::Null) },
	});
	if (!session) {
		return;
	}
	session->data().itemDataChanges(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		pushUpdate(u"message_changed"_q, MessageJson(item));
	}, _activeSessionLifetime);
	session->data().itemRemoved(
	) | rpl::on_next([=](not_null<const HistoryItem*> item) {
		pushUpdate(u"message_removed"_q, {
			{ u"account_id"_q, QString::number(session->uniqueId()) },
			{ u"peer_id"_q,
				QString::number(item->history()->peer->id.value) },
			{ u"message_id"_q, QString::number(item->id.bare) },
		});
	}, _activeSessionLifetime);
	session->data().historyCleared(
	) | rpl::on_next([=](not_null<const History*> history) {
		pushUpdate(u"history_cleared"_q, {
			{ u"account_id"_q, QString::number(session->uniqueId()) },
			{ u"peer_id"_q,
				QString::number(history->peer->id.value) },
		});
	}, _activeSessionLifetime);
}

void Service::pushUpdate(QString type, QJsonObject data) {
	const auto cursor = ++_nextUpdateCursor;
	_updates.push_back({
		.cursor = cursor,
		.type = std::move(type),
		.data = std::move(data),
	});
	while (_updates.size() > 1000) {
		_updates.pop_front();
	}
	_server.publish(u"notifications/resources/updated"_q, {
		{ u"uri"_q, u"telegram://updates?cursor=%1"_q.arg(cursor) },
	});
	flushUpdateWaiters();
}

QJsonArray Service::updatesAfter(
		quint64 cursor,
		int limit,
		const QString &accountId,
		const QSet<QString> &filters) const {
	auto result = QJsonArray();
	for (const auto &event : _updates) {
		if (event.cursor <= cursor
			|| (!filters.isEmpty() && !filters.contains(event.type))
			|| (event.type != u"lock_changed"_q
				&& event.type != u"active_session_changed"_q
				&& event.data.value(u"account_id"_q).isString()
				&& event.data.value(u"account_id"_q).toString() != accountId)) {
			continue;
		}
		result.append(QJsonObject{
			{ u"cursor"_q, QString::number(event.cursor) },
			{ u"type"_q, event.type },
			{ u"data"_q, event.data },
		});
		if (result.size() >= limit) {
			break;
		}
	}
	return result;
}

void Service::flushUpdateWaiters() {
	for (auto i = _updateWaiters.begin(); i != _updateWaiters.end();) {
		auto events = updatesAfter(
			i->cursor,
			i->limit,
			i->accountId,
			i->filters);
		if (events.isEmpty()) {
			++i;
			continue;
		}
		auto done = std::move(i->done);
		const auto accountId = i->accountId;
		i = _updateWaiters.erase(i);
		done({ .structuredContent = QJsonObject{
			{ u"account_id"_q, accountId },
			{ u"events"_q, std::move(events) },
			{ u"next_cursor"_q, QString::number(_nextUpdateCursor) },
		} });
	}
}

void Service::finishUpdateWaiter(quint64 id) {
	const auto i = ranges::find(_updateWaiters, id, &UpdateWaiter::id);
	if (i == end(_updateWaiters)) {
		return;
	}
	auto done = std::move(i->done);
	const auto accountId = i->accountId;
	_updateWaiters.erase(i);
	done({ .structuredContent = QJsonObject{
		{ u"account_id"_q, accountId },
		{ u"events"_q, QJsonArray() },
		{ u"next_cursor"_q, QString::number(_nextUpdateCursor) },
	} });
}

void Service::cancelUpdateWaiter(quint64 id) {
	const auto i = ranges::find(_updateWaiters, id, &UpdateWaiter::id);
	if (i != end(_updateWaiters)) {
		_updateWaiters.erase(i);
	}
}

void Service::watchUpdateWaiterSession(
		quint64 id,
		base::weak_ptr<Main::Session> session) {
	const auto weak = base::make_weak(this);
	QTimer::singleShot(kSessionWatchInterval, _application, [=] {
		const auto service = weak.get();
		if (!service) {
			return;
		}
		const auto i = ranges::find(
			service->_updateWaiters,
			id,
			&UpdateWaiter::id);
		if (i == end(service->_updateWaiters)) {
			return;
		} else if (session && !service->_application->passcodeLocked()) {
			service->watchUpdateWaiterSession(id, session);
			return;
		}
		auto done = std::move(i->done);
		service->_updateWaiters.erase(i);
		done(service->_application->passcodeLocked()
			? ToolError(
				u"APP_LOCKED"_q,
				u"Forkgram was locked during the request."_q)
			: ToolError(
				u"SESSION_GONE"_q,
				u"The captured Telegram session ended."_q));
	});
}

ToolResult Service::clientState() const {
	const auto session = _application->maybePrimarySession();
	auto result = QJsonObject{
		{ u"launched"_q, true },
		{ u"locked"_q, _application->passcodeLocked() },
		{ u"authenticated"_q, (session != nullptr) },
		{ u"mcp_available"_q, available() },
		{ u"mcp_enabled"_q, enabled() },
		{ u"authentication_required"_q, authenticationEnabled() },
		{ u"enabled_tool_count"_q, enabledToolCount() },
		{ u"total_tool_count"_q, totalToolCount() },
		{ u"endpoint"_q, endpoint() },
	};
	result.insert(
		u"account_id"_q,
		session
			? QJsonValue(QString::number(session->uniqueId()))
			: QJsonValue(QJsonValue::Null));
	return { .structuredContent = std::move(result) };
}

} // namespace Core::Mcp
