#pragma once

#include "api/message_share_policy.h"
#include "base/assertion.h"
#include "base/basic_types.h"
#include "ui/text/text_entity.h"

namespace Api::MessageShare {

enum class Mode { Automatic, Copy, WithoutCaptions };

struct Content {
	TextWithEntities text;
	TextWithEntities representation;
	bool forward = false;
	bool media = false;
	bool caption = false;
	bool textual = false;
	uint64_t peer = 0;
	uint64_t group = 0;
	uint64_t account = 0;
};

struct PreparedContent {
	std::size_t source = 0;
	Delivery delivery = Delivery::Text;
	TextWithEntities text;
};

inline constexpr auto kCommentSource = std::size_t(-1);

[[nodiscard]] std::vector<PreparedContent> PrepareContent(
	const std::vector<Content> &sources,
	TextWithEntities comment,
	Mode mode,
	int messageLimit,
	int captionLimit,
	bool dropCaptions = false);

} // namespace Api::MessageShare
