/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_unread_summaries.h"

#include "apiwrap.h"
#include "base/qthelp_url.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "data/data_document.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_history_messages.h"
#include "data/data_message_reaction_id.h"
#include "data/data_peer.h"
#include "data/data_replies_list.h"
#include "data/data_saved_messages.h"
#include "data/data_saved_sublist.h"
#include "data/data_session.h"
#include "data/data_messages.h"
#include "data/stickers/data_custom_emoji.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "lang/lang_keys.h"
#include "ui/text/text_utilities.h"
#include "main/main_session.h"
#include "ui/toast/toast.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <algorithm>

namespace Api {
namespace {

constexpr auto kMessagesAfterUnreadLimit = 1000000;
constexpr auto kErrorToastDuration = 8 * crl::time(1000);

struct TranscriptReaction {
	QString emoji;
	int count = 0;
	int order = 0;
};

[[nodiscard]] QString NormalizeFailureDetails(QString text) {
	text.replace(u"\r\n"_q, u"\n"_q);
	text.replace(u'\r', u'\n');
	text = text.trimmed();
	if (text.isEmpty()) {
		return QString();
	}
	auto lines = text.split(u'\n');
	if (lines.size() > 12) {
		lines = lines.mid(0, 12);
		lines.push_back(u"..."_q);
	}
	text = lines.join(u'\n');
	if (text.size() > 1200) {
		text = text.left(1197) + u"..."_q;
	}
	return text;
}

[[nodiscard]] QString JoinFailureDetails(std::initializer_list<QString> parts) {
	auto result = QStringList();
	for (auto part : parts) {
		part = NormalizeFailureDetails(std::move(part));
		if (!part.isEmpty() && !result.contains(part)) {
			result.push_back(std::move(part));
		}
	}
	return result.join(u"\n"_q);
}

[[nodiscard]] bool InvalidSummaryApiBaseUrl(QString value) {
	const auto validated = qthelp::validate_url(value.trimmed());
	if (validated.isEmpty()) {
		return true;
	}
	const auto url = QUrl(validated);
	return !url.isValid()
		|| url.scheme().isEmpty()
		|| !url.path().contains(u"/v1"_q);
}

[[nodiscard]] QString SummaryEndpoint(QString value) {
	value = qthelp::validate_url(value.trimmed());
	if (value.endsWith('/')) {
		value.chop(1);
	}
	return value + u"/chat/completions"_q;
}

[[nodiscard]] QString NormalizeTextForTranscript(QString text) {
	text.replace(u"\r\n"_q, u"\n"_q);
	text.replace(u'\r', u'\n');
	text.replace(u'\n', u"\\n"_q);
	return text;
}

[[nodiscard]] QString PaidReactionEmoji() {
	return QString::fromUtf8("\xe2\xad\x90");
}

[[nodiscard]] QString NormalizeAuthor(QString text) {
	text.replace(u'\n', u' ');
	text.replace(u'\r', u' ');
	return text.trimmed();
}

[[nodiscard]] QString AuthorName(not_null<HistoryItem*> item) {
	if (const auto postAuthor = item->originalPostAuthor();
		!postAuthor.isEmpty()) {
		return NormalizeAuthor(postAuthor);
	} else if (const auto hidden = item->displayHiddenSenderInfo()) {
		return NormalizeAuthor(hidden->name);
	}
	return NormalizeAuthor(item->author()->shortName());
}

[[nodiscard]] QString CustomReactionEmoji(
		not_null<HistoryItem*> item,
		DocumentId customId) {
	const auto document = item->history()->owner().document(customId);
	const auto sticker = document->sticker();
	return sticker ? sticker->alt : QString();
}

[[nodiscard]] QString ReactionEmoji(
		not_null<HistoryItem*> item,
		const Data::ReactionId &id) {
	if (id.paid()) {
		return PaidReactionEmoji();
	} else if (const auto emoji = id.emoji(); !emoji.isEmpty()) {
		return emoji;
	} else if (const auto customId = id.custom()) {
		return CustomReactionEmoji(item, customId);
	}
	return QString();
}

[[nodiscard]] QString FormatReactionsForTranscript(
		not_null<HistoryItem*> item) {
	if (item->reactionsAreTags()) {
		return QString();
	}
	auto list = std::vector<TranscriptReaction>();
	for (const auto &reaction : item->reactions()) {
		if (reaction.count <= 0) {
			continue;
		}
		const auto emoji = ReactionEmoji(item, reaction.id);
		if (emoji.isEmpty()) {
			continue;
		}
		const auto i = ranges::find(list, emoji, &TranscriptReaction::emoji);
		if (i != end(list)) {
			i->count += reaction.count;
		} else {
			list.push_back({
				.emoji = emoji,
				.count = reaction.count,
				.order = int(list.size()),
			});
		}
	}
	if (list.empty()) {
		return QString();
	}
	ranges::sort(list, [](const auto &a, const auto &b) {
		if (a.count != b.count) {
			return a.count > b.count;
		}
		return a.order < b.order;
	});

	auto parts = QStringList();
	parts.reserve(int(list.size()));
	for (const auto &reaction : list) {
		parts.push_back(reaction.emoji
			+ ((reaction.count > 1)
				? u" x%1"_q.arg(reaction.count)
				: QString()));
	}
	return u" [%1: %2]"_q
		.arg(tr::lng_notification_reactions(tr::now))
		.arg(parts.join(u", "_q));
}

[[nodiscard]] QString TextForTranscript(not_null<HistoryItem*> item) {
	return NormalizeTextForTranscript(item->originalText().text)
		+ FormatReactionsForTranscript(item);
}

[[nodiscard]] bool ItemHasTextForTranscript(not_null<HistoryItem*> item) {
	return !item->isService()
		&& !item->originalText().text.trimmed().isEmpty();
}

[[nodiscard]] std::vector<DocumentId> UnresolvedCustomReactionIds(
		not_null<Data::Thread*> thread,
		const Data::MessagesSlice &slice) {
	auto result = std::vector<DocumentId>();
	for (const auto &id : slice.ids) {
		const auto item = thread->owner().message(id);
		if (!item
			|| item->isUnreadSummary()
			|| item->reactionsAreTags()
			|| !ItemHasTextForTranscript(item)) {
			continue;
		}
		for (const auto &reaction : item->reactions()) {
			const auto customId = reaction.id.custom();
			if (!customId || reaction.count <= 0) {
				continue;
			}
			const auto document = thread->owner().document(customId);
			if (!document->sticker()
				&& !ranges::contains(result, customId)) {
				result.push_back(customId);
			}
		}
	}
	return result;
}

[[nodiscard]] QString ExtractTextPart(const QJsonValue &value) {
	if (value.isString()) {
		return value.toString();
	} else if (!value.isObject()) {
		return QString();
	}
	const auto object = value.toObject();
	if (const auto text = object.value(u"text"_q); text.isString()) {
		return text.toString();
	} else if (text.isObject()) {
		const auto textObject = text.toObject();
		if (const auto nested = textObject.value(u"value"_q); nested.isString()) {
			return nested.toString();
		}
	}
	if (const auto content = object.value(u"content"_q); content.isString()) {
		return content.toString();
	}
	return QString();
}

[[nodiscard]] QString ExtractMessageContent(const QJsonValue &value) {
	if (value.isString()) {
		return value.toString().trimmed();
	} else if (!value.isArray()) {
		return QString();
	}
	auto result = QString();
	for (const auto &part : value.toArray()) {
		result += ExtractTextPart(part);
	}
	return result.trimmed();
}

[[nodiscard]] QString ParseSuccessContent(const QByteArray &body) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(body, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return QString();
	}
	const auto root = document.object();
	const auto choices = root.value(u"choices"_q).toArray();
	if (choices.isEmpty() || !choices[0].isObject()) {
		return QString();
	}
	const auto choice = choices[0].toObject();
	const auto message = choice.value(u"message"_q).toObject();
	return ExtractMessageContent(message.value(u"content"_q));
}

[[nodiscard]] QString ParseErrorDetails(const QByteArray &body) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(body, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return QString();
	}
	const auto root = document.object();
	const auto value = root.value(u"error"_q);
	if (value.isString()) {
		return value.toString();
	} else if (value.isObject()) {
		const auto object = value.toObject();
		return JoinFailureDetails({
			object.value(u"message"_q).toString(),
			object.value(u"type"_q).toString().isEmpty()
				? QString()
				: u"Type: %1"_q.arg(object.value(u"type"_q).toString()),
			object.value(u"code"_q).toString().isEmpty()
				? QString()
				: u"Code: %1"_q.arg(object.value(u"code"_q).toString()),
		});
	}
	if (const auto message = root.value(u"message"_q); message.isString()) {
		return message.toString();
	}
	return QString();
}

[[nodiscard]] QString ParseBodySnippet(const QByteArray &body) {
	return NormalizeFailureDetails(QString::fromUtf8(body));
}

[[nodiscard]] QString FailureTitle(UnreadSummaries::Failure failure) {
	switch (failure) {
	case UnreadSummaries::Failure::Config:
		return u"Unread Summary Setup Required"_q;
	case UnreadSummaries::Failure::NoText:
		return u"Nothing To Summarize"_q;
	case UnreadSummaries::Failure::Network:
	case UnreadSummaries::Failure::Parse:
	case UnreadSummaries::Failure::Inject:
		return u"Unread Summary Failed"_q;
	}
	Unexpected("Failure in FailureTitle.");
}

[[nodiscard]] QString SummaryTitle(
		TimeId fromDate,
		TimeId tillDate,
		int includedMessages) {
	if (includedMessages > 0) {
		if (!fromDate || !tillDate) {
			return tr::lng_fork_unread_summary_title_count(
				tr::now,
				lt_count,
				includedMessages);
		}
		return tr::lng_fork_unread_summary_title_range_count(
			tr::now,
			lt_count,
			includedMessages,
			lt_from,
			langDateTime(base::unixtime::parse(fromDate)),
			lt_to,
			langDateTime(base::unixtime::parse(tillDate)));
	}
	if (!fromDate || !tillDate) {
		return tr::lng_fork_unread_summary_title(tr::now);
	}
	return tr::lng_fork_unread_summary_title_range(
		tr::now,
		lt_from,
		langDateTime(base::unixtime::parse(fromDate)),
		lt_to,
		langDateTime(base::unixtime::parse(tillDate)));
}

[[nodiscard]] TextWithEntities ComposeSummaryText(
		QString summary,
		TimeId fromDate,
		TimeId tillDate,
		int includedMessages) {
	summary = summary.trimmed();

	auto result = TextWithEntities();
	const auto title = SummaryTitle(fromDate, tillDate, includedMessages);
	result.text = title;
	result.entities.push_back(EntityInText(
		EntityType::Bold,
		0,
		title.size()));
	if (!summary.isEmpty()) {
		result.append(u"\n\n"_q);
		auto body = Ui::Text::RichLangValue(summary);
		TextUtilities::ParseEntities(
			body,
			TextParseLinks | TextParseMultiline);
		result.append(std::move(body));
	}
	return result;
}

void EnsureServiceNotificationsUser(not_null<Main::Session*> session) {
	if (session->data().peerLoaded(PeerData::kServiceNotificationsId)) {
		return;
	}
	session->data().processUser(MTP_user(
		MTP_flags(
			MTPDuser::Flag::f_first_name
			| MTPDuser::Flag::f_phone
			| MTPDuser::Flag::f_status
			| MTPDuser::Flag::f_verified),
		MTP_long(peerToUser(PeerData::kServiceNotificationsId).bare),
		MTPlong(),
		MTP_string("Telegram"),
		MTPstring(),
		MTPstring(),
		MTP_string("42777"),
		MTP_userProfilePhotoEmpty(),
		MTP_userStatusRecently(MTP_flags(0)),
		MTPint(),
		MTPVector<MTPRestrictionReason>(),
		MTPstring(),
		MTPstring(),
		MTPEmojiStatus(),
		MTPVector<MTPUsername>(),
		MTPRecentStory(),
		MTPPeerColor(),
		MTPPeerColor(),
		MTPint(),
		MTPlong(),
		MTPlong(),
		MTPlong()));
}

[[nodiscard]] QString FailureText(UnreadSummaries::Failure failure) {
	switch (failure) {
	case UnreadSummaries::Failure::Config:
		return u"Check summarization settings in Fork."_q;
	case UnreadSummaries::Failure::NoText:
		return u"No unread text messages to summarize."_q;
	case UnreadSummaries::Failure::Network:
		return u"Summarization request failed."_q;
	case UnreadSummaries::Failure::Parse:
		return u"Summarization response was invalid."_q;
	case UnreadSummaries::Failure::Inject:
		return u"Summary arrived, but it could not be shown in this chat."_q;
	}
	Unexpected("Failure in FailureText.");
}

[[nodiscard]] bool ThreadContainsItem(
		not_null<const Data::Thread*> thread,
		not_null<const HistoryItem*> item) {
	if (const auto topic = dynamic_cast<const Data::ForumTopic*>(thread.get())) {
		return item->topic() == topic;
	} else if (const auto sublist = dynamic_cast<const Data::SavedSublist*>(
			thread.get())) {
		return item->savedSublist() == sublist;
	} else if (const auto history = dynamic_cast<const History*>(thread.get())) {
		return (item->history() == history)
			&& !item->topic()
			&& !item->savedSublist();
	}
	return false;
}

[[nodiscard]] QString SystemPrompt() {
	return u"You summarize Telegram chat transcripts. First infer the "
		"dominant language of the transcript from substantive participant "
		"messages. Write the entire answer in that language; do not default "
		"to English unless English is dominant. If languages are mixed, use "
		"the language used most in the transcript, or the language of the "
		"latest substantial messages if tied.\n\n"
		"Some transcript lines may end with a localized reactions marker, "
		"for example [Reactions: ...]. Treat these as audience reaction "
		"signals for that specific message. Consider both the emoji meaning "
		"and count: hearts, thumbs up, fire, and stars usually indicate "
		"support or approval, while clown, poop, or negative emoji may "
		"indicate mockery, disagreement, or controversy.\n\n"
		"Output only the summary in plain Markdown:\n"
		"1. A short overview bullet list: one concise sentence for each "
		"significant topic; include as many significant topics as needed, "
		"omit minor side remarks.\n"
		"2. A conversation dynamics section: 2-5 concise sentences focused "
		"on who participated, their positions, relationships, mood, "
		"agreements/disagreements, support, conflict, and notable outliers. "
		"Use reactions when they clarify the mood or approval around a "
		"message. Do not repeat the overview except where context is "
		"necessary.\n\n"
		"Do not invent facts."_q;
}

} // namespace

UnreadSummaries::UnreadSummaries(not_null<ApiWrap*> api)
: _session(&api->session())
, _network(new QNetworkAccessManager()) {
}

UnreadSummaries::~UnreadSummaries() {
	for (auto &[key, state] : _states) {
		clearPending(state);
	}
	delete _network;
}

UnreadSummaries::ThreadKey UnreadSummaries::Key(
		not_null<const Data::Thread*> thread) {
	return ThreadKey{
		.peerId = thread->peer()->id,
		.topicRootId = thread->topicRootId(),
		.monoforumPeerId = thread->monoforumPeerId(),
	};
}

bool UnreadSummaries::configured() const {
	const auto &fork = Core::App().settings().fork();
	return !InvalidSummaryApiBaseUrl(fork.summaryApiBaseUrl())
		&& !fork.summaryModel().trimmed().isEmpty();
}

bool UnreadSummaries::loading(not_null<const Data::Thread*> thread) const {
	if (const auto found = lookup(Key(thread))) {
		return found->entry.loading;
	}
	return false;
}

bool UnreadSummaries::shown(not_null<const Data::Thread*> thread) const {
	const auto found = lookup(Key(thread));
	if (!found
		|| !found->entry.shownItemId
		|| found->entry.shownVersion != found->entry.version) {
		return false;
	}
	const auto item = thread->owner().message(found->entry.shownItemId);
	return item && ThreadContainsItem(thread, item);
}

const UnreadSummaries::Entry &UnreadSummaries::entry(
		not_null<const Data::Thread*> thread) const {
	static const auto kEmpty = Entry();
	if (const auto found = lookup(Key(thread))) {
		return found->entry;
	}
	return kEmpty;
}

rpl::producer<UnreadSummaries::ThreadKey> UnreadSummaries::changes() const {
	return _changes.events();
}

UnreadSummaries::StartResult UnreadSummaries::request(
		not_null<Data::Thread*> thread) {
	const auto key = Key(thread);
	auto &current = state(key);
	if (current.entry.loading) {
		return StartResult::AlreadyLoading;
	} else if (!configured()) {
		showToast(key, Failure::Config, QString());
		return StartResult::InvalidConfig;
	}

	clearPending(current);
	current.entry.loading = true;
	current.requestToken++;
	_changes.fire_copy(key);

	const auto token = current.requestToken;
	SourceForThread(thread) | rpl::filter([=](const Data::MessagesSlice &slice) {
		return slice.skippedAfter == 0;
	}) | rpl::take(1) | rpl::on_next([=](const Data::MessagesSlice &slice) {
		if (const auto resolved = resolveThread(key)) {
			prepareTranscriptAndStartNetworkRequest(key, token, resolved, slice);
		} else {
			failRequest(
				key,
				Failure::Inject,
				u"The chat thread was not available to prepare a summary."_q);
		}
	}, *current.lifetime);

	return StartResult::Started;
}

void UnreadSummaries::restore(not_null<Data::Thread*> thread) {
	auto &current = state(Key(thread));
	if (current.entry.loading
		|| current.entry.lastSummaryText.empty()) {
		return;
	}
	if (current.entry.shownItemId) {
		if (const auto item = thread->owner().message(current.entry.shownItemId)) {
			if (current.entry.shownVersion == current.entry.version
				&& ThreadContainsItem(thread, item)) {
				return;
			}
			item->history()->destroyMessage(item);
		}
	}
	current.entry.shownItemId = injectSummary(
		thread,
		current.entry.lastSummaryText);
	if (const auto item = thread->owner().message(current.entry.shownItemId)) {
		if (ThreadContainsItem(thread, item)) {
			current.entry.shownVersion = current.entry.version;
			return;
		}
		item->history()->destroyMessage(item);
	}
	current.entry.shownItemId = FullMsgId();
	current.entry.shownVersion = 0;
}

rpl::producer<Data::MessagesSlice> UnreadSummaries::SourceForThread(
		not_null<Data::Thread*> thread) {
	if (const auto topic = dynamic_cast<Data::ForumTopic*>(thread.get())) {
		return topic->replies()->source(
			Data::UnreadMessagePosition,
			0,
			kMessagesAfterUnreadLimit);
	} else if (const auto sublist = dynamic_cast<Data::SavedSublist*>(
			thread.get())) {
		return sublist->source(
			Data::UnreadMessagePosition,
			0,
			kMessagesAfterUnreadLimit);
	} else if (const auto history = dynamic_cast<History*>(thread.get())) {
		return Data::HistoryMessagesViewer(
			history,
			Data::UnreadMessagePosition,
			0,
			kMessagesAfterUnreadLimit);
	}
	Unexpected("Thread type in UnreadSummaries::SourceForThread.");
}

UnreadSummaries::PreparedTranscript UnreadSummaries::BuildTranscript(
		not_null<Data::Thread*> thread,
		const Data::MessagesSlice &slice) {
	auto rangeItems = std::vector<not_null<HistoryItem*>>();
	auto textItems = std::vector<not_null<HistoryItem*>>();
	rangeItems.reserve(slice.ids.size());
	textItems.reserve(slice.ids.size());
	for (const auto &id : slice.ids) {
		const auto item = thread->owner().message(id);
		if (!item || item->isUnreadSummary()) {
			continue;
		}
		rangeItems.push_back(item);
		if (item->isService()) {
			continue;
		}
		if (!ItemHasTextForTranscript(item)) {
			continue;
		}
		textItems.push_back(item);
	}
	auto byPosition = [](const auto &a, const auto &b) {
		return a->position() < b->position();
	};
	std::sort(begin(rangeItems), end(rangeItems), byPosition);
	std::sort(begin(textItems), end(textItems), byPosition);

	auto result = PreparedTranscript();
	if (rangeItems.empty() || textItems.empty()) {
		return result;
	}
	result.rangeFromDate = rangeItems.front()->date();
	result.rangeTillDate = rangeItems.back()->date();
	result.includedMessages = int(textItems.size());

	auto lines = QStringList();
	lines.reserve(int(textItems.size()));
	for (const auto &item : textItems) {
		lines.push_back(u"%1, %2: %3"_q
			.arg(langDateTime(base::unixtime::parse(item->date())))
			.arg(AuthorName(item))
			.arg(TextForTranscript(item)));
	}
	result.text = lines.join(u'\n');
	return result;
}

void UnreadSummaries::prepareTranscriptAndStartNetworkRequest(
		const ThreadKey &key,
		int token,
		not_null<Data::Thread*> thread,
		const Data::MessagesSlice &slice) {
	const auto unresolved = UnresolvedCustomReactionIds(thread, slice);
	if (unresolved.empty()) {
		buildTranscriptAndStartNetworkRequest(key, token, thread, slice);
		return;
	}

	struct ResolveState {
		Data::MessagesSlice slice;
		int remaining = 0;
	};

	auto &current = state(key);
	const auto resolveState = current.lifetime->make_state<ResolveState>();
	resolveState->slice = slice;
	resolveState->remaining = int(unresolved.size());

	const auto resolved = [=] {
		const auto found = lookup(key);
		if (!found || found->requestToken != token) {
			return;
		}
		if (--resolveState->remaining > 0) {
			return;
		}
		if (const auto resolved = resolveThread(key)) {
			buildTranscriptAndStartNetworkRequest(
				key,
				token,
				resolved,
				resolveState->slice);
		} else {
			failRequest(
				key,
				Failure::Inject,
				u"The chat thread was not available to prepare a summary."_q);
		}
	};

	for (const auto customId : unresolved) {
		thread->owner().customEmojiManager().resolve(
			customId
		) | rpl::take(
			1
		) | rpl::on_next_error(
			[=](not_null<DocumentData*>) {
				resolved();
			},
			[=](rpl::empty_error) {
				resolved();
			},
			*current.lifetime);
	}
}

void UnreadSummaries::buildTranscriptAndStartNetworkRequest(
		const ThreadKey &key,
		int token,
		not_null<Data::Thread*> thread,
		const Data::MessagesSlice &slice) {
	const auto found = lookup(key);
	if (!found || found->requestToken != token) {
		return;
	}

	const auto transcript = BuildTranscript(thread, slice);
	if (transcript.empty()) {
		failRequest(key, Failure::NoText);
		return;
	}
	startNetworkRequest(key, transcript);
}

void UnreadSummaries::startNetworkRequest(
		const ThreadKey &key,
		PreparedTranscript transcript) {
	const auto &fork = Core::App().settings().fork();
	auto request = QNetworkRequest(QUrl(SummaryEndpoint(
		fork.summaryApiBaseUrl())));
	request.setHeader(
		QNetworkRequest::ContentTypeHeader,
		QByteArray("application/json"));
	if (const auto apiKey = fork.summaryApiKey().trimmed(); !apiKey.isEmpty()) {
		request.setRawHeader(
			"Authorization",
			("Bearer " + apiKey).toUtf8());
	}

	const auto body = QJsonDocument(QJsonObject{
		{ u"model"_q, fork.summaryModel().trimmed() },
		{ u"messages"_q, QJsonArray{
			QJsonObject{
				{ u"role"_q, u"system"_q },
				{ u"content"_q, SystemPrompt() },
			},
			QJsonObject{
				{ u"role"_q, u"user"_q },
				{ u"content"_q, std::move(transcript.text) },
			},
		} },
	}).toJson(QJsonDocument::Compact);

	auto &current = state(key);
	const auto token = current.requestToken;
	const auto rangeFromDate = transcript.rangeFromDate;
	const auto rangeTillDate = transcript.rangeTillDate;
	const auto includedMessages = transcript.includedMessages;
	current.reply = _network->post(request, body);
	QObject::connect(current.reply, &QNetworkReply::finished, [=] {
		const auto found = lookup(key);
		if (!found || found->requestToken != token || !found->reply) {
			return;
		}
		const auto reply = found->reply;
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto networkError = reply->error();
		const auto bytes = reply->readAll();
		reply->deleteLater();

		auto &state = this->state(key);
		state.reply = nullptr;
		state.lifetime->destroy();
		state.lifetime = std::make_unique<rpl::lifetime>();

		if (networkError != QNetworkReply::NoError
			|| status < 200
			|| status >= 300) {
			failRequest(
				key,
				Failure::Network,
				JoinFailureDetails({
					status
						? u"HTTP %1"_q.arg(status)
						: QString(),
					reply->errorString(),
					ParseErrorDetails(bytes),
					ParseErrorDetails(bytes).isEmpty()
						? ParseBodySnippet(bytes)
						: QString(),
				}));
			return;
		}

		const auto content = ParseSuccessContent(bytes);
		if (content.isEmpty()) {
			failRequest(
				key,
				Failure::Parse,
				JoinFailureDetails({
					ParseErrorDetails(bytes),
					u"No text was found in choices[0].message.content."_q,
					ParseErrorDetails(bytes).isEmpty()
						? ParseBodySnippet(bytes)
						: QString(),
				}));
			return;
		}

		const auto formatted = ComposeSummaryText(
			content,
			rangeFromDate,
			rangeTillDate,
			includedMessages);
		const auto previousText = state.entry.lastSummaryText;
		const auto previousVersion = state.entry.version;
		const auto previousShownItemId = state.entry.shownItemId;
		const auto previousShownVersion = state.entry.shownVersion;
		const auto version = state.entry.version + 1;
		state.entry.lastSummaryText = formatted;
		state.entry.version = version;
		if (const auto thread = resolveThread(key)) {
			const auto shownItemId = injectSummary(thread, formatted);
			const auto item = thread->owner().message(shownItemId);
			if (!item || !ThreadContainsItem(thread, item)) {
				if (item) {
					item->history()->destroyMessage(item);
				}
				state.entry.lastSummaryText = previousText;
				state.entry.version = previousVersion;
				state.entry.shownItemId = previousShownItemId;
				state.entry.shownVersion = previousShownVersion;
				failRequest(
					key,
					Failure::Inject,
					u"The summary message was created outside the current chat thread."_q);
				return;
			}
			if (const auto history = resolveHistory(key)) {
				if (const auto item = history->owner().message(
						previousShownItemId)) {
					history->destroyMessage(item);
				}
			}
			state.entry.shownItemId = shownItemId;
			state.entry.shownVersion = version;
		}
		finishLoading(key);
	});
}

void UnreadSummaries::failRequest(
		const ThreadKey &key,
		Failure failure,
		QString details) {
	finishLoading(key);
	showToast(key, failure, std::move(details));
}

void UnreadSummaries::finishLoading(const ThreadKey &key) {
	auto &current = state(key);
	current.entry.loading = false;
	clearPending(current);
	_changes.fire_copy(key);
}

void UnreadSummaries::clearPending(State &state) {
	if (state.lifetime) {
		state.lifetime->destroy();
	}
	state.lifetime = std::make_unique<rpl::lifetime>();
	if (state.reply) {
		QObject::disconnect(state.reply, nullptr, nullptr, nullptr);
		state.reply->abort();
		state.reply->deleteLater();
		state.reply = nullptr;
	}
}

void UnreadSummaries::showToast(
		const ThreadKey &key,
		Failure failure,
		const QString &details) {
	if (const auto history = resolveHistory(key)) {
		if (const auto window = Core::App().windowForShowingHistory(
				history->peer)) {
			if (const auto controller = window->sessionController()) {
				controller->showToast(Ui::Toast::Config{
					.title = FailureTitle(failure),
					.text = TextWithEntities{
						.text = details.isEmpty()
							? FailureText(failure)
							: (FailureText(failure) + u"\n\n"_q + details),
					},
					.maxlines = 16,
					.duration = kErrorToastDuration,
				});
			}
		}
	}
}

History *UnreadSummaries::resolveHistory(const ThreadKey &key) const {
	return key.peerId ? _session->data().history(key.peerId).get() : nullptr;
}

Data::Thread *UnreadSummaries::resolveThread(const ThreadKey &key) const {
	const auto history = resolveHistory(key);
	if (!history) {
		return nullptr;
	} else if (!key.topicRootId && !key.monoforumPeerId) {
		return history;
	} else if (key.topicRootId) {
		const auto forum = history->peer->forum();
		return forum ? forum->enforceTopicFor(key.topicRootId) : nullptr;
	} else if (key.monoforumPeerId) {
		const auto monoforum = history->peer->monoforum();
		return monoforum
			? monoforum->sublist(
				_session->data().peer(key.monoforumPeerId)).get()
			: nullptr;
	}
	Unexpected("Thread key in UnreadSummaries::resolveThread.");
}

UnreadSummaries::State &UnreadSummaries::state(const ThreadKey &key) {
	auto &result = _states[key];
	if (!result.lifetime) {
		result.lifetime = std::make_unique<rpl::lifetime>();
	}
	return result;
}

const UnreadSummaries::State *UnreadSummaries::lookup(
		const ThreadKey &key) const {
	const auto i = _states.find(key);
	return (i == end(_states)) ? nullptr : &i->second;
}

FullMsgId UnreadSummaries::injectSummary(
		not_null<Data::Thread*> thread,
		const TextWithEntities &text) {
	EnsureServiceNotificationsUser(_session);
	const auto history = thread->owningHistory();
	auto flags = MessageFlags(MessageFlag::HasFromId);
	auto replyTo = FullReplyTo();
	if (const auto topicRootId = thread->topicRootId()) {
		flags |= MessageFlag::HasReplyInfo;
		replyTo.messageId = { history->peer->id, topicRootId };
		replyTo.topicRootId = topicRootId;
	}
	if (const auto monoforumPeerId = thread->monoforumPeerId()) {
		flags |= MessageFlag::HasReplyInfo;
		replyTo.monoforumPeerId = monoforumPeerId;
	}
	const auto item = history->addNewLocalMessage({
		.id = history->owner().nextLocalMessageId(),
		.flags = flags,
		.from = PeerData::kServiceNotificationsId,
		.replyTo = replyTo,
		.date = base::unixtime::now(),
		.unreadSummary = true,
	}, text, MTP_messageMediaEmpty());
	return item->fullId();
}

} // namespace Api
