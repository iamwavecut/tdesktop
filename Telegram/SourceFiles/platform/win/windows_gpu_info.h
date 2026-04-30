/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Platform {

struct GpuInfo {
	uint32 vendorId = 0;
	uint32 deviceId = 0;
	QString driverVersion;
	QString deviceString;

	[[nodiscard]] bool valid() const {
		return vendorId != 0;
	}
};

[[nodiscard]] GpuInfo EnumeratePrimaryGpu();

[[nodiscard]] bool GpuBlacklistedForFeature(
	const GpuInfo &gpu,
	QLatin1String feature);

} // namespace Platform
