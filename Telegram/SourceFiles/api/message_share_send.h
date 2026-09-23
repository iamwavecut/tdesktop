#pragma once

#include "api/message_share_policy.h"
#include "scheme.h"

#include <variant>

namespace Api::MessageShare {

struct SendPart {
	Delivery delivery = Delivery::Text;
	MTPInputMedia media;
	QString text;
	MTPVector<MTPMessageEntity> entities;
	int messageId = 0;
	uint64 randomId = 0;
	bool ephemeral = false;
};

struct SendRequest {
	MTPInputPeer peer;
	MTPInputPeer from;
	std::optional<MTPInputPeer> sendAs;
	std::optional<MTPInputReplyTo> replyTo;
	std::optional<MTPInputQuickReplyShortcut> shortcut;
	std::optional<MTPSuggestedPost> suggest;
	std::vector<SendPart> parts;
	int topicRootId = 0;
	int scheduled = 0;
	int repeatPeriod = 0;
	int stars = 0;
	uint64 effectId = 0;
	bool monoforum = false;
	bool silent = false;
	bool hideAuthor = false;
	bool dropCaptions = false;
};

using PreparedRequest = std::variant<
	MTPmessages_SendMessage,
	MTPmessages_SendMedia,
	MTPmessages_SendInlineBotResult,
	MTPmessages_SendMultiMedia,
	MTPmessages_ForwardMessages>;

[[nodiscard]] PreparedRequest PrepareRequest(const SendRequest &request);

} // namespace Api::MessageShare
