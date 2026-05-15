/*
This file is part of Forkgram.
*/
#pragma once

#include "base/assertion.h"
#include "base/basic_types.h"
#include "base/flat_map.h"
#include "base/flat_set.h"
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

struct Snapshot {
	RevisionMap revisions;
	HiddenMap hidden;
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

[[nodiscard]] inline bool PruneExpiredRevisionEntries(
		RevisionMap &revisions,
		TimeId now) {
	const auto cutoff = PartitionCutoff(now, kRevisionRetentionMonths);
	auto changed = false;
	for (auto i = revisions.begin(); i != revisions.end();) {
		if (!i->second.deletedDate
			&& PartitionForRevisionEntry(i->second, now) < cutoff) {
			i = revisions.erase(i);
			changed = true;
		} else {
			++i;
		}
	}
	return changed;
}

[[nodiscard]] Snapshot Read(
	const QString &path,
	MTP::AuthKeyPtr localKey);

void WriteAsync(
	const QString &path,
	MTP::AuthKeyPtr localKey,
	const RevisionMap &revisions,
	const HiddenMap &hidden,
	const base::flat_set<int> &dirtyPartitions,
	bool rewriteAll);

} // namespace Forkgram::LocalMessageState
