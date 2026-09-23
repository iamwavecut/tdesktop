#include "api/message_share_send.h"

namespace Api::MessageShare {

PreparedRequest PrepareRequest(const SendRequest &request) {
	Expects(!request.parts.empty());
	const auto &first = request.parts.front();
	const auto replyTo = request.replyTo.value_or(MTPInputReplyTo());
	const auto &entities = first.entities;
	const auto sendAs = request.sendAs.value_or(MTP_inputPeerEmpty());
	const auto shortcut = request.shortcut.value_or(MTPInputQuickReplyShortcut());
	const auto top = request.topicRootId;
	const auto mono = request.monoforum;
	const auto silent = request.silent;
	const auto stars = request.stars;
	const auto hasReply = request.replyTo.has_value();
	const auto hasSendAs = request.sendAs.has_value();
	const auto hasShortcut = request.shortcut.has_value();
	const auto hasSuggest = request.suggest.has_value();
	auto randoms = QVector<MTPlong>();
	for (const auto &part : request.parts) {
		randoms.push_back(MTP_long(part.randomId));
	}
	if (first.delivery == Delivery::Forward) {
		using Flag = MTPmessages_ForwardMessages::Flag;
		auto ids = QVector<MTPint>();
		for (const auto &part : request.parts) {
			ids.push_back(MTP_int(part.messageId));
		}
		return MTPmessages_ForwardMessages(
			MTP_flags(Flag::f_with_my_score
				| (silent ? Flag::f_silent : Flag())
				| (top ? Flag::f_top_msg_id : Flag())
				| (mono ? Flag::f_reply_to : Flag())
				| (first.ephemeral ? Flag::f_from_ephemeral : Flag())
				| (request.effectId ? Flag::f_effect : Flag())
				| (hasSuggest ? Flag::f_suggested_post : Flag())
				| (request.scheduled ? Flag::f_schedule_date : Flag())
				| (request.repeatPeriod ? Flag::f_schedule_repeat_period : Flag())
				| (hasSendAs ? Flag::f_send_as : Flag())
				| (hasShortcut ? Flag::f_quick_reply_shortcut : Flag())
				| (stars ? Flag::f_allow_paid_stars : Flag())
				| (request.hideAuthor ? Flag::f_drop_author : Flag())
				| (request.dropCaptions ? Flag::f_drop_media_captions : Flag())),
			request.from,
			MTP_vector<MTPint>(ids),
			MTP_vector<MTPlong>(randoms),
			request.peer,
			MTP_int(top),
			mono ? replyTo : MTPInputReplyTo(),
			MTP_int(request.scheduled),
			MTP_int(request.repeatPeriod),
			sendAs,
			shortcut,
			MTP_long(request.effectId),
			MTP_int(0),
			MTP_long(stars),
			request.suggest.value_or(MTPSuggestedPost()));
	} else if (first.delivery == Delivery::Text) {
		using Flag = MTPmessages_SendMessage::Flag;
		return MTPmessages_SendMessage(
			MTP_flags((hasReply ? Flag::f_reply_to : Flag())
				| (silent ? Flag::f_silent : Flag())
				| (!entities.v.isEmpty() ? Flag::f_entities : Flag())
				| (request.scheduled ? Flag::f_schedule_date : Flag())
				| (request.repeatPeriod ? Flag::f_schedule_repeat_period : Flag())
				| (hasSendAs ? Flag::f_send_as : Flag())
				| (hasShortcut ? Flag::f_quick_reply_shortcut : Flag())
				| (request.effectId ? Flag::f_effect : Flag())
				| (hasSuggest ? Flag::f_suggested_post : Flag())
				| (stars ? Flag::f_allow_paid_stars : Flag())),
			request.peer,
			replyTo,
			MTP_string(first.text),
			randoms.front(),
			MTPReplyMarkup(),
			entities,
			MTP_int(request.scheduled),
			MTP_int(request.repeatPeriod),
			sendAs,
			shortcut,
			MTP_long(request.effectId),
			MTP_long(stars),
			request.suggest.value_or(MTPSuggestedPost()),
			MTPInputRichMessage());
	} else if (request.parts.size() == 1) {
		using Flag = MTPmessages_SendMedia::Flag;
		return MTPmessages_SendMedia(
			MTP_flags((hasReply ? Flag::f_reply_to : Flag())
				| (silent ? Flag::f_silent : Flag())
				| (!entities.v.isEmpty() ? Flag::f_entities : Flag())
				| (request.scheduled ? Flag::f_schedule_date : Flag())
				| (request.repeatPeriod ? Flag::f_schedule_repeat_period : Flag())
				| (hasSendAs ? Flag::f_send_as : Flag())
				| (hasShortcut ? Flag::f_quick_reply_shortcut : Flag())
				| (request.effectId ? Flag::f_effect : Flag())
				| (hasSuggest ? Flag::f_suggested_post : Flag())
				| (stars ? Flag::f_allow_paid_stars : Flag())),
			request.peer,
			replyTo,
			first.media,
			MTP_string(first.text),
			randoms.front(),
			MTPReplyMarkup(),
			entities,
			MTP_int(request.scheduled),
			MTP_int(request.repeatPeriod),
			sendAs,
			shortcut,
			MTP_long(request.effectId),
			MTP_long(stars),
			request.suggest.value_or(MTPSuggestedPost()));
	}
	using Flag = MTPmessages_SendMultiMedia::Flag;
	auto media = QVector<MTPInputSingleMedia>();
	for (auto i = 0; i != request.parts.size(); ++i) {
		const auto &part = request.parts[i];
		const auto entities = part.entities;
		media.push_back(MTP_inputSingleMedia(
			MTP_flags(entities.v.isEmpty()
				? MTPDinputSingleMedia::Flag()
				: MTPDinputSingleMedia::Flag::f_entities),
			part.media,
			randoms[i],
			MTP_string(part.text),
			entities));
	}
	return MTPmessages_SendMultiMedia(
		MTP_flags((hasReply ? Flag::f_reply_to : Flag())
			| (silent ? Flag::f_silent : Flag())
			| (request.scheduled ? Flag::f_schedule_date : Flag())
			| (hasSendAs ? Flag::f_send_as : Flag())
			| (hasShortcut ? Flag::f_quick_reply_shortcut : Flag())
			| (request.effectId ? Flag::f_effect : Flag())
			| (stars ? Flag::f_allow_paid_stars : Flag())),
		request.peer,
		replyTo,
		MTP_vector<MTPInputSingleMedia>(media),
		MTP_int(request.scheduled),
		sendAs,
		shortcut,
		MTP_long(request.effectId),
		MTP_long(stars));
}

} // namespace Api::MessageShare
