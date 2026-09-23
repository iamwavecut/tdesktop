#include "api/message_share_preparation.h"

#include "ui/text/text_utilities.h"

#include <set>
#include <tuple>

namespace Api::MessageShare {

std::vector<PreparedContent> PrepareContent(
		const std::vector<Content> &sources,
		TextWithEntities comment,
		Mode mode,
		int messageLimit,
		int captionLimit,
		bool dropCaptions) {
	auto result = std::vector<PreparedContent>();
	auto copyAlbums = std::set<std::tuple<uint64_t, uint64_t, uint64_t>>();
	for (const auto &source : sources) {
		if (source.group && !source.forward) {
			copyAlbums.emplace(source.account, source.peer, source.group);
		}
	}
	auto captions = std::vector<std::pair<std::size_t, TextWithEntities>>();
	const auto addText = [&](std::size_t source, TextWithEntities text) {
		auto part = TextWithEntities();
		while (TextUtilities::CutPart(part, text, messageLimit)) {
			if (!part.empty()) {
				result.push_back({ source, Delivery::Text, std::move(part) });
			}
		}
	};
	if (!comment.empty()) {
		addText(kCommentSource, std::move(comment));
	}
	for (auto i = std::size_t(0); i != sources.size(); ++i) {
		const auto &source = sources[i];
		if (mode == Mode::Automatic && source.forward
			&& !copyAlbums.contains({ source.account, source.peer, source.group })) {
			result.push_back({ i, Delivery::Forward, {} });
		} else if (source.media) {
			auto text = ((mode == Mode::WithoutCaptions || dropCaptions) && source.caption)
				? TextWithEntities() : source.text;
			if (text.text.size() > captionLimit) {
				result.push_back({ i, Delivery::Media, {} });
				captions.emplace_back(i, std::move(text));
			} else {
				result.push_back({ i, Delivery::Media, std::move(text) });
			}
		} else {
			auto text = source.textual ? source.representation : source.text;
			if (text.empty()) {
				text = source.representation;
			}
			if (text.empty()) {
				return {};
			}
			addText(i, std::move(text));
		}
		if (!source.group || i + 1 == sources.size()
			|| sources[i + 1].group != source.group
			|| sources[i + 1].account != source.account
			|| sources[i + 1].peer != source.peer) {
			for (auto &[source, caption] : captions) {
				addText(source, std::move(caption));
			}
			captions.clear();
		}
	}
	return result;
}

} // namespace Api::MessageShare
