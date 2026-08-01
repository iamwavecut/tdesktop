/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/

#pragma once

#include "platform/platform_webauthn.h"

namespace Platform::WebAuthn::Local {

[[nodiscard]] bool IsSupported();
[[nodiscard]] bool HasKeys(bool testServer);
void RegisterKey(
	const Data::Passkey::RegisterData &data,
	bool testServer,
	Fn<void(RegisterResult result)> callback);
void Login(
	const Data::Passkey::LoginData &data,
	bool testServer,
	Fn<void(LoginResult result)> callback);
void RemoveKey(const QString &credentialId);

} // namespace Platform::WebAuthn::Local
