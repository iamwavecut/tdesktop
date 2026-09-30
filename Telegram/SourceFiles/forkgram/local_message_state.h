/*
This file is part of Forkgram.
*/
#pragma once

#include "base/assertion.h"
#include "base/basic_types.h"
#include "base/flat_map.h"
#include "base/flat_set.h"
#include "base/weak_ptr.h"
#include <rpl/event_stream.h>
#include <optional>
#include "core/credits_amount.h"
#include "scheme.h"
#include "data/data_msg_id.h"

#include <algorithm>
#include <memory>
#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QString>

namespace MTP {
class AuthKey;
using AuthKeyPtr = std::shared_ptr<AuthKey>;
} // namespace MTP

namespace Forkgram::LocalMessageState {

constexpr auto kRevisionRetentionMonths = 24;
constexpr auto kDeletedRetentionDays = 7;
constexpr auto kSecondsInDay = 24 * 60 * 60;

struct RevisionSnapshot {
	QByteArray raw;
	QString text;
	QString media;
	TimeId date = 0;
	TimeId editDate = 0;
	int entitiesCount = 0;
};

struct RevisionEntry {
	std::vector<RevisionSnapshot> versions;
	TimeId deletedDate = 0;
};

using RevisionMap = base::flat_map<FullMsgId, RevisionEntry>;
using HiddenMap = base::flat_map<FullMsgId, TimeId>;

struct Metadata {
	int revisionPartition = 0;
	int hiddenPartition = 0;
	int versionsCount = 0;
	TimeId deletedDate = 0;
};

using MetadataMap = base::flat_map<FullMsgId, Metadata>;

struct Snapshot {
	RevisionMap revisions;
	HiddenMap hidden;
	MetadataMap metadata;
	bool complete = false;
	bool valid = true;
};

[[nodiscard]] inline int PartitionFromDate(TimeId date) {
	if (date <= 0) {
		date = 1;
	}
	const auto when = QDateTime::fromSecsSinceEpoch(date, Qt::UTC).date();
	return when.year() * 100 + when.month();
}

[[nodiscard]] inline int PartitionCutoff(TimeId now, int months) {
	const auto when = QDateTime::fromSecsSinceEpoch(
		std::max(now, TimeId(1)),
		Qt::UTC).date().addMonths(-months);
	return when.year() * 100 + when.month();
}

[[nodiscard]] inline TimeId RevisionEntryDate(const RevisionEntry &entry) {
	auto result = entry.deletedDate;
	for (const auto &version : entry.versions) {
		result = std::max(result, version.date);
		result = std::max(result, version.editDate);
	}
	return result;
}

[[nodiscard]] inline int PartitionForRevisionEntry(
		const RevisionEntry &entry,
		TimeId fallback) {
	const auto date = RevisionEntryDate(entry);
	return PartitionFromDate(date ? date : fallback);
}

[[nodiscard]] Snapshot Read(
	const QString &path,
	MTP::AuthKeyPtr localKey);

class Store final : public base::has_weak_ptr {
public:
	Store(
		QString path,
		MTP::AuthKeyPtr localKey,
		QByteArray legacyRevisions = {},
		QByteArray legacyHidden = {},
		Fn<void()> migrated = nullptr);
	~Store();

	[[nodiscard]] Metadata metadata(FullMsgId id) const;
	[[nodiscard]] rpl::producer<std::vector<FullMsgId>> changes() const;
	void load(FullMsgId id, Fn<void(std::optional<RevisionEntry>)> done);
	void record(FullMsgId id, RevisionSnapshot before, RevisionSnapshot after);
	void markDeleted(FullMsgId id, RevisionSnapshot snapshot, TimeId date);
	void hide(FullMsgId id, TimeId date);

private:
	void initialized(MetadataMap metadata, Fn<void()> migrated);
	void update(FullMsgId id, Metadata metadata, uint64 generation);

	struct Private;
	std::unique_ptr<Private> _private;

};

} // namespace Forkgram::LocalMessageState
