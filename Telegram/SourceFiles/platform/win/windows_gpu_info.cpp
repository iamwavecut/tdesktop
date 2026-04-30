/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/win/windows_gpu_info.h"

#include "base/debug_log.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QVersionNumber>

#include <windows.h>

// Must live in the global namespace — Q_INIT_RESOURCE's extern
// declaration would otherwise land inside a namespace and fail to
// resolve against the generator-emitted global symbol.
static void InitLibUiWinResources() {
	Q_INIT_RESOURCE(win);
}

namespace Platform {
namespace {

constexpr auto kBlacklistPath = ":/misc/gpu_driver_bug_list.json";

[[nodiscard]] std::optional<uint32> ParseHex(QStringView text) {
	auto s = text;
	if (s.startsWith(u"0x", Qt::CaseInsensitive)) {
		s = s.mid(2);
	}
	auto ok = false;
	const auto value = s.toUInt(&ok, 16);
	return ok ? std::make_optional(value) : std::nullopt;
}

[[nodiscard]] QJsonDocument LoadBlacklist() {
	InitLibUiWinResources();
	auto file = QFile(kBlacklistPath);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	const auto bytes = file.readAll();
	auto error = QJsonParseError();
	auto doc = QJsonDocument::fromJson(bytes, &error);
	if (error.error != QJsonParseError::NoError) {
		LOG(("GPU blacklist parse error: %1 at offset %2"
			).arg(error.errorString()
			).arg(error.offset));
		return {};
	}
	return doc;
}

[[nodiscard]] bool MatchesVersionConstraint(
		const QString &actual,
		const QJsonObject &constraint) {
	const auto op = constraint.value(u"op"_q).toString();
	const auto target = constraint.value(u"value"_q).toString();
	if (op.isEmpty() || target.isEmpty()) {
		return true;
	}
	const auto a = QVersionNumber::fromString(actual);
	const auto b = QVersionNumber::fromString(target);
	if (a.isNull() || b.isNull()) {
		return false;
	}
	const auto cmp = QVersionNumber::compare(a, b);
	if (op == u"=") {
		return cmp == 0;
	} else if (op == u"<") {
		return cmp < 0;
	} else if (op == u"<=") {
		return cmp <= 0;
	} else if (op == u">") {
		return cmp > 0;
	} else if (op == u">=") {
		return cmp >= 0;
	}
	return false;
}

[[nodiscard]] bool EntryMatches(
		const QJsonObject &entry,
		const GpuInfo &gpu,
		QLatin1String feature) {
	const auto features = entry.value(u"features"_q).toArray();
	auto hasFeature = false;
	for (const auto &f : features) {
		if (f.toString() == feature) {
			hasFeature = true;
			break;
		}
	}
	if (!hasFeature) {
		return false;
	}

	const auto os = entry.value(u"os"_q).toObject();
	const auto osType = os.value(u"type"_q).toString();
	if (!osType.isEmpty() && osType != u"win") {
		return false;
	}

	if (const auto v = entry.value(u"vendor_id"_q); !v.isUndefined()) {
		const auto parsed = ParseHex(v.toString());
		if (!parsed || *parsed != gpu.vendorId) {
			return false;
		}
	}

	if (const auto d = entry.value(u"device_id"_q); !d.isUndefined()) {
		const auto array = d.toArray();
		auto deviceMatches = false;
		for (const auto &item : array) {
			const auto parsed = ParseHex(item.toString());
			if (parsed && *parsed == gpu.deviceId) {
				deviceMatches = true;
				break;
			}
		}
		if (!deviceMatches) {
			return false;
		}
	}

	if (const auto dv = entry.value(u"driver_version"_q); !dv.isUndefined()) {
		if (gpu.driverVersion.isEmpty()
			|| !MatchesVersionConstraint(gpu.driverVersion, dv.toObject())) {
			return false;
		}
	}

	return true;
}

} // namespace

GpuInfo EnumeratePrimaryGpu() {
	auto result = GpuInfo();
	DISPLAY_DEVICEW dd{};
	dd.cb = sizeof(dd);
	for (auto i = DWORD(0); EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
		if (!(dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)) {
			continue;
		}
		const auto deviceId = QString::fromWCharArray(dd.DeviceID);
		static const auto regex = QRegularExpression(
			u"VEN_([0-9A-Fa-f]{4}).*DEV_([0-9A-Fa-f]{4})"_q);
		const auto match = regex.match(deviceId);
		if (!match.hasMatch()) {
			break;
		}
		auto ok = false;
		result.vendorId = match.captured(1).toUInt(&ok, 16);
		if (!ok) {
			result.vendorId = 0;
			break;
		}
		result.deviceId = match.captured(2).toUInt(&ok, 16);
		if (!ok) {
			result.vendorId = 0;
			result.deviceId = 0;
			break;
		}
		result.deviceString = QString::fromWCharArray(dd.DeviceString);
		break;
	}
	return result;
}

bool GpuBlacklistedForFeature(const GpuInfo &gpu, QLatin1String feature) {
	if (!gpu.valid()) {
		return false;
	}
	const auto doc = LoadBlacklist();
	if (doc.isNull()) {
		return false;
	}
	const auto entries = doc.object().value(u"entries"_q).toArray();
	for (const auto &value : entries) {
		const auto entry = value.toObject();
		if (EntryMatches(entry, gpu, feature)) {
			LOG(("GPU blacklist: vendor=0x%1 device=0x%2 driver='%3' "
				"matched entry id=%4 feature=%5 desc='%6'"
				).arg(gpu.vendorId, 4, 16, QChar('0')
				).arg(gpu.deviceId, 4, 16, QChar('0')
				).arg(gpu.driverVersion
				).arg(entry.value(u"id"_q).toInt()
				).arg(feature
				).arg(entry.value(u"description"_q).toString()));
			return true;
		}
	}
	return false;
}

} // namespace Platform
