#include "api/message_share_preparation.h"
#include "api/message_share_send.h"

#include <cstdlib>
#include <iostream>
#include <rpl/never.h>

namespace crl {
rpl::producer<> on_main_update_requests() {
	return rpl::never<>();
}
} // namespace crl

namespace {

void Require(bool condition, const char *message) {
	if (!condition) {
		std::cerr << message << '\n';
		std::exit(1);
	}
}

void TestOrderedMixedSelection() {
	using namespace Api::MessageShare;
	const auto input = std::vector<Entry>{
		{ Delivery::Forward, 1, 0 },
		{ Delivery::Forward, 2, 0 },
		{ Delivery::Text, 2, 0 },
		{ Delivery::Media, 2, 7 },
		{ Delivery::Media, 2, 7 },
		{ Delivery::Media, 3, 7 },
		{ Delivery::Forward, 2, 0, true },
		{ Delivery::Forward, 2, 0, false },
	};
	const auto ranges = Partition(input);
	Require(ranges.size() == 7, "mixed sources or delivery modes merged");
	Require(ranges[3].begin == 3 && ranges[3].end == 5,
		"selected album members were not kept together");
	auto next = std::size_t(0);
	for (const auto range : ranges) {
		Require(range.begin == next, "selection reordered or lost");
		next = range.end;
	}
	Require(next == input.size(), "selection truncated");
	Require(Partition({}).empty(), "empty selection produced a request");
}

void TestAlbumBoundaries() {
	using namespace Api::MessageShare;
	const auto entries = std::vector<Entry>(23, { Delivery::Media, 1, 8 });
	const auto ranges = Partition(entries);
	Require(ranges.size() == 3 && ranges[0].end == 10
		&& ranges[1].end == 20 && ranges[2].end == 23,
		"album request exceeded the server limit");
	Require(Partition({ { Delivery::Media }, { Delivery::Media } }).size() == 2,
		"unrelated attachments became an album");
}

void TestAcknowledgementsAndRetry() {
	using namespace Api::MessageShare;
	auto progress = DeliveryProgress(3);
	Require(progress.begin() == 0, "first request not started");
	Require(!progress.begin(), "double click duplicated an in-flight request");
	Require(progress.completed() == 0, "dispatch counted as delivery");
	progress.succeed();
	Require(progress.begin() == 1, "next request out of order");
	progress.fail(true);
	Require(progress.begin() == 1, "retry resent an acknowledged request");
	progress.succeed();
	Require(progress.begin() == 2, "retry skipped an unsent request");
	progress.fail(false);
	Require(!progress.begin() && progress.blocked(),
		"ambiguous failure retried and could duplicate a message");
	Require(progress.completed() == 2, "failed request counted as delivered");
	auto empty = DeliveryProgress(0);
	Require(!empty.begin(), "empty batch dispatched");
}

void TestContentPreparation() {
	using namespace Api::MessageShare;
	auto formatted = TextWithEntities{
		.text = u"bold link hidden emoji"_q,
		.entities = {
			{ EntityType::Bold, 0, 4 },
			{ EntityType::CustomUrl, 5, 4, u"https://example.com"_q },
			{ EntityType::Spoiler, 10, 6 },
			{ EntityType::CustomEmoji, 17, 5, u"123456"_q },
		},
	};
	const auto content = std::vector<Content>{
		{ .text = formatted, .forward = true },
		{ .text = formatted },
		{ .text = formatted, .media = true, .caption = true },
		{ .representation = TextWithEntities::Simple(u"question\nanswer one\nanswer two"_q), .textual = true },
	};
	const auto normal = PrepareContent(content, {}, Mode::Automatic, 4096, 1024);
	Require(normal.size() == 4, "content missing from mixed preparation");
	Require(normal[0].delivery == Delivery::Forward && normal[1].delivery == Delivery::Text,
		"protected text did not fall back to copying");
	Require(normal[1].text == formatted, "formatting lost in text copy");
	Require(normal[2].delivery == Delivery::Media && normal[2].text == formatted,
		"media caption lost its formatting");
	Require(normal[3].text == content[3].representation,
		"structured message did not use its full representation");
	const auto copy = PrepareContent(content, {}, Mode::Copy, 4096, 1024);
	Require(copy[0].delivery == Delivery::Text && copy[0].text == formatted,
		"copy mode used native forwarding");
	const auto without = PrepareContent(content, formatted, Mode::WithoutCaptions, 4096, 1024);
	Require(without.size() == 5 && without[0].source == kCommentSource,
		"comment missing or reordered");
	Require(without[1].text == formatted && without[2].text == formatted,
		"without captions erased standalone text");
	Require(without[3].text.empty(), "media caption not removed");
	Require(!without[4].text.empty(), "without captions erased structured fallback");
	const auto nativeWithout = PrepareContent(content, {}, Mode::Automatic, 4096, 4, true);
	Require(nativeWithout.size() == 4 && nativeWithout[0].delivery == Delivery::Forward
		&& nativeWithout[1].text == formatted && nativeWithout[2].text.empty(),
		"native hide-caption option failed to remove copied captions or changed standalone text");
	Require(PrepareContent({ Content{} }, {}, Mode::Copy, 4096, 1024).empty(),
		"unavailable content produced an empty message");
	const auto longCaption = PrepareContent({ content[2] }, {}, Mode::Copy, 12, 4);
	Require(longCaption.size() > 1 && longCaption.front().delivery == Delivery::Media
		&& longCaption.front().text.empty(), "oversized caption was truncated");
	auto joined = QString();
	for (const auto &part : longCaption) {
		joined += part.text.text;
	}
	Require(joined == formatted.text, "split caption lost content");
}

class RequestReader {
public:
	template <typename Request>
	explicit RequestReader(const Request &request) {
		request.write(_data);
		_from = _data.data();
		_end = _from + _data.size();
	}
	template <typename Value>
	Value read() {
		auto value = Value();
		Require(value.read(_from, _end), "invalid serialized request");
		return value;
	}
private:
	mtpBuffer _data;
	const mtpPrime *_from = nullptr;
	const mtpPrime *_end = nullptr;

};

void TestMixedAlbumPreparation() {
	using namespace Api::MessageShare;
	const auto sources = std::vector<Content>{
		{ .text = TextWithEntities::Simple(u"long caption"_q), .forward = true, .media = true, .caption = true, .peer = 1, .group = 4 },
		{ .media = true, .peer = 1, .group = 4 },
	};
	const auto album = PrepareContent(sources, {}, Mode::Automatic, 4096, 1024);
	Require(album.size() == 2 && album[0].delivery == Delivery::Media && album[1].delivery == Delivery::Media,
		"mixed copy/forward availability broke an album");
	const auto overflow = PrepareContent(sources, {}, Mode::Copy, 4096, 4);
	Require(overflow.size() == 3 && overflow[0].delivery == Delivery::Media
		&& overflow[1].delivery == Delivery::Media && overflow[2].text.text == u"long caption"_q,
		"oversized caption split its album or lost text");
	const auto ranges = Partition({
		{ Delivery::Forward, 1, 0 }, { Delivery::Forward, 1, 4 },
		{ Delivery::Forward, 1, 4 }, { Delivery::Forward, 1, 5 },
	});
	Require(ranges.size() == 3 && ranges[1].begin == 1 && ranges[1].end == 3,
		"forward requests did not preserve album boundaries");
	Require(Partition({
		{ Delivery::Media, 1, 4, false, 1 },
		{ Delivery::Media, 1, 4, false, 2 },
	}).size() == 2, "albums from different accounts were merged");
	const auto accounts = PrepareContent({
		{ .forward = true, .media = true, .peer = 1, .group = 4, .account = 1 },
		{ .media = true, .peer = 1, .group = 4, .account = 2 },
	}, {}, Mode::Automatic, 4096, 1024);
	Require(accounts.size() == 2 && accounts[0].delivery == Delivery::Forward,
		"copying from another account changed the native source album");
}

void TestPreparedRequests() {
	using namespace Api::MessageShare;
	auto request = SendRequest{
		.peer = MTP_inputPeerChannel(MTP_long(22), MTP_long(33)),
		.from = MTP_inputPeerUser(MTP_long(11), MTP_long(44)),
		.sendAs = std::nullopt,
		.replyTo = MTP_inputReplyToMessage(
			MTP_flags(MTPDinputReplyToMessage::Flag::f_top_msg_id),
			MTP_int(77), MTP_int(77), MTPInputPeer(), MTPstring(),
			MTPVector<MTPMessageEntity>(), MTPint(), MTPInputPeer(), MTPint(), MTPbytes()),
		.parts = {{
			.delivery = Delivery::Text,
			.text = u"formatted text"_q,
			.entities = MTP_vector<MTPMessageEntity>({ MTP_messageEntityBold(MTP_int(0), MTP_int(9)) }),
			.messageId = 7,
			.randomId = 123,
		}},
		.topicRootId = 77,
		.scheduled = 123456789,
		.stars = 15,
		.silent = true,
	};
	const auto text = std::get<MTPmessages_SendMessage>(PrepareRequest(request));
	auto reader = RequestReader(text);
	Require(reader.read<MTPint>().v == mtpc_messages_sendMessage, "wrong text request method");
	const auto textFlags = reader.read<MTPflags<MTPmessages_SendMessage::Flags>>().v;
	using TextFlag = MTPmessages_SendMessage::Flag;
	Require(bool(textFlags & TextFlag::f_silent) && !(textFlags & TextFlag::f_clear_draft),
		"copy lost silent delivery or cleared a draft");
	Require(reader.read<MTPInputPeer>().c_inputPeerChannel().vchannel_id().v == 22,
		"copy sent to a different peer");
	Require(reader.read<MTPInputReplyTo>().c_inputReplyToMessage().vtop_msg_id()->v == 77,
		"copy lost the selected forum topic");
	Require(reader.read<MTPstring>().v == "formatted text", "copy lost text");
	Require(reader.read<MTPlong>().v == 123, "copy lost its idempotency key");
	Require(reader.read<MTPVector<MTPMessageEntity>>().v.size() == 1, "copy lost formatting");
	Require(reader.read<MTPint>().v == request.scheduled, "copy lost its schedule");
	Require(reader.read<MTPlong>().v == 15, "copy changed the payment budget");
	request.parts.front().delivery = Delivery::Forward;
	const auto forward = std::get<MTPmessages_ForwardMessages>(PrepareRequest(request));
	auto forwardReader = RequestReader(forward);
	Require(forwardReader.read<MTPint>().v == mtpc_messages_forwardMessages, "wrong forward method");
	const auto forwardFlags = forwardReader.read<MTPflags<MTPmessages_ForwardMessages::Flags>>().v;
	Require(bool(forwardFlags & MTPmessages_ForwardMessages::Flag::f_top_msg_id), "forward lost topic flag");
	Require(forwardReader.read<MTPInputPeer>().c_inputPeerUser().vuser_id().v == 11,
		"native forward used the wrong source chat");
	Require(forwardReader.read<MTPVector<MTPint>>().v.front().v == 7, "forward lost source id");
	Require(forwardReader.read<MTPVector<MTPlong>>().v.front().v == 123, "forward lost random id");
	Require(forwardReader.read<MTPInputPeer>().c_inputPeerChannel().vchannel_id().v == 22, "forward lost peer");
	Require(forwardReader.read<MTPint>().v == 77, "forward lost selected topic");
	request.monoforum = true;
	request.topicRootId = 0;
	request.replyTo = MTP_inputReplyToMonoForum(MTP_inputPeerUser(MTP_long(55), MTP_long(66)));
	request.parts.front().delivery = Delivery::Media;
	request.parts.front().media = MTP_inputMediaContact(
		MTP_string("123"), MTP_string("First"), MTP_string("Last"), MTP_string("VCARD"));
	const auto media = std::get<MTPmessages_SendMedia>(PrepareRequest(request));
	auto mediaReader = RequestReader(media);
	Require(mediaReader.read<MTPint>().v == mtpc_messages_sendMedia, "wrong media method");
	const auto mediaFlags = mediaReader.read<MTPflags<MTPmessages_SendMedia::Flags>>().v;
	Require(bool(mediaFlags & MTPmessages_SendMedia::Flag::f_reply_to), "media lost reply flag");
	Require(mediaReader.read<MTPInputPeer>().c_inputPeerChannel().vchannel_id().v == 22, "media lost peer");
	Require(mediaReader.read<MTPInputReplyTo>().c_inputReplyToMonoForum().vmonoforum_peer_id()
		.c_inputPeerUser().vuser_id().v == 55, "copy flattened channel correspondence into parent peer");
	Require(mediaReader.read<MTPInputMedia>().c_inputMediaContact().vvcard().v == "VCARD", "contact details lost");
	request.parts.push_back(request.parts.front());
	request.parts.back().randomId = 456;
	const auto album = std::get<MTPmessages_SendMultiMedia>(PrepareRequest(request));
	auto albumReader = RequestReader(album);
	Require(albumReader.read<MTPint>().v == mtpc_messages_sendMultiMedia, "wrong album method");
	const auto albumFlags = albumReader.read<MTPflags<MTPmessages_SendMultiMedia::Flags>>().v;
	using AlbumFlag = MTPmessages_SendMultiMedia::Flag;
	Require(bool(albumFlags & AlbumFlag::f_silent) && !(albumFlags & AlbumFlag::f_clear_draft),
		"album lost silent delivery or changed a draft");
	Require(albumReader.read<MTPInputPeer>().c_inputPeerChannel().vchannel_id().v == 22, "album lost peer");
	Require(albumReader.read<MTPInputReplyTo>().type() == mtpc_inputReplyToMonoForum, "album lost correspondence");
	const auto members = albumReader.read<MTPVector<MTPInputSingleMedia>>().v;
	Require(members.size() == 2 && members[1].data().vrandom_id().v == 456,
		"album lost an element or reused its random id");

}

void TestMockTransport() {
	using namespace Api::MessageShare;
	auto sent = std::vector<std::size_t>();
	auto events = std::vector<std::pair<Acknowledgement, std::size_t>>();
	auto pending = DeliveryQueue::Completion();
	const auto queue = std::make_shared<DeliveryQueue>(3,
		[&](std::size_t index, DeliveryQueue::Completion done) {
			sent.push_back(index);
			pending = std::move(done);
		}, [&](Acknowledgement ack, std::size_t completed) {
			events.emplace_back(ack, completed);
		});
	const auto complete = [&](Acknowledgement ack) {
		auto callback = std::exchange(pending, {});
		Require(bool(callback), "transport had no pending request");
		callback(ack);
	};
	queue->start();
	queue->start();
	Require(sent == std::vector<std::size_t>{ 0 } && events.empty(),
		"double click sent twice or dispatch was reported as success");
	complete(Acknowledgement::Sent);
	complete(Acknowledgement::Rejected);
	Require(sent == std::vector<std::size_t>{ 0, 1 } && !pending,
		"queue continued after server rejection");
	queue->start();
	Require(sent == std::vector<std::size_t>{ 0, 1, 1 },
		"retry repeated confirmed delivery or skipped the rejected request");
	complete(Acknowledgement::Sent);
	complete(Acknowledgement::Unknown);
	queue->start();
	Require(sent == std::vector<std::size_t>{ 0, 1, 1, 2 } && queue->blocked(),
		"uncertain delivery was retried and could duplicate content");
	Require(events.back().second == 2,
		"partial delivery result included an unconfirmed message");
}

} // namespace

int main() {
	TestMixedAlbumPreparation();
	TestPreparedRequests();
	TestMockTransport();
	TestContentPreparation();
	TestOrderedMixedSelection();
	TestAlbumBoundaries();
	TestAcknowledgementsAndRetry();
	std::cout << "message share tests passed\n";
}
