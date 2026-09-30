/*
This file is part of Forkgram.
*/
#include "forkgram/local_message_state.h"

#include <crl/crl.h>
#include <rpl/rpl.h>

#include "base/debug_log.h"
#include "base/openssl_help.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "mtproto/mtproto_auth_key.h"

#include <crl/crl_object_on_thread.h>
#include <sodium.h>
#include <QtCore/QBuffer>
#include <QtCore/QDataStream>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QIODevice>
#include <QtCore/QSaveFile>

namespace Forkgram::LocalMessageState {
namespace {

constexpr auto kIndexVersion = qint32(1);
constexpr auto kPartitionVersion = qint32(1);
constexpr auto kEncryptedFileVersion = qint32(1);
constexpr auto kFileMagic = "FGLS";
constexpr auto kIndexFile = "messages_index.aegis";
constexpr auto kMetadataFile = "messages_metadata.aegis";

using IndexEntry = Metadata;
using IndexMap = MetadataMap;

[[nodiscard]] QString PartitionFile(int partition) {
	return u"messages_%1.aegis"_q.arg(partition);
}

[[nodiscard]] QByteArray AdditionalData(const QByteArray &name) {
	return QByteArray(kFileMagic, 4) + name;
}

[[nodiscard]] QByteArray AegisKey(const MTP::AuthKeyPtr &localKey) {
	if (!localKey) {
		return {};
	}
	const auto context = QByteArray("Forkgram local message state AEGIS-128L v1");
	const auto hash = openssl::Sha256(
		localKey->data(),
		bytes::make_span(context));
	auto result = QByteArray(
		crypto_aead_aegis128l_KEYBYTES,
		Qt::Uninitialized);
	memcpy(result.data(), hash.data(), result.size());
	return result;
}

[[nodiscard]] bool EnsureSodium() {
	return sodium_init() >= 0;
}

[[nodiscard]] QByteArray Encrypt(
		const QByteArray &payload,
		const QByteArray &name,
		const MTP::AuthKeyPtr &localKey) {
	if (!EnsureSodium()) {
		return {};
	}
	const auto key = AegisKey(localKey);
	if (key.isEmpty()) {
		return {};
	}
	auto nonce = QByteArray(
		crypto_aead_aegis128l_NPUBBYTES,
		Qt::Uninitialized);
	randombytes_buf(nonce.data(), nonce.size());

	auto encrypted = QByteArray(
		payload.size() + crypto_aead_aegis128l_ABYTES,
		Qt::Uninitialized);
	auto encryptedSize = static_cast<unsigned long long>(0);
	const auto ad = AdditionalData(name);
	if (crypto_aead_aegis128l_encrypt(
		reinterpret_cast<unsigned char*>(encrypted.data()),
		&encryptedSize,
		reinterpret_cast<const unsigned char*>(payload.constData()),
		payload.size(),
		reinterpret_cast<const unsigned char*>(ad.constData()),
		ad.size(),
		nullptr,
		reinterpret_cast<const unsigned char*>(nonce.constData()),
		reinterpret_cast<const unsigned char*>(key.constData())) != 0) {
		return {};
	}
	encrypted.resize(int(encryptedSize));

	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream.writeRawData(kFileMagic, 4);
	stream << kEncryptedFileVersion << nonce << encrypted;
	return (stream.status() == QDataStream::Ok) ? result : QByteArray();
}

[[nodiscard]] QByteArray Decrypt(
		const QByteArray &data,
		const QByteArray &name,
		const MTP::AuthKeyPtr &localKey) {
	if (!EnsureSodium()) {
		return {};
	}
	const auto key = AegisKey(localKey);
	if (key.isEmpty()) {
		return {};
	}
	auto stream = QDataStream(data);
	stream.setVersion(QDataStream::Qt_5_1);
	auto magic = QByteArray(4, Qt::Uninitialized);
	auto version = qint32();
	auto nonce = QByteArray();
	auto encrypted = QByteArray();
	stream.readRawData(magic.data(), magic.size());
	stream >> version >> nonce >> encrypted;
	if (stream.status() != QDataStream::Ok
		|| magic != QByteArray(kFileMagic, 4)
		|| version != kEncryptedFileVersion
		|| nonce.size() != crypto_aead_aegis128l_NPUBBYTES
		|| encrypted.size() < crypto_aead_aegis128l_ABYTES) {
		return {};
	}

	auto payload = QByteArray(
		encrypted.size() - crypto_aead_aegis128l_ABYTES,
		Qt::Uninitialized);
	auto payloadSize = static_cast<unsigned long long>(0);
	const auto ad = AdditionalData(name);
	if (crypto_aead_aegis128l_decrypt(
		reinterpret_cast<unsigned char*>(payload.data()),
		&payloadSize,
		nullptr,
		reinterpret_cast<const unsigned char*>(encrypted.constData()),
		encrypted.size(),
		reinterpret_cast<const unsigned char*>(ad.constData()),
		ad.size(),
		reinterpret_cast<const unsigned char*>(nonce.constData()),
		reinterpret_cast<const unsigned char*>(key.constData())) != 0) {
		return {};
	}
	payload.resize(int(payloadSize));
	return payload;
}

[[nodiscard]] QByteArray ReadEncryptedPayload(
		const QString &path,
		const QByteArray &name,
		const MTP::AuthKeyPtr &localKey) {
	auto file = QFile(path + QString::fromUtf8(name));
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	return Decrypt(file.readAll(), name, localKey);
}

[[nodiscard]] bool WriteEncryptedPayload(
		const QString &path,
		const QByteArray &name,
		const QByteArray &payload,
		const MTP::AuthKeyPtr &localKey) {
	const auto encrypted = Encrypt(payload, name, localKey);
	if (encrypted.isEmpty()) {
		return false;
	}
	if (!QDir().mkpath(path)) {
		return false;
	}
	auto file = QSaveFile(path + QString::fromUtf8(name));
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	if (file.write(encrypted) != encrypted.size()) {
		return false;
	}
	return file.commit();
}

[[nodiscard]] QByteArray SerializeIndex(const IndexMap &index) {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << kIndexVersion << qint32(index.size());
	for (const auto &[id, entry] : index) {
		stream
			<< SerializePeerId(id.peer)
			<< qint64(id.msg.bare)
			<< qint32(entry.revisionPartition)
			<< qint32(entry.hiddenPartition);
	}
	return (stream.status() == QDataStream::Ok) ? result : QByteArray();
}

[[nodiscard]] IndexMap DeserializeIndex(const QByteArray &bytes, bool &valid) {
	if (bytes.isEmpty()) {
		return {};
	}
	auto stream = QDataStream(bytes);
	stream.setVersion(QDataStream::Qt_5_1);
	auto version = qint32();
	auto count = qint32();
	stream >> version >> count;
	if (version != kIndexVersion || count < 0 || count > (bytes.size() - 8) / 24) {
		return {};
	}
	auto entries = std::vector<IndexMap::value_type>();
	entries.reserve(count);
	for (auto i = 0; i != count; ++i) {
		auto peerSerialized = quint64();
		auto msg = qint64();
		auto revisionPartition = qint32();
		auto hiddenPartition = qint32();
		stream
			>> peerSerialized
			>> msg
			>> revisionPartition
			>> hiddenPartition;
		if (revisionPartition < 0 || hiddenPartition < 0) {
			return {};
		}
		entries.emplace_back(
			FullMsgId(DeserializePeerId(peerSerialized), MsgId(msg)),
			IndexEntry{
				.revisionPartition = int(revisionPartition),
				.hiddenPartition = int(hiddenPartition),
				.versionsCount = revisionPartition ? 2 : 0,
			});
	}
	auto result = IndexMap(entries.begin(), entries.end());
	valid = stream.status() == QDataStream::Ok
		&& stream.atEnd()
		&& result.size() == entries.size();
	return valid ? result : IndexMap();
}

[[nodiscard]] QByteArray SerializePartition(
		const Snapshot &snapshot,
		int partition) {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	auto revisionsCount = qint32(0);
	auto hiddenCount = qint32(0);
	for (const auto &[id, entry] : snapshot.revisions) {
		if (PartitionForRevisionEntry(entry, TimeId(1)) == partition) {
			++revisionsCount;
		}
	}
	for (const auto &[id, date] : snapshot.hidden) {
		if (PartitionFromDate(date) == partition) {
			++hiddenCount;
		}
	}
	stream << kPartitionVersion << qint32(partition) << revisionsCount;
	for (const auto &[id, entry] : snapshot.revisions) {
		if (PartitionForRevisionEntry(entry, TimeId(1)) != partition) {
			continue;
		}
		stream
			<< SerializePeerId(id.peer)
			<< qint64(id.msg.bare)
			<< qint32(entry.deletedDate)
			<< qint32(entry.versions.size());
		for (const auto &snapshot : entry.versions) {
			stream
				<< qint32(snapshot.date)
				<< qint32(snapshot.editDate)
				<< qint32(snapshot.entitiesCount)
				<< snapshot.raw
				<< snapshot.text
				<< snapshot.media;
		}
	}
	stream << hiddenCount;
	for (const auto &[id, date] : snapshot.hidden) {
		if (PartitionFromDate(date) != partition) {
			continue;
		}
		stream
			<< SerializePeerId(id.peer)
			<< qint64(id.msg.bare)
			<< qint32(date);
	}
	return (stream.status() == QDataStream::Ok) ? result : QByteArray();
}

[[nodiscard]] bool SkipField(QDataStream &stream) {
	auto size = quint32();
	stream >> size;
	if (size == 0xFFFFFFFFU) {
		return stream.status() == QDataStream::Ok;
	}
	const auto device = stream.device();
	if (stream.status() != QDataStream::Ok
		|| size > device->size() - device->pos()) {
		return false;
	}
	return device->seek(device->pos() + size);
}

[[nodiscard]] std::optional<Snapshot> ParsePartition(
		const QByteArray &bytes,
		const IndexMap &index,
		int expectedPartition,
		bool metadataOnly,
		std::optional<FullMsgId> selected = std::nullopt) {
	auto stream = QDataStream(bytes);
	stream.setVersion(QDataStream::Qt_5_1);
	auto version = qint32();
	auto partition = qint32();
	auto count = qint32();
	stream >> version >> partition >> count;
	if (version != kPartitionVersion
		|| partition != expectedPartition
		|| count < 0
		|| count > bytes.size() / 24) {
		return std::nullopt;
	}
	auto result = Snapshot();
	auto revisions = std::vector<RevisionMap::value_type>();
	auto metadata = std::vector<MetadataMap::value_type>();
	metadata.reserve(count);
	for (auto i = 0; i != count; ++i) {
		auto peer = quint64();
		auto msg = qint64();
		auto deleted = qint32();
		auto versions = qint32();
		stream >> peer >> msg >> deleted >> versions;
		if (versions < 0 || versions > bytes.size() / 24) {
			return std::nullopt;
		}
		const auto id = FullMsgId(DeserializePeerId(peer), MsgId(msg));
		const auto indexed = index.find(id);
		const auto keep = indexed != index.end()
			&& indexed->second.revisionPartition == partition;
		const auto materialize = keep && !metadataOnly
			&& (!selected || *selected == id);
		auto entry = RevisionEntry{ .deletedDate = deleted };
		if (materialize) {
			entry.versions.reserve(versions);
		}
		for (auto j = 0; j != versions; ++j) {
			auto snapshot = RevisionSnapshot();
			auto date = qint32();
			auto editDate = qint32();
			auto entities = qint32();
			stream >> date >> editDate >> entities;
			if (materialize) {
				stream >> snapshot.raw >> snapshot.text >> snapshot.media;
				snapshot.date = date;
				snapshot.editDate = editDate;
				snapshot.entitiesCount = entities;
				entry.versions.push_back(std::move(snapshot));
			} else if (!SkipField(stream)
				|| !SkipField(stream)
				|| !SkipField(stream)) {
				return std::nullopt;
			}
		}
		if (keep) {
			auto value = indexed->second;
			value.versionsCount = versions;
			value.deletedDate = deleted;
			metadata.emplace_back(id, value);
		}
		if (materialize) {
			revisions.emplace_back(id, std::move(entry));
		}
	}
	stream >> count;
	if (count < 0 || count > bytes.size() / 20) {
		return std::nullopt;
	}
	auto hidden = std::vector<HiddenMap::value_type>();
	for (auto i = 0; i != count; ++i) {
		auto peer = quint64();
		auto msg = qint64();
		auto date = qint32();
		stream >> peer >> msg >> date;
		const auto id = FullMsgId(DeserializePeerId(peer), MsgId(msg));
		const auto indexed = index.find(id);
		if (indexed != index.end()
			&& indexed->second.hiddenPartition == partition) {
			hidden.emplace_back(id, date);
		}
	}
	if (stream.status() != QDataStream::Ok || !stream.atEnd()) {
		return std::nullopt;
	}
	result.revisions = RevisionMap(
		std::make_move_iterator(revisions.begin()),
		std::make_move_iterator(revisions.end()));
	result.metadata = MetadataMap(metadata.begin(), metadata.end());
	result.hidden = HiddenMap(hidden.begin(), hidden.end());
	auto expectedRevisions = 0;
	auto expectedHidden = 0;
	for (const auto &[id, value] : index) {
		expectedRevisions += value.revisionPartition == partition;
		expectedHidden += value.hiddenPartition == partition;
	}
	if (result.metadata.size() != metadata.size()
		|| result.hidden.size() != hidden.size()
		|| result.metadata.size() != expectedRevisions
		|| result.hidden.size() != expectedHidden) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] QByteArray IndexDigest(const IndexMap &index, const QString &path) {
	auto bytes = SerializeIndex(index);
	auto partitions = base::flat_set<int>();
	for (const auto &[id, entry] : index) {
		if (entry.revisionPartition) {
			partitions.emplace(entry.revisionPartition);
		}
		if (entry.hiddenPartition) {
			partitions.emplace(entry.hiddenPartition);
		}
	}
	auto stream = QDataStream(&bytes, QIODevice::Append);
	stream.setVersion(QDataStream::Qt_5_1);
	for (const auto partition : partitions) {
		const auto info = QFileInfo(path + PartitionFile(partition));
		stream << qint32(partition) << info.size() << info.lastModified().toMSecsSinceEpoch();
	}
	const auto hash = openssl::Sha256(bytes::make_span(bytes));
	return QByteArray(reinterpret_cast<const char*>(hash.data()), hash.size());
}

[[nodiscard]] QByteArray SerializeMetadata(const IndexMap &index, const QString &path) {
	auto bytes = QByteArray();
	auto stream = QDataStream(&bytes, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << qint32(1) << IndexDigest(index, path) << qint32(index.size());
	for (const auto &[id, entry] : index) {
		stream << SerializePeerId(id.peer) << qint64(id.msg.bare)
			<< qint32(entry.versionsCount) << qint32(entry.deletedDate);
	}
	return stream.status() == QDataStream::Ok ? bytes : QByteArray();
}

[[nodiscard]] bool ApplyMetadata(IndexMap &index, const QByteArray &bytes, const QString &path) {
	auto stream = QDataStream(bytes);
	stream.setVersion(QDataStream::Qt_5_1);
	auto version = qint32();
	auto digest = QByteArray();
	auto count = qint32();
	stream >> version >> digest >> count;
	if (version != 1 || digest != IndexDigest(index, path)
		|| count != index.size()) {
		return false;
	}
	auto values = std::vector<std::pair<int, TimeId>>();
	values.reserve(count);
	for (const auto &[id, entry] : index) {
		auto peer = quint64();
		auto msg = qint64();
		auto versions = qint32();
		auto deleted = qint32();
		stream >> peer >> msg >> versions >> deleted;
		if (FullMsgId(DeserializePeerId(peer), MsgId(msg)) != id
			|| versions < 0 || deleted < 0) {
			return false;
		}
		values.emplace_back(versions, deleted);
	}
	if (stream.status() != QDataStream::Ok || !stream.atEnd()) {
		return false;
	}
	auto i = 0;
	for (auto &[id, entry] : index) {
		entry.versionsCount = values[i].first;
		entry.deletedDate = values[i++].second;
	}
	return true;
}

[[nodiscard]] bool SameRevision(
		const RevisionSnapshot &a,
		const RevisionSnapshot &b) {
	return (!a.raw.isEmpty() && !b.raw.isEmpty())
		? a.raw == b.raw
		: a.text == b.text && a.media == b.media
			&& a.entitiesCount == b.entitiesCount && a.editDate == b.editDate;
}

[[nodiscard]] bool Expired(const Metadata &entry, TimeId now) {
	return entry.deletedDate
		? entry.deletedDate <= now - kDeletedRetentionDays * kSecondsInDay
		: entry.revisionPartition
			&& entry.revisionPartition < PartitionCutoff(now, kRevisionRetentionMonths);
}

class Worker final {
public:
	Worker(QString path, MTP::AuthKeyPtr key, Snapshot initial)
	: _path(std::move(path))
	, _key(std::move(key))
	, _index(std::move(initial.metadata))
	, _complete(initial.complete)
	, _valid(initial.valid) {
	}

	MetadataMap initialize(QByteArray legacyRevisions, QByteArray legacyHidden) {
		if (!_valid) {
			return {};
		}
		if (!_complete) {
			auto partitions = base::flat_set<int>();
			for (const auto &[id, value] : _index) {
				if (value.revisionPartition) {
					partitions.emplace(value.revisionPartition);
				}
			}
			_complete = true;
			for (const auto partition : partitions) {
				const auto parsed = readPartition(partition, true);
				if (!parsed) {
					_complete = false;
					continue;
				}
				for (const auto &[id, value] : parsed->metadata) {
					_index[id] = value;
				}
			}
		}
		_migrated = importLegacy(std::move(legacyRevisions), std::move(legacyHidden));
		prune();
		if (_complete && !_index.empty()) {
			saveMetadata();
		}
		return _index;
	}

	bool migrated() const {
		return _migrated;
	}

	std::optional<RevisionEntry> load(FullMsgId id) {
		const auto found = _index.find(id);
		if (found == _index.end() || !found->second.revisionPartition
			|| Expired(found->second, base::unixtime::now())) {
			return RevisionEntry();
		}
		const auto parsed = readPartition(found->second.revisionPartition, false, id);
		if (!parsed) {
			return std::nullopt;
		}
		const auto entry = parsed->revisions.find(id);
		return entry != parsed->revisions.end()
			? std::optional(entry->second)
			: std::optional(RevisionEntry());
	}

	Metadata record(
			FullMsgId id,
			RevisionSnapshot before,
			std::optional<RevisionSnapshot> after,
			TimeId deleted) {
		if (!_valid) {
			return value(id);
		}
		auto loaded = load(id);
		if (!loaded) {
			return value(id);
		}
		auto &entry = *loaded;
		auto changed = false;
		if (entry.versions.empty()) {
			entry.versions.push_back(std::move(before));
			changed = true;
		}
		if (after && !SameRevision(entry.versions.back(), *after)) {
			entry.versions.push_back(std::move(*after));
			changed = true;
		}
		if (deleted && !entry.deletedDate) {
			entry.deletedDate = deleted;
			changed = true;
		}
		if (changed && (entry.versions.size() > 1 || entry.deletedDate)) {
			saveEntry(id, std::move(entry));
		}
		return value(id);
	}

	Metadata hide(FullMsgId id, TimeId date) {
		if (!_valid || value(id).hiddenPartition) {
			return value(id);
		}
		const auto partition = PartitionFromDate(date);
		auto data = readForWrite(partition);
		if (!data) {
			return value(id);
		}
		data->hidden[id] = date;
		auto next = value(id);
		next.hiddenPartition = partition;
		commit(partition, *data, id, next);
		return value(id);
	}

private:
	Metadata value(FullMsgId id) const {
		const auto i = _index.find(id);
		return i != _index.end() ? i->second : Metadata();
	}

	std::optional<Snapshot> readPartition(
			int partition,
			bool metadataOnly,
			std::optional<FullMsgId> selected = std::nullopt) const {
		return ParsePartition(
			ReadEncryptedPayload(_path, PartitionFile(partition).toUtf8(), _key),
			_index,
			partition,
			metadataOnly,
			selected);
	}

	std::optional<Snapshot> readForWrite(int partition) const {
		if (!QFile::exists(_path + PartitionFile(partition))) {
			for (const auto &[id, entry] : _index) {
				if (entry.revisionPartition == partition
					|| entry.hiddenPartition == partition) {
					return std::nullopt;
				}
			}
			return Snapshot();
		}
		return readPartition(partition, false);
	}

	bool saveIndex() {
		return WriteEncryptedPayload(
			_path, QByteArray(kIndexFile), SerializeIndex(_index), _key);
	}

	void saveMetadata() {
		if (!WriteEncryptedPayload(
			_path, QByteArray(kMetadataFile), SerializeMetadata(_index, _path), _key)) {
			LOG(("Forkgram Error: Could not write message metadata cache."));
		}
	}

	bool commit(
			int partition,
			const Snapshot &data,
			FullMsgId id,
			Metadata next,
			bool updateCache = true) {
		QFile::remove(_path + QString::fromLatin1(kMetadataFile));
		if (!WriteEncryptedPayload(
			_path, PartitionFile(partition).toUtf8(),
			SerializePartition(data, partition), _key)) {
			LOG(("Forkgram Error: Could not write message archive partition."));
			return false;
		}
		const auto previous = value(id);
		const auto existed = _index.contains(id);
		_index[id] = next;
		if (!saveIndex()) {
			if (existed) {
				_index[id] = previous;
			} else {
				_index.remove(id);
			}
			LOG(("Forkgram Error: Could not write message archive index."));
			return false;
		}
		if (updateCache && _complete && !_index.empty()) {
			saveMetadata();
		}
		return true;
	}

	bool saveEntry(FullMsgId id, RevisionEntry entry) {
		const auto previousPartition = value(id).revisionPartition;
		const auto partition = PartitionForRevisionEntry(entry, base::unixtime::now());
		auto data = readForWrite(partition);
		if (!data) {
			return false;
		}
		auto next = value(id);
		next.revisionPartition = partition;
		next.versionsCount = int(entry.versions.size());
		next.deletedDate = entry.deletedDate;
		data->revisions[id] = std::move(entry);
		if (!commit(partition, *data, id, next, false)) {
			return false;
		}
		data.reset();
		if (previousPartition && previousPartition != partition) {
			compact(previousPartition);
		}
		if (_complete) {
			saveMetadata();
		}
		return true;
	}

	void compact(int partition) {
		const auto referenced = std::any_of(_index.begin(), _index.end(), [&](const auto &value) {
			return value.second.revisionPartition == partition
				|| value.second.hiddenPartition == partition;
		});
		if (!referenced) {
			QFile::remove(_path + PartitionFile(partition));
			return;
		}
		const auto data = readForWrite(partition);
		if (!data || !WriteEncryptedPayload(
			_path,
			PartitionFile(partition).toUtf8(),
			SerializePartition(*data, partition),
			_key)) {
			LOG(("Forkgram Error: Could not compact message archive partition."));
		}
	}

	void prune() {
		const auto now = base::unixtime::now();
		auto partitions = base::flat_set<int>();
		for (const auto &[id, entry] : _index) {
			if (Expired(entry, now)) {
				partitions.emplace(entry.revisionPartition);
			}
		}
		for (const auto partition : partitions) {
			auto parsed = readForWrite(partition);
			if (!parsed) {
				continue;
			}
			auto removed = std::vector<FullMsgId>();
			for (auto i = parsed->revisions.begin(); i != parsed->revisions.end();) {
				if (Expired(value(i->first), now)) {
					removed.push_back(i->first);
					i = parsed->revisions.erase(i);
				} else {
					++i;
				}
			}
			QFile::remove(_path + QString::fromLatin1(kMetadataFile));
			if (!WriteEncryptedPayload(_path, PartitionFile(partition).toUtf8(),
				SerializePartition(*parsed, partition), _key)) {
				continue;
			}
			for (const auto id : removed) {
				auto &entry = _index[id];
				entry.revisionPartition = 0;
				entry.versionsCount = 0;
				entry.deletedDate = 0;
				if (!entry.hiddenPartition) {
					_index.remove(id);
				}
			}
			saveIndex();
		}
	}

	bool importLegacy(QByteArray revisions, QByteArray hidden) {
		struct Reference {
			FullMsgId id;
			qint64 offset = 0;
		};
		auto groups = base::flat_map<int, std::vector<Reference>>();
		auto existing = base::flat_map<int, std::vector<FullMsgId>>();
		const auto now = base::unixtime::now();
		if (!revisions.isEmpty()) {
			auto stream = QDataStream(revisions);
			stream.setVersion(QDataStream::Qt_5_1);
			auto version = qint32();
			auto count = qint32();
			stream >> version >> count;
			if (version != 1 || count < 0 || count > revisions.size() / 24) {
				return false;
			}
			for (auto i = 0; i != count; ++i) {
				const auto offset = stream.device()->pos();
				auto peer = quint64();
				auto msg = qint64();
				auto deleted = qint32();
				auto versions = qint32();
				stream >> peer >> msg >> deleted >> versions;
				if (versions < 0 || versions > revisions.size() / 24) {
					return false;
				}
				auto latest = TimeId(deleted);
				for (auto j = 0; j != versions; ++j) {
					auto date = qint32();
					auto edited = qint32();
					auto entities = qint32();
					stream >> date >> edited >> entities;
					latest = std::max(latest, std::max(date, edited));
					if (!SkipField(stream) || !SkipField(stream) || !SkipField(stream)) {
						return false;
					}
				}
				const auto id = FullMsgId(DeserializePeerId(peer), MsgId(msg));
				if (const auto partition = value(id).revisionPartition) {
					existing[partition].push_back(id);
				} else if (deleted || versions > 1) {
					groups[PartitionFromDate(latest ? latest : now)].push_back({ id, offset });
				}
			}
			if (stream.status() != QDataStream::Ok || !stream.atEnd()) {
				return false;
			}
		}
		for (const auto &[partition, ids] : existing) {
			const auto parsed = readPartition(partition, true);
			if (!parsed) {
				return false;
			}
			for (const auto id : ids) {
				if (!parsed->metadata.contains(id)) {
					return false;
				}
			}
		}
		for (const auto &[partition, references] : groups) {
			auto data = readForWrite(partition);
			if (!data) {
				return false;
			}
			auto updates = std::vector<MetadataMap::value_type>();
			auto entries = std::vector<RevisionMap::value_type>();
			entries.reserve(data->revisions.size() + references.size());
			for (auto &[id, entry] : data->revisions) {
				entries.emplace_back(id, std::move(entry));
			}
			for (const auto &reference : references) {
				auto stream = QDataStream(revisions);
				stream.setVersion(QDataStream::Qt_5_1);
				if (!stream.device()->seek(reference.offset)) {
					return false;
				}
				auto peer = quint64();
				auto msg = qint64();
				auto deleted = qint32();
				auto versions = qint32();
				stream >> peer >> msg >> deleted >> versions;
				auto entry = RevisionEntry{ .deletedDate = deleted };
				for (auto j = 0; j != versions; ++j) {
					auto snapshot = RevisionSnapshot();
					stream >> snapshot.date >> snapshot.editDate >> snapshot.entitiesCount
						>> snapshot.raw >> snapshot.text >> snapshot.media;
					entry.versions.push_back(std::move(snapshot));
				}
				auto metadata = value(reference.id);
				metadata.revisionPartition = partition;
				metadata.versionsCount = versions;
				metadata.deletedDate = deleted;
				if (!Expired(metadata, now)) {
					updates.emplace_back(reference.id, metadata);
					entries.emplace_back(reference.id, std::move(entry));
				}
			}
			if (updates.empty()) {
				continue;
			}
			data->revisions = RevisionMap(
				std::make_move_iterator(entries.begin()),
				std::make_move_iterator(entries.end()));
			QFile::remove(_path + QString::fromLatin1(kMetadataFile));
			if (!WriteEncryptedPayload(
				_path, PartitionFile(partition).toUtf8(),
				SerializePartition(*data, partition), _key)) {
				return false;
			}
			for (const auto &[id, metadata] : updates) {
				_index[id] = metadata;
			}
			if (!saveIndex()) {
				for (const auto &[id, metadata] : updates) {
					_index[id].revisionPartition = 0;
					_index[id].versionsCount = 0;
					_index[id].deletedDate = 0;
					if (!_index[id].hiddenPartition) {
						_index.remove(id);
					}
				}
				return false;
			}
		}
		if (!hidden.isEmpty()) {
			auto stream = QDataStream(hidden);
			stream.setVersion(QDataStream::Qt_5_1);
			auto version = qint32();
			auto count = qint32();
			stream >> version >> count;
			if (version != 1 || count < 0 || count > hidden.size() / 16) {
				return false;
			}
			const auto partition = PartitionFromDate(now);
			auto data = readForWrite(partition);
			if (!data) {
				return false;
			}
			auto added = std::vector<FullMsgId>();
			for (auto i = 0; i != count; ++i) {
				auto peer = quint64();
				auto msg = qint64();
				stream >> peer >> msg;
				const auto id = FullMsgId(DeserializePeerId(peer), MsgId(msg));
				if (!value(id).hiddenPartition) {
					data->hidden[id] = now;
					added.push_back(id);
				}
			}
			if (stream.status() != QDataStream::Ok || !stream.atEnd()) {
				return false;
			}
			if (!added.empty()) {
				QFile::remove(_path + QString::fromLatin1(kMetadataFile));
				if (!WriteEncryptedPayload(
					_path, PartitionFile(partition).toUtf8(),
					SerializePartition(*data, partition), _key)) {
					return false;
				}
				for (const auto id : added) {
					_index[id].hiddenPartition = partition;
				}
				if (!saveIndex()) {
					for (const auto id : added) {
						_index[id].hiddenPartition = 0;
						if (!_index[id].revisionPartition) {
							_index.remove(id);
						}
					}
					return false;
				}
			}
		}
		return true;
	}

	QString _path;
	MTP::AuthKeyPtr _key;
	IndexMap _index;
	bool _complete = false;
	bool _valid = false;
	bool _migrated = false;

};

} // namespace

Snapshot Read(const QString &path, MTP::AuthKeyPtr localKey) {
	auto result = Snapshot();
	result.valid = !QFile::exists(path + QString::fromLatin1(kIndexFile))
		&& QDir(path).entryList({ u"messages_??????.aegis"_q }, QDir::Files).empty();
	result.metadata = DeserializeIndex(ReadEncryptedPayload(
		path, QByteArray(kIndexFile), localKey), result.valid);
	result.complete = result.valid && (result.metadata.empty() || ApplyMetadata(
		result.metadata,
		ReadEncryptedPayload(path, QByteArray(kMetadataFile), localKey),
		path));
	return result;
}

struct Store::Private {
	Private(QString path, MTP::AuthKeyPtr key, Snapshot initial)
	: metadata(initial.metadata)
	, worker(std::move(path), std::move(key), std::move(initial)) {
	}

	MetadataMap metadata;
	base::flat_map<FullMsgId, uint64> generations;
	rpl::event_stream<std::vector<FullMsgId>> changes;
	crl::object_on_thread<Worker> worker;
	uint64 generation = 0;
};

Store::Store(
		QString path,
		MTP::AuthKeyPtr localKey,
		QByteArray legacyRevisions,
		QByteArray legacyHidden,
		Fn<void()> migrated)
: _private(std::make_unique<Private>(path, localKey, Read(path, localKey))) {
	_private->worker.with([
		weak = base::make_weak(this),
		legacyRevisions = std::move(legacyRevisions),
		legacyHidden = std::move(legacyHidden),
		migrated = std::move(migrated)
	](Worker &worker) mutable {
		auto metadata = worker.initialize(std::move(legacyRevisions), std::move(legacyHidden));
		const auto imported = worker.migrated();
		crl::on_main([
			weak,
			imported,
			migrated = std::move(migrated),
			metadata = std::move(metadata)
		]() mutable {
			if (const auto that = weak.get()) {
				that->initialized(
					std::move(metadata),
					imported ? std::move(migrated) : nullptr);
			}
		});
	});
}

void Store::initialized(MetadataMap metadata, Fn<void()> migrated) {
	for (const auto &[id, generation] : _private->generations) {
		metadata[id] = this->metadata(id);
	}
	auto changed = std::vector<FullMsgId>();
	changed.reserve(_private->metadata.size() + metadata.size());
	for (const auto &[id, value] : _private->metadata) {
		if (!metadata.contains(id)) {
			changed.push_back(id);
		}
	}
	_private->metadata = std::move(metadata);
	for (const auto &[id, value] : _private->metadata) {
		changed.push_back(id);
	}
	_private->changes.fire(std::move(changed));
	if (migrated) {
		migrated();
	}
}

Store::~Store() {
	_private->worker.with_sync([](Worker &) {});
}

Metadata Store::metadata(FullMsgId id) const {
	const auto i = _private->metadata.find(id);
	if (i == _private->metadata.end()) {
		return {};
	}
	auto result = i->second;
	if (Expired(result, base::unixtime::now())) {
		result.revisionPartition = result.versionsCount = result.deletedDate = 0;
	}
	return result;
}

rpl::producer<std::vector<FullMsgId>> Store::changes() const {
	return _private->changes.events();
}

void Store::load(FullMsgId id, Fn<void(std::optional<RevisionEntry>)> done) {
	_private->worker.with([
		weak = base::make_weak(this),
		id,
		done = std::move(done)
	](Worker &worker) mutable {
		auto result = worker.load(id);
		crl::on_main([weak, done = std::move(done), result = std::move(result)]() mutable {
			if (weak) {
				done(std::move(result));
			}
		});
	});
}

void Store::update(FullMsgId id, Metadata metadata, uint64 generation) {
	if (_private->generations[id] != generation) {
		return;
	}
	_private->metadata[id] = metadata;
	_private->changes.fire(std::vector<FullMsgId>{ id });
}

void Store::record(FullMsgId id, RevisionSnapshot before, RevisionSnapshot after) {
	if (SameRevision(before, after)) {
		return;
	}
	const auto generation = ++_private->generation;
	_private->generations[id] = generation;
	auto &value = _private->metadata[id];
	value.versionsCount = std::max(value.versionsCount, 1) + 1;
	value.revisionPartition = PartitionFromDate(std::max(after.date, after.editDate));
	_private->worker.with([weak = base::make_weak(this), id, generation,
		before = std::move(before), after = std::move(after)](Worker &worker) mutable {
		const auto value = worker.record(id, std::move(before), std::move(after), 0);
		crl::on_main([weak, id, generation, value] {
			if (const auto that = weak.get()) {
				that->update(id, value, generation);
			}
		});
	});
}

void Store::markDeleted(FullMsgId id, RevisionSnapshot snapshot, TimeId date) {
	const auto generation = ++_private->generation;
	_private->generations[id] = generation;
	auto &value = _private->metadata[id];
	if (!value.deletedDate) {
		value.deletedDate = date;
	}
	value.versionsCount = std::max(value.versionsCount, 1);
	value.revisionPartition = PartitionFromDate(std::max(
		date,
		std::max(snapshot.date, snapshot.editDate)));
	_private->worker.with([weak = base::make_weak(this), id, generation, date,
		snapshot = std::move(snapshot)](Worker &worker) mutable {
		const auto value = worker.record(id, std::move(snapshot), std::nullopt, date);
		crl::on_main([weak, id, generation, value] {
			if (const auto that = weak.get()) {
				that->update(id, value, generation);
			}
		});
	});
}

void Store::hide(FullMsgId id, TimeId date) {
	const auto generation = ++_private->generation;
	_private->generations[id] = generation;
	_private->metadata[id].hiddenPartition = PartitionFromDate(date);
	_private->worker.with([weak = base::make_weak(this), id, generation, date](Worker &worker) {
		const auto value = worker.hide(id, date);
		crl::on_main([weak, id, generation, value] {
			if (const auto that = weak.get()) {
				that->update(id, value, generation);
			}
		});
	});
}

} // namespace Forkgram::LocalMessageState
