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
#include <QtCore/QIODevice>
#include <QtCore/QSaveFile>

namespace Forkgram::LocalMessageState {
namespace {

constexpr auto kIndexVersion = qint32(1);
constexpr auto kPartitionVersion = qint32(1);
constexpr auto kEncryptedFileVersion = qint32(1);
constexpr auto kFileMagic = "FGLS";
constexpr auto kIndexFile = "messages_index.aegis";
constexpr auto kFileSuffix = ".aegis";

struct IndexEntry {
	int revisionPartition = 0;
	int hiddenPartition = 0;
};

using IndexMap = base::flat_map<FullMsgId, IndexEntry>;

struct WriteRequest {
	IndexMap index;
	Snapshot partitions;
	base::flat_set<int> dirtyPartitions;
	bool rewriteAll = false;
};

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

[[nodiscard]] IndexMap DeserializeIndex(const QByteArray &bytes) {
	if (bytes.isEmpty()) {
		return {};
	}
	auto stream = QDataStream(bytes);
	stream.setVersion(QDataStream::Qt_5_1);
	auto version = qint32();
	auto count = qint32();
	stream >> version >> count;
	if (version != kIndexVersion || count < 0) {
		return {};
	}
	auto result = IndexMap();
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
		result.emplace(
			FullMsgId(DeserializePeerId(peerSerialized), MsgId(msg)),
			IndexEntry{
				.revisionPartition = int(revisionPartition),
				.hiddenPartition = int(hiddenPartition),
			});
	}
	return (stream.status() == QDataStream::Ok) ? result : IndexMap();
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

void MergePartition(
		Snapshot &result,
		const QByteArray &bytes,
		const IndexMap &index,
		int expectedPartition) {
	if (bytes.isEmpty()) {
		return;
	}
	auto stream = QDataStream(bytes);
	stream.setVersion(QDataStream::Qt_5_1);
	auto version = qint32();
	auto partition = qint32();
	auto revisionsCount = qint32();
	stream >> version >> partition >> revisionsCount;
	if (version != kPartitionVersion
		|| partition != expectedPartition
		|| revisionsCount < 0) {
		return;
	}
	for (auto i = 0; i != revisionsCount; ++i) {
		auto peerSerialized = quint64();
		auto msg = qint64();
		auto deletedDate = qint32();
		auto versionsCount = qint32();
		stream >> peerSerialized >> msg >> deletedDate >> versionsCount;
		if (versionsCount < 0) {
			return;
		}
		auto id = FullMsgId(DeserializePeerId(peerSerialized), MsgId(msg));
		const auto indexEntry = index.find(id);
		if (indexEntry == index.end()
			|| indexEntry->second.revisionPartition != expectedPartition) {
			for (auto j = 0; j != versionsCount; ++j) {
				auto ignored = RevisionSnapshot();
				auto ignoredDate = qint32();
				auto ignoredEditDate = qint32();
				auto ignoredEntitiesCount = qint32();
				stream
					>> ignoredDate
					>> ignoredEditDate
					>> ignoredEntitiesCount
					>> ignored.raw
					>> ignored.text
					>> ignored.media;
			}
			continue;
		}
		auto entry = RevisionEntry();
		entry.deletedDate = deletedDate;
		entry.versions.reserve(versionsCount);
		for (auto j = 0; j != versionsCount; ++j) {
			auto snapshot = RevisionSnapshot();
			auto date = qint32();
			auto editDate = qint32();
			auto entitiesCount = qint32();
			stream
				>> date
				>> editDate
				>> entitiesCount
				>> snapshot.raw
				>> snapshot.text
				>> snapshot.media;
			snapshot.date = date;
			snapshot.editDate = editDate;
			snapshot.entitiesCount = entitiesCount;
			entry.versions.push_back(std::move(snapshot));
		}
		result.revisions.emplace(id, std::move(entry));
	}

	auto hiddenCount = qint32();
	stream >> hiddenCount;
	if (hiddenCount < 0) {
		return;
	}
	for (auto i = 0; i != hiddenCount; ++i) {
		auto peerSerialized = quint64();
		auto msg = qint64();
		auto date = qint32();
		stream >> peerSerialized >> msg >> date;
		const auto id = FullMsgId(DeserializePeerId(peerSerialized), MsgId(msg));
		const auto indexEntry = index.find(id);
		if (indexEntry != index.end()
			&& indexEntry->second.hiddenPartition == expectedPartition) {
			result.hidden.emplace(id, date);
		}
	}
	if (stream.status() != QDataStream::Ok) {
		result = Snapshot();
	}
}

[[nodiscard]] IndexMap BuildIndex(
		const RevisionMap &revisions,
		const HiddenMap &hidden,
		TimeId now) {
	auto result = IndexMap();
	for (const auto &[id, entry] : revisions) {
		result[id].revisionPartition = PartitionForRevisionEntry(entry, now);
	}
	for (const auto &[id, date] : hidden) {
		result[id].hiddenPartition = PartitionFromDate(date ? date : now);
	}
	return result;
}

[[nodiscard]] WriteRequest MakeWriteRequest(
		const RevisionMap &revisions,
		const HiddenMap &hidden,
		const base::flat_set<int> &dirtyPartitions,
		bool rewriteAll) {
	const auto now = base::unixtime::now();
	auto result = WriteRequest{
		.index = BuildIndex(revisions, hidden, now),
		.dirtyPartitions = dirtyPartitions,
		.rewriteAll = rewriteAll,
	};
	if (rewriteAll) {
		for (const auto &[id, entry] : result.index) {
			if (entry.revisionPartition) {
				result.dirtyPartitions.emplace(entry.revisionPartition);
			}
			if (entry.hiddenPartition) {
				result.dirtyPartitions.emplace(entry.hiddenPartition);
			}
		}
	}
	for (const auto &[id, entry] : revisions) {
		const auto partition = PartitionForRevisionEntry(entry, now);
		if (result.dirtyPartitions.contains(partition)) {
			result.partitions.revisions.emplace(id, entry);
		}
	}
	for (const auto &[id, date] : hidden) {
		const auto partition = PartitionFromDate(date ? date : now);
		if (result.dirtyPartitions.contains(partition)) {
			result.partitions.hidden.emplace(id, date ? date : now);
		}
	}
	return result;
}

void RemovePartitionFile(const QString &path, int partition) {
	QFile::remove(path + PartitionFile(partition));
}

[[nodiscard]] bool HasPartitionData(
		const Snapshot &snapshot,
		int partition) {
	for (const auto &[id, entry] : snapshot.revisions) {
		if (PartitionForRevisionEntry(entry, TimeId(1)) == partition) {
			return true;
		}
	}
	for (const auto &[id, date] : snapshot.hidden) {
		if (PartitionFromDate(date) == partition) {
			return true;
		}
	}
	return false;
}

class WriteManager final {
public:
	void write(
		QString path,
		MTP::AuthKeyPtr localKey,
		WriteRequest request) {
		if (request.index.empty()) {
			QFile::remove(path + QString::fromLatin1(kIndexFile));
		} else {
			const auto written = WriteEncryptedPayload(
				path,
				QByteArray(kIndexFile),
				SerializeIndex(request.index),
				localKey);
			if (!written) {
				LOG(("Forkgram Error: Could not write local message state index."));
			}
		}
		if (request.rewriteAll) {
			const auto files = QDir(path).entryList(
				{ u"messages_*%1"_q.arg(kFileSuffix) },
				QDir::Files);
			for (const auto &file : files) {
				if (file != QString::fromLatin1(kIndexFile)) {
					QFile::remove(path + file);
				}
			}
		}
		for (const auto partition : request.dirtyPartitions) {
			if (!partition) {
				continue;
			}
			const auto name = PartitionFile(partition).toUtf8();
			if (HasPartitionData(request.partitions, partition)) {
				const auto written = WriteEncryptedPayload(
					path,
					name,
					SerializePartition(request.partitions, partition),
					localKey);
				if (!written) {
					LOG(("Forkgram Error: Could not write local message state partition."));
				}
			} else {
				RemovePartitionFile(path, partition);
			}
		}
	}
};

[[nodiscard]] crl::object_on_thread<WriteManager> &Writer() {
	static auto result = crl::object_on_thread<WriteManager>();
	return result;
}

} // namespace

Snapshot Read(
		const QString &path,
		MTP::AuthKeyPtr localKey) {
	const auto index = DeserializeIndex(ReadEncryptedPayload(
		path,
		QByteArray(kIndexFile),
		localKey));
	if (index.empty()) {
		return {};
	}
	auto partitions = base::flat_set<int>();
	for (const auto &[id, entry] : index) {
		if (entry.revisionPartition) {
			partitions.emplace(entry.revisionPartition);
		}
		if (entry.hiddenPartition) {
			partitions.emplace(entry.hiddenPartition);
		}
	}
	auto result = Snapshot();
	for (const auto partition : partitions) {
		MergePartition(
			result,
			ReadEncryptedPayload(
				path,
				PartitionFile(partition).toUtf8(),
				localKey),
			index,
			partition);
	}
	return result;
}

void WriteAsync(
		const QString &path,
		MTP::AuthKeyPtr localKey,
		const RevisionMap &revisions,
		const HiddenMap &hidden,
		const base::flat_set<int> &dirtyPartitions,
		bool rewriteAll) {
	if (!rewriteAll && dirtyPartitions.empty()) {
		return;
	}
	auto request = MakeWriteRequest(
		revisions,
		hidden,
		dirtyPartitions,
		rewriteAll);
	if (!rewriteAll && request.dirtyPartitions.empty()) {
		return;
	}
	Writer().with([
		path,
		localKey = std::move(localKey),
		request = std::move(request)
	](WriteManager &writer) mutable {
		writer.write(std::move(path), std::move(localKey), std::move(request));
	});
}

} // namespace Forkgram::LocalMessageState
