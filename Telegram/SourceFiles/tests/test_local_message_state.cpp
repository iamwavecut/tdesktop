#include "forkgram/local_message_state.h"

#include "base/openssl_help.h"
#include "base/unixtime.h"
#include <crl/crl.h>
#include "mtproto/mtproto_auth_key.h"

#include <sodium.h>
#include <QtGui/QGuiApplication>
#include <QtCore/QDataStream>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QTemporaryDir>
#include <iostream>

namespace {

void Require(bool condition, const char *message) {
	if (!condition) {
		std::cerr << message << '\n';
		std::exit(1);
	}
}

MTP::AuthKeyPtr TestKey() {
	auto data = MTP::AuthKey::Data();
	std::fill(data.begin(), data.end(), gsl::byte(0x42));
	return std::make_shared<MTP::AuthKey>(data);
}

void WriteFixtureFile(
		const QString &path,
		const QByteArray &name,
		const QByteArray &payload,
		const MTP::AuthKeyPtr &localKey) {
	Require(sodium_init() >= 0, "sodium initialization failed");
	const auto context = QByteArray("Forkgram local message state AEGIS-128L v1");
	const auto hash = openssl::Sha256(localKey->data(), bytes::make_span(context));
	const auto nonce = QByteArray(crypto_aead_aegis128l_NPUBBYTES, char(0));
	const auto ad = QByteArray("FGLS", 4) + name;
	auto encrypted = QByteArray(
		payload.size() + crypto_aead_aegis128l_ABYTES,
		Qt::Uninitialized);
	auto length = static_cast<unsigned long long>(0);
	Require(crypto_aead_aegis128l_encrypt(
		reinterpret_cast<unsigned char*>(encrypted.data()),
		&length,
		reinterpret_cast<const unsigned char*>(payload.constData()),
		payload.size(),
		reinterpret_cast<const unsigned char*>(ad.constData()),
		ad.size(),
		nullptr,
		reinterpret_cast<const unsigned char*>(nonce.constData()),
		reinterpret_cast<const unsigned char*>(hash.data())) == 0,
		"fixture encryption failed");
	encrypted.resize(int(length));
	auto file = QFile(path + QString::fromUtf8(name));
	Require(file.open(QIODevice::WriteOnly), "fixture file could not be opened");
	auto stream = QDataStream(&file);
	stream.setVersion(QDataStream::Qt_5_1);
	stream.writeRawData("FGLS", 4);
	stream << qint32(1) << nonce << encrypted;
	Require(stream.status() == QDataStream::Ok, "fixture write failed");
}

void WriteFixture(const QString &path, const MTP::AuthKeyPtr &key) {
	auto index = QByteArray();
	auto stream = QDataStream(&index, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << qint32(1) << qint32(2);
	stream << quint64(1001) << qint64(42) << qint32(202609) << qint32(0);
	stream << quint64(1001) << qint64(99) << qint32(0) << qint32(202609);
	WriteFixtureFile(path, "messages_index.aegis", index, key);

	auto partition = QByteArray();
	auto part = QDataStream(&partition, QIODevice::WriteOnly);
	part.setVersion(QDataStream::Qt_5_1);
	part << qint32(1) << qint32(202609) << qint32(1);
	part << quint64(1001) << qint64(42) << qint32(0) << qint32(2);
	for (const auto &text : { u"original"_q, u"edited"_q }) {
		part << qint32(1789000000) << qint32(1789000001) << qint32(0)
			<< QByteArray() << text << QString();
	}
	part << qint32(1) << quint64(1001) << qint64(99) << qint32(1789000000);
	WriteFixtureFile(path, "messages_202609.aegis", partition, key);
}

void TestStartupDoesNotMaterializeRevisionPayloads() {
	const auto directory = QTemporaryDir();
	Require(directory.isValid(), "temporary directory unavailable");
	const auto path = directory.path() + u"/"_q;
	const auto key = TestKey();
	WriteFixture(path, key);
	const auto snapshot = Forkgram::LocalMessageState::Read(path, key);
	Require(snapshot.revisions.empty(),
		"startup metadata read materialized revision payloads");
}

using namespace Forkgram::LocalMessageState;

FullMsgId EditedId() {
	return FullMsgId(peerFromUser(UserId(1001)), MsgId(42));
}

FullMsgId HiddenId() {
	return FullMsgId(peerFromUser(UserId(1001)), MsgId(99));
}

QByteArray FileBytes(const QString &path) {
	auto file = QFile(path);
	Require(file.open(QIODevice::ReadOnly), "could not read fixture");
	return file.readAll();
}

std::optional<RevisionEntry> Load(Store &store, FullMsgId id) {
	auto loop = QEventLoop();
	auto received = false;
	auto result = std::optional<RevisionEntry>();
	QTimer::singleShot(10000, &loop, &QEventLoop::quit);
	store.load(id, [&](std::optional<RevisionEntry> entry) {
		received = true;
		result = std::move(entry);
		loop.quit();
	});
	loop.exec();
	Require(received, "background load did not finish");
	return result;
}

RevisionSnapshot Version(const QString &text, TimeId date) {
	return { .text = text, .date = date, .editDate = date };
}

void TestDemandLoadingAndWarmMetadata() {
	const auto directory = QTemporaryDir();
	const auto path = directory.path() + u"/"_q;
	const auto key = TestKey();
	WriteFixture(path, key);
	{
		auto store = Store(path, key);
		Require(store.metadata(HiddenId()).hiddenPartition == 202609,
			"hidden state required reading payloads");
		const auto entry = Load(store, EditedId());
		Require(entry && entry->versions.size() == 2, "on-demand history missing");
		Require(entry->versions.front().text == u"original"_q
			&& entry->versions.back().text == u"edited"_q,
			"on-demand history content changed");
	}
	const auto snapshot = Read(path, key);
	Require(snapshot.complete && snapshot.revisions.empty(),
		"warm metadata cache was not usable");
	Require(snapshot.metadata.find(EditedId())->second.versionsCount == 2,
		"warm cache lost version count");
	Require(!QFile::exists(path + u"messages_202609.aegis.bak"_q),
		"unexpected archive copy");
}

void TestEditsDuringInitializationAndMonthMove() {
	const auto directory = QTemporaryDir();
	const auto path = directory.path() + u"/"_q;
	const auto key = TestKey();
	WriteFixture(path, key);
	const auto oldSize = QFileInfo(path + u"messages_202609.aegis"_q).size();
	const auto now = base::unixtime::now();
	const auto nextMonth = TimeId(QDateTime(QDate(2026, 10, 1), QTime(), Qt::UTC).toSecsSinceEpoch());
	{
		auto store = Store(path, key);
		store.record(EditedId(), Version(u"edited"_q, now), Version(u"third"_q, now));
		store.record(EditedId(), Version(u"third"_q, now), Version(u"fourth"_q, nextMonth));
		store.markDeleted(EditedId(), Version(u"fourth"_q, nextMonth), now);
		const auto entry = Load(store, EditedId());
		Require(entry && entry->versions.size() == 4,
			"queued edits replaced old history");
		Require(entry->versions.front().text == u"original"_q
			&& entry->versions.back().text == u"fourth"_q,
			"month move lost revision content");
		Require(entry->deletedDate == now, "deletion date missing");
		Require(store.metadata(EditedId()).versionsCount == 4,
			"initialization overwrote newer metadata");
	}
	Require(QFileInfo(path + u"messages_202609.aegis"_q).size() < oldSize,
		"month move retained obsolete revision payloads in the old partition");
	{
		auto store = Store(path, key);
		const auto entry = Load(store, EditedId());
		Require(entry && entry->versions.size() == 4 && entry->deletedDate == now,
			"shutdown or restart lost pending writes");
		Require(store.metadata(HiddenId()).hiddenPartition == 202609,
			"month rewrite lost hidden messages");
	}
}

void TestDuplicateAndHiddenPersistence() {
	const auto directory = QTemporaryDir();
	const auto path = directory.path() + u"/"_q;
	const auto key = TestKey();
	WriteFixture(path, key);
	const auto now = base::unixtime::now();
	const auto id = FullMsgId(EditedId().peer, MsgId(123));
	{
		auto store = Store(path, key);
		store.record(EditedId(), Version(u"edited"_q, 1789000001), Version(u"edited"_q, 1789000001));
		store.hide(id, now);
		const auto entry = Load(store, EditedId());
		Require(entry && entry->versions.size() == 2, "duplicate revision was appended");
	}
	const auto snapshot = Read(path, key);
	Require(snapshot.metadata.find(id)->second.hiddenPartition == PartitionFromDate(now),
		"hidden message was not durable");
	Require(snapshot.metadata.find(HiddenId())->second.hiddenPartition == 202609,
		"rewriting a month dropped unrelated hidden messages");
}

void TestCorruptArchivesAndWrongKeyArePreserved() {
	for (const auto corruptIndex : { false, true }) {
		const auto directory = QTemporaryDir();
		const auto path = directory.path() + u"/"_q;
		const auto key = TestKey();
		WriteFixture(path, key);
		const auto fileName = corruptIndex ? u"messages_index.aegis"_q : u"messages_202609.aegis"_q;
		{
			auto file = QFile(path + fileName);
			Require(file.open(QIODevice::WriteOnly), "could not corrupt fixture");
			Require(file.write("corrupted") == 9, "could not corrupt fixture bytes");
		}
		const auto archive = FileBytes(path + u"messages_202609.aegis"_q);
		const auto index = FileBytes(path + u"messages_index.aegis"_q);
		{
			auto store = Store(path, key);
			store.record(EditedId(), Version(u"before"_q, base::unixtime::now()),
				Version(u"after"_q, base::unixtime::now()));
			store.hide(HiddenId(), base::unixtime::now());
			Load(store, EditedId());
		}
		Require(archive == FileBytes(path + u"messages_202609.aegis"_q)
			&& index == FileBytes(path + u"messages_index.aegis"_q),
			"failed archive read destroyed data");
	}
	const auto directory = QTemporaryDir();
	const auto path = directory.path() + u"/"_q;
	WriteFixture(path, TestKey());
	auto data = MTP::AuthKey::Data();
	std::fill(data.begin(), data.end(), gsl::byte(0x43));
	const auto wrong = std::make_shared<MTP::AuthKey>(data);
	const auto index = FileBytes(path + u"messages_index.aegis"_q);
	{
		auto store = Store(path, wrong);
		store.hide(HiddenId(), base::unixtime::now());
		Load(store, EditedId());
	}
	Require(index == FileBytes(path + u"messages_index.aegis"_q),
		"wrong key replaced the archive index");
}

void TestCacheInvalidationAndAccountIsolation() {
	const auto first = QTemporaryDir();
	const auto second = QTemporaryDir();
	const auto a = first.path() + u"/"_q;
	const auto b = second.path() + u"/"_q;
	const auto key = TestKey();
	WriteFixture(a, key);
	{
		auto store = Store(a, key);
		Load(store, EditedId());
	}
	Require(Read(a, key).complete, "metadata cache missing");
	{
		auto file = QFile(a + u"messages_202609.aegis"_q);
		Require(file.open(QIODevice::Append), "could not change archive fixture");
		file.write("changed");
	}
	Require(!Read(a, key).complete, "changed partition did not invalidate cache");
	{
		auto store = Store(b, key);
		Require(!store.metadata(EditedId()).revisionPartition,
			"another account's index leaked into empty account");
		const auto entry = Load(store, EditedId());
		Require(entry && entry->versions.empty(), "another account's history leaked");
	}
}

void TestDestroyedStoreDropsCallback() {
	const auto directory = QTemporaryDir();
	const auto path = directory.path() + u"/"_q;
	const auto key = TestKey();
	WriteFixture(path, key);
	auto called = false;
	{
		auto store = Store(path, key);
		store.load(EditedId(), [&](std::optional<RevisionEntry>) { called = true; });
	}
	auto loop = QEventLoop();
	QTimer::singleShot(50, &loop, &QEventLoop::quit);
	loop.exec();
	Require(!called, "load callback outlived account store");
}

QByteArray LegacyRevisions() {
	auto bytes = QByteArray();
	auto stream = QDataStream(&bytes, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << qint32(1) << qint32(1)
		<< SerializePeerId(EditedId().peer) << qint64(EditedId().msg.bare)
		<< qint32(0) << qint32(2);
	for (const auto &text : { u"legacy original"_q, u"legacy edited"_q }) {
		stream << qint32(base::unixtime::now()) << qint32(base::unixtime::now())
			<< qint32(0) << QByteArray() << text << QString();
	}
	return bytes;
}

QByteArray LegacyHidden() {
	auto bytes = QByteArray();
	auto stream = QDataStream(&bytes, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << qint32(1) << qint32(1)
		<< SerializePeerId(HiddenId().peer) << qint64(HiddenId().msg.bare);
	return bytes;
}

void TestLegacyMigrationPreservesHistoryAndAcknowledgesDurability() {
	const auto directory = QTemporaryDir();
	const auto path = directory.path() + u"/"_q;
	const auto key = TestKey();
	auto migrated = false;
	{
		auto store = Store(path, key, LegacyRevisions(), LegacyHidden(), [&] { migrated = true; });
		store.record(EditedId(), Version(u"legacy edited"_q, base::unixtime::now()),
			Version(u"new edit"_q, base::unixtime::now()));
		const auto entry = Load(store, EditedId());
		Require(entry && entry->versions.size() == 3
			&& entry->versions.front().text == u"legacy original"_q,
			"edit during migration lost legacy history");
		Require(migrated, "durable migration was not acknowledged");
	}
	const auto state = Read(path, key);
	Require(state.complete && state.metadata.contains(HiddenId()),
		"legacy hidden messages were not durable");
}

void TestFailedMigrationKeepsLegacySource() {
	const auto directory = QTemporaryDir();
	const auto path = directory.path() + u"/"_q;
	const auto key = TestKey();
	WriteFixture(path, key);
	{
		auto file = QFile(path + u"messages_202609.aegis"_q);
		Require(file.open(QIODevice::WriteOnly), "could not corrupt fixture");
		file.write("broken");
	}
	auto migrated = false;
	{
		auto store = Store(path, key, LegacyRevisions(), {}, [&] { migrated = true; });
		Require(!Load(store, EditedId()), "corrupt archive unexpectedly loaded");
	}
	Require(!migrated, "migration discarded legacy source despite unreadable archive");
}

void TestDeletedRetentionPreservesHiddenState() {
	const auto directory = QTemporaryDir();
	const auto path = directory.path() + u"/"_q;
	const auto key = TestKey();
	WriteFixture(path, key);
	{
		auto store = Store(path, key);
		store.markDeleted(EditedId(), Version(u"edited"_q, base::unixtime::now()),
			base::unixtime::now() - 8 * kSecondsInDay);
		Load(store, EditedId());
	}
	{
		auto store = Store(path, key);
		const auto entry = Load(store, EditedId());
		Require(entry && entry->versions.empty(), "expired deletion history was returned");
	}
	const auto state = Read(path, key);
	Require(!state.metadata.contains(EditedId()) && state.metadata.contains(HiddenId()),
		"retention removed unrelated hidden state or retained expired history");
}

void TestMissingIndexAndIncompletePartitionArePreserved() {
	for (const auto missingIndex : { false, true }) {
		const auto directory = QTemporaryDir();
		const auto path = directory.path() + u"/"_q;
		const auto key = TestKey();
		WriteFixture(path, key);
		if (missingIndex) {
			Require(QFile::remove(path + u"messages_index.aegis"_q), "could not remove fixture index");
		} else {
			auto payload = QByteArray();
			auto stream = QDataStream(&payload, QIODevice::WriteOnly);
			stream.setVersion(QDataStream::Qt_5_1);
			stream << qint32(1) << qint32(202609) << qint32(0) << qint32(0);
			WriteFixtureFile(path, "messages_202609.aegis", payload, key);
		}
		const auto archive = FileBytes(path + u"messages_202609.aegis"_q);
		{
			auto store = Store(path, key);
			store.record(EditedId(), Version(u"before"_q, base::unixtime::now()),
				Version(u"after"_q, base::unixtime::now()));
			Load(store, EditedId());
		}
		Require(archive == FileBytes(path + u"messages_202609.aegis"_q),
			"missing index or missing entries caused archive replacement");
	}
}

} // namespace

int main(int argc, char *argv[]) {
	const auto app = QGuiApplication(argc, argv);
	TestStartupDoesNotMaterializeRevisionPayloads();
	TestDemandLoadingAndWarmMetadata();
	TestEditsDuringInitializationAndMonthMove();
	TestDuplicateAndHiddenPersistence();
	TestCorruptArchivesAndWrongKeyArePreserved();
	TestCacheInvalidationAndAccountIsolation();
	TestDestroyedStoreDropsCallback();
	TestLegacyMigrationPreservesHistoryAndAcknowledgesDurability();
	TestFailedMigrationKeepsLegacySource();
	TestDeletedRetentionPreservesHiddenState();
	TestMissingIndexAndIncompletePartitionArePreserved();
	std::cout << "local message state tests passed\n";
	return 0;
}
