#include "boxes/share_box.h"

#include "api/api_common.h"
#include "api/api_text_entities.h"
#include "api/message_share_preparation.h"
#include "api/message_share_send.h"
#include "apiwrap.h"
#include "base/random.h"
#include "boxes/peer_list_controllers.h"
#include "data/business/data_shortcut_messages.h"
#include "data/components/ephemeral_messages.h"
#include "data/data_channel.h"
#include "data/data_chat_participant_status.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_game.h"
#include "data/data_histories.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "data/data_premium_limits.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "history/history_item_text.h"
#include "history/view/controls/history_view_forward_panel.h"
#include "iv/iv_rich_page.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "main/session/session_show.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/padding_wrap.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace {

using Api::MessageShare::Delivery;
using Api::MessageShare::Acknowledgement;
using Completion = Api::MessageShare::DeliveryQueue::Completion;

using Api::MessageShare::Mode;

struct Source {
	FullMsgId id;
	uint64 sessionId = 0;
	int nativeId = 0;
	MessageGroupId group;
	TextWithEntities text;
	TextWithEntities representation;
	DocumentId documentId = 0;
	PhotoId photoId = 0;
	std::optional<MTPInputMedia> media;
	ChatRestriction right = ChatRestriction::SendOther;
	bool resolve = false;
	bool forward = false;
	bool ephemeral = false;
	bool caption = false;
	bool textual = false;
	bool spoiler = false;
	bool inlineRight = false;
	QString link;
	std::vector<Source> richParts;
};

void CaptureMedia(Source &source, DocumentData *document, PhotoData *photo) {
	if (document && !document->uploading()) {
		using Flag = MTPDinputMediaDocument::Flag;
		const auto cover = document->goodThumbnailPhoto();
		source.documentId = document->id;
		source.media = MTP_inputMediaDocument(
			MTP_flags((source.spoiler ? Flag::f_spoiler : Flag())
				| (cover ? Flag::f_video_cover : Flag())),
			document->mtpInput(),
			cover ? cover->mtpInput() : MTPInputPhoto(),
			MTP_int(0),
			MTP_int(0),
			MTPstring());
	} else if (photo && !photo->uploading()) {
		using Flag = MTPDinputMediaPhoto::Flag;
		source.photoId = photo->id;
		source.media = MTP_inputMediaPhoto(
			MTP_flags(source.spoiler ? Flag::f_spoiler : Flag()),
			photo->mtpInput(),
			MTP_int(0),
			MTPInputDocument());
	} else if (document || photo) {
		source.text = source.representation = {};
	}
}

MTPInputMedia InputMedia(const Source &source) {
	Assert(source.media.has_value());
	return *source.media;
}

Source Capture(not_null<HistoryItem*> item, std::optional<QString> knownLink = {}) {
	const auto session = &item->history()->session();
	const auto media = item->media();
	const auto nativeId = item->isEphemeral()
		? session->ephemeralMessages().lookupId(item)
		: IsServerMsgId(item->id) ? int(item->id.bare) : 0;
	auto result = Source{
		.id = item->fullId(),
		.sessionId = session->uniqueId(),
		.nativeId = nativeId,
		.group = item->groupId(),
		.text = item->originalText(),
		.representation = HistoryItemText(item).rich,
		.right = item->requiredSendRight(),
		.resolve = (item->isRegular() || item->isEphemeral())
			&& !item->isAdminLogEntry(),
		.forward = item->allowsForward()
			&& nativeId
			&& !item->isAdminLogEntry(),
		.ephemeral = item->isEphemeral(),
		.inlineRight = item->requiresSendInlineRight(),
	};
	if (const auto page = item->richPage()) {
		result.textual = true;
		result.text = result.representation = {};
		auto groups = base::flat_map<uint64, MessageGroupId>();
		for (const auto &part : Iv::PrepareRichPageShare(*page)) {
			auto &group = groups[part.group];
			if (part.group && !group) {
				group = MessageGroupId::FromRaw(
					result.id.peer, base::RandomValue<uint64>(), false);
			}
			auto child = Source{
				.id = result.id,
				.sessionId = result.sessionId,
				.group = group,
				.text = part.unavailable ? TextWithEntities() : part.text,
				.right = part.document ? part.document->requiredSendRight()
					: part.photo ? ChatRestriction::SendPhotos : ChatRestriction::SendOther,
				.resolve = result.resolve,
				.caption = bool(part.document || part.photo || part.location),
				.textual = true,
				.spoiler = part.spoiler,
			};
			CaptureMedia(child, part.document, part.photo);
			if (part.location) {
				child.media = MTP_inputMediaGeoPoint(MTP_inputGeoPoint(
					MTP_flags(0),
					MTP_double(part.location->first),
					MTP_double(part.location->second),
					MTP_int(0)));
			}
			child.representation = child.text;
			result.richParts.push_back(std::move(child));
		}
	} else if (media && !media->webpage()) {
		result.caption = bool(media->document() || media->photo());
		result.spoiler = media->hasSpoiler();
		if (media->document() || media->photo()) {
			CaptureMedia(result, media->document(), media->photo());
		} else if (const auto contact = media->sharedContact()) {
			result.media = MTP_inputMediaContact(
				MTP_string(contact->phoneNumber),
				MTP_string(contact->firstName),
				MTP_string(contact->lastName),
				MTP_string(contact->vcard));
		} else if (const auto location = dynamic_cast<Data::MediaLocation*>(media)) {
			result.media = location->copyInputMedia();
		} else if (!media->webpage()) {
			result.textual = true;
			if (const auto invoice = media->invoice()) {
				result.representation = TextWithEntities::Simple(invoice->title);
				result.representation.append(u"\n"_q).append(invoice->description);
			} else if (const auto game = media->game()) {
				result.representation = TextWithEntities::Simple(game->title);
				result.representation.append(u"\n"_q).append(game->description);
				result.representation.append(u"\n"_q).append(media->consumedMessageText());
			} else if (result.representation.empty()) {
				result.representation = media->notificationText();
			}
		}
	}
	if (knownLink) {
		result.link = *knownLink;
	} else if (item->hasDirectLink()) {
		result.link = item->history()->session().api().exportDirectMessageLink(item, false);
	} else if (const auto bot = item->getMessageBot()) {
		if (media && media->game()) {
			result.link = bot->session().createInternalLinkFull(
				bot->username() + u"?game="_q + media->game()->shortName);
		}
	}
	if (result.textual && !result.link.isEmpty()
		&& !result.representation.text.contains(result.link)) {
		result.representation.append(u"\n"_q).append(result.link);
	}
	return result;
}

std::vector<Source> CaptureSources(const HistoryItemsList &items) {
	auto result = std::vector<Source>();
	auto seen = base::flat_set<GlobalMsgId>();
	for (const auto item : items) {
		if (CanShareMessage(item) && seen.emplace(item->globalId()).second) {
			result.push_back(Capture(item));
		}
	}
	return result;
}

bool RefreshSources(not_null<Main::Session*> session, std::vector<Source> &sources) {
	for (auto &source : sources) {
		const auto origin = SessionByUniqueId(source.sessionId);
		if (!origin) {
			return false;
		}
		if (source.resolve) {
			const auto item = origin->data().message(source.id);
			if (!CanShareMessage(item) || item->isDeleted()) {
				return false;
			}
			source = Capture(item, source.link);
		}
		source.forward = source.forward && (origin == session);
	}
	return true;
}

struct Part {
	Source source;
	Delivery delivery = Delivery::Text;
	TextWithEntities text;
};

void RebindShareMentions(not_null<Main::Session*> session, EntitiesInText &entities) {
	for (auto &entity : entities) {
		if (entity.type() != EntityType::MentionName) {
			continue;
		}
		const auto fields = TextUtilities::MentionNameDataToFields(entity.data());
		if (!fields.userId || fields.selfId == session->userId().bare) {
			continue;
		}
		const auto user = session->data().userLoaded(UserId(fields.userId));
		const auto known = user || session->data().messageWithPeer(peerFromUser(UserId(fields.userId)));
		entity = EntityInText(
			known ? EntityType::MentionName : EntityType::CustomUrl,
			entity.offset(),
			entity.length(),
			known ? TextUtilities::MentionNameDataFromFields({
				.selfId = session->userId().bare,
				.userId = fields.userId,
				.accessHash = user ? user->accessHash() : 0,
			}) : u"tg://user?id=%1"_q.arg(fields.userId));
	}
}

std::vector<Part> PrepareParts(
		not_null<Main::Session*> session,
		const std::vector<Source> &sources,
		const TextWithTags &comment,
		Mode mode,
		Data::Thread *thread = nullptr,
		bool dropCaptions = false) {
	auto expanded = std::vector<Source>();
	for (auto source : sources) {
		if (source.forward && thread) {
			const auto item = session->data().message(source.id);
			source.forward = item && !item->errorTextForForward(thread);
		}
		if (!source.richParts.empty()
			&& (mode != Mode::Automatic || !source.forward)) {
			const auto &parts = source.richParts;
			expanded.insert(expanded.end(), parts.begin(), parts.end());
		} else {
			expanded.push_back(std::move(source));
		}
	}
	auto contents = std::vector<Api::MessageShare::Content>();
	for (const auto &source : expanded) {
		contents.push_back({
			.text = source.text,
			.representation = source.representation,
			.forward = source.forward,
			.media = source.media.has_value(),
			.caption = source.caption,
			.textual = source.textual,
			.peer = source.id.peer.value,
			.group = source.group.raw(),
			.account = source.sessionId,
		});
	}
	auto result = std::vector<Part>();
	auto prepared = Api::MessageShare::PrepareContent(
		contents,
		{ comment.text, TextUtilities::ConvertTextTagsToEntities(comment.tags) },
		mode,
		Data::PremiumLimits(session).messageLengthCurrent(),
		Data::PremiumLimits(session).captionLengthCurrent(),
		dropCaptions);
	for (auto &part : prepared) {
		RebindShareMentions(session, part.text.entities);
		result.push_back({
			part.source == Api::MessageShare::kCommentSource
				? Source() : expanded[part.source],
			part.delivery,
			std::move(part.text),
		});
	}
	return result;
}

struct Job {
	Api::SendAction action;
	std::vector<Part> parts;
	QVector<MTPlong> randoms;
	Data::ForwardOptions forwardOptions = Data::ForwardOptions::PreserveInfo;
	bool refreshed = false;
};

class Batch final : public std::enable_shared_from_this<Batch> {
public:
	Batch(std::shared_ptr<Main::SessionShow> show,
			std::vector<Job> jobs,
			std::shared_ptr<QPointer<ShareBox>> box,
			std::shared_ptr<rpl::variable<QString>> status)
	: _show(std::move(show))
	, _sessionId(_show->session().uniqueId())
	, _jobs(std::move(jobs))
	, _box(std::move(box))
	, _status(std::move(status)) {
	}

	void start() {
		if (!_queue) {
			const auto weak = weak_from_this();
			_queue = std::make_shared<Api::MessageShare::DeliveryQueue>(_jobs.size(),
				[weak](std::size_t index, Completion done) {
					if (const auto self = weak.lock()) {
						self->send(index, std::move(done));
					}
				}, [weak](Acknowledgement result, std::size_t completed) {
					if (const auto self = weak.lock()) {
						self->acknowledged(result, completed);
					}
				});
		}
		if (_queue->blocked()) {
			if (*_box && SessionByUniqueId(_sessionId) && _show->valid()) {
				_show->showToast(tr::lng_share_uncertain(tr::now));
			}
			return;
		}
		_queue->start();
	}

private:
	Data::Histories::PreparedMessage request(
			not_null<Main::Session*> session,
			const Job &job,
			not_null<History*> history,
			FullReplyTo replyTo) {
		const auto &options = job.action.options;
		const auto peer = history->peer;
		auto request = Api::MessageShare::SendRequest{
			.peer = peer->input(),
			.from = job.parts.front().delivery == Delivery::Forward
				? session->data().peer(job.parts.front().source.id.peer)->input() : MTP_inputPeerEmpty(),
			.sendAs = options.sendAs
				? std::make_optional(options.sendAs->input()) : std::nullopt,
			.replyTo = replyTo
				? std::make_optional(Data::ReplyToForMTP(history, replyTo)) : std::nullopt,
			.shortcut = options.shortcutId
				? std::make_optional(Data::ShortcutIdToMTP(session, options.shortcutId))
				: std::nullopt,
			.suggest = options.suggest
				? std::make_optional(Api::SuggestToMTP(options.suggest)) : std::nullopt,
			.topicRootId = (replyTo.topicRootId == Data::ForumTopic::kGeneralId
				? 0 : int(replyTo.topicRootId.bare)),
			.scheduled = options.scheduled,
			.repeatPeriod = options.scheduleRepeatPeriod,
			.stars = options.starsApproved,
			.effectId = options.effectId,
			.monoforum = bool(replyTo.monoforumPeerId),
			.silent = ShouldSendSilent(peer, options),
			.hideAuthor = job.forwardOptions != Data::ForwardOptions::PreserveInfo,
			.dropCaptions = job.forwardOptions == Data::ForwardOptions::NoNamesAndCaptions,
		};
		for (auto i = 0; i != job.parts.size(); ++i) {
			const auto &part = job.parts[i];
			request.parts.push_back({
				.delivery = part.delivery,
				.media = part.delivery == Delivery::Media ? InputMedia(part.source) : MTPInputMedia(),
				.text = part.text.text,
				.entities = Api::EntitiesToMTP(session, part.text.entities, Api::ConvertOption::SkipLocal),
				.messageId = part.source.nativeId,
				.randomId = job.randoms[i].v,
				.ephemeral = part.source.ephemeral,
			});
		}
		return Api::MessageShare::PrepareRequest(request);
	}

	void send(std::size_t index, Completion done) {
		const auto session = SessionByUniqueId(_sessionId);
		if (!session) {
			done(Acknowledgement::Unknown);
			return;
		}
		auto &job = _jobs[index];
		for (const auto &part : job.parts) {
			const auto &source = part.source;
			if (!source.sessionId) {
				continue;
			}
			const auto origin = SessionByUniqueId(source.sessionId);
			const auto item = (origin && source.resolve)
				? origin->data().message(source.id) : nullptr;
			if (!origin || (source.resolve
				&& (!CanShareMessage(item) || item->isDeleted()))) {
				unavailable(std::move(done));
				return;
			}
		}
		const auto self = shared_from_this();
		session->data().histories().sendPreparedMessage(
			job.action.history,
			job.action.replyTo,
			0,
			[self, index, session](not_null<History*> history, FullReplyTo replyTo) {
				return self->request(session, self->_jobs[index], history, replyTo);
			}, [self, index, done](const MTPUpdates &updates, const MTP::Response &) {
				const auto &job = self->_jobs[index];
				if (updates.type() == mtpc_updateShortSentMessage) {
					job.action.history->session().api().requestMessageData(
						job.action.history->peer, updates.c_updateShortSentMessage().vid().v, [] {});
				}
				self->_sent += int(job.parts.size());
				done(Acknowledgement::Sent);
			}, [self, index, done](const MTP::Error &error, const MTP::Response &) {
				self->failed(index, error, done);
			});
	}

	void acknowledged(Acknowledgement result, std::size_t completed) {
		if (!*_box || !SessionByUniqueId(_sessionId) || !_show->valid()) {
			return;
		}
		if (result == Acknowledgement::Sent && completed == _jobs.size()) {
			(*_box)->closeBox();
			_show->showToast(tr::lng_share_done(tr::now));
		} else if (result == Acknowledgement::Sent) {
			*_status = tr::lng_share_progress(tr::now, lt_amount, QString::number(_sent));
		}
	}

	void failed(std::size_t index, const MTP::Error &error, Completion done) {
		auto &job = _jobs[index];
		const auto session = SessionByUniqueId(_sessionId);
		if (session && !job.refreshed && error.type().startsWith(u"FILE_REFERENCE_"_q)) {
			job.refreshed = true;
			refresh(index, 0, std::move(done));
			return;
		}
		const auto retryable = (error.code() == 400 || error.code() == 403 || error.code() == 420);
		done(retryable ? Acknowledgement::Rejected : Acknowledgement::Unknown);
		if (*_box && session && _show->valid() && !MTP::IgnoreError(error)) {
			*_status = tr::lng_share_progress(tr::now, lt_amount, QString::number(_sent))
				+ u"\n"_q + (retryable ? tr::lng_share_failed : tr::lng_share_uncertain)(tr::now);
			_show->showToast(tr::lng_share_failed(tr::now)
				+ u"\n"_q + error.type());
		}
	}

	void unavailable(Completion done) {
		done(Acknowledgement::Rejected);
		if (*_box && SessionByUniqueId(_sessionId) && _show->valid()) {
			*_status = tr::lng_share_progress(tr::now, lt_amount, QString::number(_sent))
				+ u"\n"_q + tr::lng_share_source_unavailable(tr::now);
			_show->showToast(tr::lng_share_source_unavailable(tr::now));
		}
	}

	void refresh(std::size_t index, std::size_t partIndex, Completion done) {
		const auto session = SessionByUniqueId(_sessionId);
		if (!session) {
			done(Acknowledgement::Unknown);
			return;
		}
		if (partIndex == _jobs[index].parts.size()) {
			send(index, std::move(done));
			return;
		}
		const auto &source = _jobs[index].parts[partIndex].source;
		if (!source.documentId && !source.photoId) {
			refresh(index, partIndex + 1, std::move(done));
			return;
		}
		const auto origin = SessionByUniqueId(source.sessionId);
		if (!origin) {
			unavailable(std::move(done));
			return;
		}
		const auto self = shared_from_this();
		const auto pending = std::make_shared<Completion>(std::move(done));
		origin->lifetime().add([weak = weak_from_this(), pending] {
			const auto self = weak.lock();
			if (self && *pending) {
				self->unavailable(std::exchange(*pending, {}));
			}
		});
		origin->api().refreshFileReference(source.id, [self, pending, index, partIndex](const auto &) {
			if (!*pending) {
				return;
			}
			auto &source = self->_jobs[index].parts[partIndex].source;
			const auto origin = SessionByUniqueId(source.sessionId);
			if (!origin) {
				self->unavailable(std::exchange(*pending, {}));
				return;
			}
			CaptureMedia(source,
				source.documentId ? origin->data().document(source.documentId).get() : nullptr,
				source.photoId ? origin->data().photo(source.photoId).get() : nullptr);
			self->refresh(index, partIndex + 1, std::exchange(*pending, {}));
		});
	}

	std::shared_ptr<Main::SessionShow> _show;
	uint64 _sessionId = 0;
	std::vector<Job> _jobs;
	std::shared_ptr<Api::MessageShare::DeliveryQueue> _queue;
	std::shared_ptr<QPointer<ShareBox>> _box;
	std::shared_ptr<rpl::variable<QString>> _status;
	int _sent = 0;

};

struct Submission {
	std::vector<Api::SendAction> targets;
	TextWithTags comment;
	Data::ForwardOptions forwardOptions = Data::ForwardOptions::PreserveInfo;
	Mode mode = Mode::Automatic;
	friend bool operator==(const Submission &, const Submission &) = default;
};

void OpenShare(
		std::shared_ptr<Main::SessionShow> show,
		std::vector<Source> sources,
		ShareBoxStyleOverrides st) {
	const auto session = &show->session();
	if (sources.empty() || !RefreshSources(session, sources)) {
		show->showToast(tr::lng_share_source_unavailable(tr::now));
		return;
	}
	const auto sessionId = session->uniqueId();
	const auto batch = std::make_shared<std::shared_ptr<Batch>>();
	const auto box = std::make_shared<QPointer<ShareBox>>();
	const auto previous = std::make_shared<std::optional<Submission>>();
	const auto warned = std::make_shared<bool>(ranges::any_of(sources, &Source::textual));
	const auto status = std::make_shared<rpl::variable<QString>>(
		*warned
			? tr::lng_share_text_fallback(tr::now) : QString());
	const auto submit = [=](
			std::vector<not_null<Data::Thread*>> threads,
			Fn<bool()> checkPaid,
			TextWithTags comment,
			Api::SendOptions options,
			Data::ForwardOptions forwardOptions,
			Mode mode) {
		const auto session = SessionByUniqueId(sessionId);
		if (!session || !show->valid() || threads.empty()) {
			return;
		}
		auto submission = Submission{
			.comment = comment,
			.forwardOptions = forwardOptions,
			.mode = mode,
		};
		auto comparisonOptions = options;
		comparisonOptions.starsApproved = 0;
		for (const auto thread : threads) {
			auto action = Api::SendAction(thread, comparisonOptions);
			if (const auto mono = thread->maybeSublistPeer()) {
				action.replyTo.monoforumPeerId = mono->id;
			}
			submission.targets.push_back(action);
		}
		if (*batch) {
			if (**previous == submission) {
				(*batch)->start();
			} else {
				show->showToast(tr::lng_share_retry_unchanged(tr::now));
			}
			return;
		}
		auto current = sources;
		if (!RefreshSources(session, current)) {
			show->showToast(tr::lng_share_source_unavailable(tr::now));
			return;
		}
		if (!*warned && ranges::any_of(current, &Source::textual)) {
			*warned = true;
			*status = tr::lng_share_text_fallback(tr::now);
			return;
		}
		auto jobs = std::vector<Job>();
		for (const auto thread : threads) {
			const auto parts = PrepareParts(
				session,
				current,
				comment,
				mode,
				thread,
				forwardOptions == Data::ForwardOptions::NoNamesAndCaptions);
			if (parts.empty()) {
				show->showToast(tr::lng_share_source_unavailable(tr::now));
				return;
			}
			auto native = HistoryItemsList();
			for (const auto &part : parts) {
				if (part.delivery == Delivery::Forward) {
					if (const auto item = session->data().message(part.source.id)) {
						native.push_back(item);
					}
				}
			}
			const auto normalizedForward = HistoryView::Controls::NormalizeForwardOptions(
				session, native, forwardOptions);
			const auto album = parts.front().source.group;
			const auto oneAlbum = album && ranges::all_of(parts, [&](const Part &part) {
				return part.delivery != Delivery::Text
					&& part.source.group == album
					&& part.source.sessionId == parts.front().source.sessionId;
			});
			if (thread->peer()->slowmodeApplied() && parts.size() > 1 && !oneAlbum) {
				show->showToast(tr::lng_slowmode_no_many(tr::now));
				return;
			}
			const auto error = GetErrorForSending(thread, { .messagesCount = 1 });
			if (error) {
				show->showBox(MakeSendErrorBox({ error, thread }, threads.size() > 1));
				return;
			}
			for (const auto &part : parts) {
				const auto right = (part.delivery == Delivery::Text)
					? ChatRestriction::SendOther : part.source.right;
				if (!Data::CanSend(thread, right)
					|| (part.delivery == Delivery::Forward && part.source.inlineRight
						&& !Data::CanSend(thread, ChatRestriction::SendInline))) {
					const auto error = Data::RestrictionError(thread->peer(), right);
					show->showToast(error.text.isEmpty()
						? tr::lng_share_recipient_unavailable(tr::now) : error.text);
					return;
				}
			}
			auto entries = std::vector<Api::MessageShare::Entry>();
			for (const auto &part : parts) {
				entries.push_back({
					part.delivery,
					part.source.id.peer.value,
					(part.source.documentId || part.source.photoId)
						? part.source.group.raw() : 0,
					part.source.ephemeral,
					part.source.sessionId,
				});
			}
			auto action = Api::SendAction(thread, options);
			action.clearDraft = false;
			if (const auto mono = thread->maybeSublistPeer()) {
				action.replyTo.monoforumPeerId = mono->id;
			}
			for (const auto range : Api::MessageShare::Partition(entries)) {
				auto job = Job{ .action = action, .forwardOptions = normalizedForward };
				job.parts.assign(parts.begin() + range.begin, parts.begin() + range.end);
				job.action.options.starsApproved = std::min(
					options.starsApproved,
					int(job.parts.size()) * thread->peer()->starsPerMessageChecked());
				options.starsApproved -= job.action.options.starsApproved;
				for (const auto &part : job.parts) {
					job.randoms.push_back(MTP_long(base::RandomValue<uint64>()));
				}
				jobs.push_back(std::move(job));
			}
		}
		if (!checkPaid()) {
			return;
		}
		*previous = std::move(submission);
		*status = tr::lng_share_sending(tr::now);
		*batch = std::make_shared<Batch>(show, std::move(jobs), box, status);
		(*batch)->start();
	};
	const auto count = [=](
			const TextWithTags &comment,
			Mode mode,
			Data::Thread *thread = nullptr,
			bool dropCaptions = false) {
		const auto session = SessionByUniqueId(sessionId);
		auto current = sources;
		return session && RefreshSources(session, current)
			? int(PrepareParts(session, current, comment, mode, thread, dropCaptions).size()) : 0;
	};
	auto label = object_ptr<Ui::FlatLabel>(nullptr, status->value(), st::boxLabel);
	auto bottom = object_ptr<Ui::PaddingWrap<Ui::FlatLabel>>(
		nullptr,
		std::move(label),
		st::boxPadding);
	auto native = HistoryItemsList();
	for (const auto &source : sources) {
		if (source.forward) {
			if (const auto item = session->data().message(source.id)) {
				native.push_back(item);
			}
		}
	}
	const auto link = sources.size() == 1 ? sources.front().link : QString();
	auto copyLink = link.isEmpty() ? Fn<void()>() : Fn<void()>([=] {
		QGuiApplication::clipboard()->setText(link);
		show->showToast(tr::lng_background_link_copied(tr::now));
	});
	auto content = Box<ShareBox>(ShareBox::Descriptor{
		.session = session,
		.copyCallback = std::move(copyLink),
		.countMessagesCallback = [=](const TextWithTags &comment) { return count(comment, Mode::Automatic); },
		.submitCallback = [=](auto &&threads, auto check, auto &&comment, auto options, auto forward) {
			submit(std::move(threads), check, std::move(comment), options, forward, Mode::Automatic);
		},
		.filterCallback = [](not_null<Data::Thread*> thread) {
			if (const auto user = thread->peer()->asUser()) {
				return user->canSendIgnoreMoneyRestrictions();
			}
			return Data::CanSendAnyOf(thread, Data::AllSendRestrictions());
		},
		.asCopyCallback = [=](auto &&threads, auto check, auto &&comment, auto options, bool emptyText) {
			submit(std::move(threads), check, std::move(comment), options,
				Data::ForwardOptions::PreserveInfo, emptyText ? Mode::WithoutCaptions : Mode::Copy);
		},
		.copyCountMessagesCallback = [=](const TextWithTags &comment, bool emptyText) {
			return count(comment, emptyText ? Mode::WithoutCaptions : Mode::Copy);
		},
		.preparedCountMessagesCallback = [=](auto thread, const auto &comment, bool copy, bool empty, bool dropCaptions) {
			return count(comment, !copy ? Mode::Automatic
				: empty ? Mode::WithoutCaptions : Mode::Copy, thread, !copy && dropCaptions);
		},
		.bottomWidget = std::move(bottom),
		.st = st,
		.forwardOptions = {
			.sendersCount = ItemsForwardSendersCount(native),
			.captionsCount = ItemsForwardCaptionsCount(native),
			.show = !native.empty() && !HistoryView::Controls::HasOnlyForcedForwardedInfo(native),
		},
		.moneyRestrictionError = ShareMessageMoneyRestrictionError(),
	});
	*box = content.data();
	session->account().sessionChanges(
	) | rpl::filter([](Main::Session *session) {
		return !session;
	}) | rpl::on_next([box] {
		if (*box) {
			(*box)->closeBox();
		}
	}, content->lifetime());
	show->show(std::move(content), Ui::LayerOption::CloseOther);
}

} // namespace

bool CanShareMessage(HistoryItem *item) {
	if (!item || item->isService()) {
		return false;
	}
	const auto media = item->media();
	return !media || (!media->call() && !media->gift() && !media->paper());
}

Fn<void(std::shared_ptr<Ui::Show>)> PrepareMessageShare(
		HistoryItemsList items,
		uint64 fallbackSessionId,
		ShareBoxStyleOverrides st) {
	const auto sources = CaptureSources(items);
	if (sources.empty()) {
		return {};
	}
	const auto sourceSessionId = sources.front().sessionId;
	const auto sameAccount = ranges::all_of(sources, [&](const Source &source) {
		return source.sessionId == sourceSessionId;
	});
	const auto sessionId = sameAccount ? sourceSessionId : fallbackSessionId;
	return [=](std::shared_ptr<Ui::Show> show) {
		if (!show || !show->valid()) {
			return;
		}
		const auto session = SessionByUniqueId(sessionId);
		if (session) {
			OpenShare(Main::MakeSessionShow(show, session), sources, st);
		} else {
			show->showToast(tr::lng_share_source_unavailable(tr::now));
		}
	};
}

Fn<void()> PrepareMessageShare(
		std::shared_ptr<Main::SessionShow> show,
		HistoryItemsList items,
		ShareBoxStyleOverrides st) {
	const auto callback = PrepareMessageShare(
		std::move(items), show->session().uniqueId(), st);
	return callback ? Fn<void()>([=] { callback(show); }) : Fn<void()>();
}

void ShareMessages(
		std::shared_ptr<Main::SessionShow> show,
		HistoryItemsList items,
		ShareBoxStyleOverrides st) {
	if (const auto callback = PrepareMessageShare(show, std::move(items), st)) {
		callback();
	}
}

void AddMessageShareAction(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<Main::SessionShow> show,
		HistoryItemsList items) {
	if (auto callback = PrepareMessageShare(std::move(show), std::move(items))) {
		menu->addAction(tr::lng_background_share(tr::now), std::move(callback), &st::menuIconShare);
	}
}
