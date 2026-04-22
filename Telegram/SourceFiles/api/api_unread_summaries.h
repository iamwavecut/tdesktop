/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_map.h"
#include "data/data_msg_id.h"
#include "rpl/lifetime.h"
#include "rpl/producer.h"
#include "ui/text/text_entity.h"

#include <QtCore/QPointer>
#include <QtCore/QString>

#include <memory>

class ApiWrap;
class History;
class HistoryItem;
class QNetworkAccessManager;
class QNetworkReply;

namespace Data {
struct MessagesSlice;
class Thread;
class SavedSublist;
class ForumTopic;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace Api {

class UnreadSummaries final {
public:
	struct ThreadKey {
		PeerId peerId = 0;
		MsgId topicRootId = 0;
		PeerId monoforumPeerId = 0;

		friend inline auto operator<=>(ThreadKey, ThreadKey) = default;
		friend inline bool operator==(ThreadKey, ThreadKey) = default;
	};

	struct Entry {
		bool loading = false;
		TextWithEntities lastSummaryText;
		FullMsgId shownItemId;
		int version = 0;
	};

	enum class StartResult {
		Started,
		AlreadyLoading,
		InvalidConfig,
	};

	enum class Failure {
		Config,
		NoText,
		Network,
		Parse,
		Inject,
	};

	explicit UnreadSummaries(not_null<ApiWrap*> api);
	~UnreadSummaries();

	[[nodiscard]] static ThreadKey Key(not_null<const Data::Thread*> thread);

	[[nodiscard]] bool configured() const;
	[[nodiscard]] bool loading(not_null<const Data::Thread*> thread) const;
	[[nodiscard]] const Entry &entry(not_null<const Data::Thread*> thread) const;
	[[nodiscard]] rpl::producer<ThreadKey> changes() const;

	StartResult request(not_null<Data::Thread*> thread);
	void restore(not_null<Data::Thread*> thread);

private:
	struct PreparedTranscript {
		QString text;
		TimeId rangeFromDate = 0;
		TimeId rangeTillDate = 0;

		[[nodiscard]] bool empty() const {
			return text.isEmpty();
		}
	};

	struct State {
		Entry entry;
		int requestToken = 0;
		std::unique_ptr<rpl::lifetime> lifetime;
		QPointer<QNetworkReply> reply;
	};

	[[nodiscard]] static rpl::producer<Data::MessagesSlice> SourceForThread(
		not_null<Data::Thread*> thread);
	[[nodiscard]] static PreparedTranscript BuildTranscript(
		not_null<Data::Thread*> thread,
		const Data::MessagesSlice &slice);
	void startNetworkRequest(
		const ThreadKey &key,
		PreparedTranscript transcript);
	void failRequest(
		const ThreadKey &key,
		Failure failure,
		QString details = QString());
	void finishLoading(const ThreadKey &key);
	void clearPending(State &state);
	void showToast(
		const ThreadKey &key,
		Failure failure,
		const QString &details);
	[[nodiscard]] History *resolveHistory(const ThreadKey &key) const;
	[[nodiscard]] Data::Thread *resolveThread(const ThreadKey &key) const;
	[[nodiscard]] State &state(const ThreadKey &key);
	[[nodiscard]] const State *lookup(const ThreadKey &key) const;
	[[nodiscard]] FullMsgId injectSummary(
		not_null<Data::Thread*> thread,
		const TextWithEntities &text);

	const not_null<Main::Session*> _session;
	QNetworkAccessManager *_network = nullptr;
	base::flat_map<ThreadKey, State> _states;
	rpl::event_stream<ThreadKey> _changes;
};

} // namespace Api
