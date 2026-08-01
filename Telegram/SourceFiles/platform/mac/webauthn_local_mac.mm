/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/

#include "platform/mac/webauthn_local_mac.h"

#include "base/openssl_help.h"
#include "core/application.h"
#include "data/data_passkey_deserialize.h"
#include "lang/lang_keys.h"
#include "window/main_window.h"
#include "window/window_controller.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <LocalAuthentication/LocalAuthentication.h>
#import <Security/Security.h>

#include <QtCore/QCborStreamWriter>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace Platform::WebAuthn::Local {
namespace {

constexpr auto kCredentialIdSize = 32;
constexpr auto kAaguidSize = 16;
constexpr auto kCoordinateSize = 32;
constexpr auto kPublicKeySize = 1 + 2 * kCoordinateSize;
constexpr auto kAlgorithmEs256 = -7;
constexpr auto kFlagUserPresent = 0x01;
constexpr auto kFlagUserVerified = 0x04;
constexpr auto kFlagAttestedData = 0x40;

struct Credential {
	QByteArray id;
	QString rpId;
	QByteArray userHandle;
	QString name;
	QString displayName;
	bool testServer = false;
};

struct StoredCredential {
	Credential data;
	QByteArray tag;
};

// Only the name shown in Keychain Access, lookup goes by the metadata.
[[nodiscard]] NSString *ItemLabel(const Credential &credential) {
	auto result = u"Forkgram Desktop Passkey"_q;
	if (!credential.name.isEmpty()) {
		result += u" — "_q + credential.name;
	}
	result += credential.testServer
		? u" (Test server)"_q
		: u" (Production server)"_q;
	return result.toNSString();
}

[[nodiscard]] NSWindow *ResolveAnchorWindow() {
	if (Core::IsAppLaunched()) {
		const auto controller = Core::App().activeWindow()
			? Core::App().activeWindow()
			: Core::App().activePrimaryWindow();
		if (controller) {
			const auto view = reinterpret_cast<NSView*>(
				controller->widget()->winId());
			if (NSWindow *window = [view window]) {
				return window;
			}
		}
	}
	if (NSWindow *window = [NSApp keyWindow]) {
		return window;
	}
	return [NSApp mainWindow];
}

[[nodiscard]] QByteArray RandomBytes(int size) {
	auto result = QByteArray(size, Qt::Uninitialized);
	const auto pointer = reinterpret_cast<uint8_t*>(result.data());
	return (SecRandomCopyBytes(kSecRandomDefault, size, pointer) == 0)
		? result
		: QByteArray();
}

[[nodiscard]] QByteArray Sha256(const QByteArray &data) {
	const auto digest = openssl::Sha256(bytes::make_span(data));
	return QByteArray(
		reinterpret_cast<const char*>(digest.data()),
		digest.size());
}

[[nodiscard]] QByteArray SerializeCredential(const Credential &credential) {
	auto object = QJsonObject();
	object["id"] = QString::fromUtf8(credential.id.toBase64());
	object["rpId"] = credential.rpId;
	object["userHandle"] = QString::fromUtf8(credential.userHandle.toBase64());
	object["name"] = credential.name;
	object["displayName"] = credential.displayName;
	object["server"] = credential.testServer ? "test" : "production";
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

[[nodiscard]] std::optional<Credential> ParseCredential(
		const QByteArray &serialized) {
	const auto document = QJsonDocument::fromJson(serialized);
	if (!document.isObject()) {
		return std::nullopt;
	}
	const auto object = document.object();
	auto result = Credential();
	result.id = QByteArray::fromBase64(object["id"].toString().toUtf8());
	result.rpId = object["rpId"].toString();
	result.userHandle = QByteArray::fromBase64(
		object["userHandle"].toString().toUtf8());
	result.name = object["name"].toString();
	result.displayName = object["displayName"].toString();
	result.testServer = (object["server"].toString() == u"test"_q);
	if (result.id.isEmpty() || result.rpId.isEmpty()) {
		return std::nullopt;
	}
	return result;
}

// COSE_Key labels: 1 kty EC2, 3 alg ES256, -1 crv P-256, -2 X, -3 Y.
[[nodiscard]] QByteArray CosePublicKey(const QByteArray &x963) {
	auto result = QByteArray();
	{
		auto writer = QCborStreamWriter(&result);
		writer.startMap(5);
		writer.append(1);
		writer.append(2);
		writer.append(3);
		writer.append(kAlgorithmEs256);
		writer.append(-1);
		writer.append(1);
		writer.append(-2);
		writer.append(x963.mid(1, kCoordinateSize));
		writer.append(-3);
		writer.append(x963.mid(1 + kCoordinateSize, kCoordinateSize));
		writer.endMap();
	}
	return result;
}

// rpIdHash, flags, signature counter kept at zero, attested data.
[[nodiscard]] QByteArray AuthenticatorData(
		const QString &rpId,
		int flags,
		const QByteArray &attested) {
	auto result = Sha256(rpId.toUtf8());
	result.append(char(flags));
	result.append(4, char(0));
	result.append(attested);
	return result;
}

[[nodiscard]] QByteArray AttestedCredentialData(
		const QByteArray &credentialId,
		const QByteArray &cosePublicKey) {
	auto result = QByteArray(kAaguidSize, char(0));
	result.append(char((credentialId.size() >> 8) & 0xFF));
	result.append(char(credentialId.size() & 0xFF));
	result.append(credentialId);
	result.append(cosePublicKey);
	return result;
}

[[nodiscard]] QByteArray AttestationObject(const QByteArray &authData) {
	auto result = QByteArray();
	{
		auto writer = QCborStreamWriter(&result);
		writer.startMap(3);
		writer.append(QLatin1StringView("fmt"));
		writer.append(QLatin1StringView("none"));
		writer.append(QLatin1StringView("attStmt"));
		writer.startMap(0);
		writer.endMap();
		writer.append(QLatin1StringView("authData"));
		writer.append(authData);
		writer.endMap();
	}
	return result;
}

[[nodiscard]] NSMutableDictionary *LookupQuery() {
	auto *result = [NSMutableDictionary dictionary];
	result[(id)kSecClass] = (id)kSecClassKey;
	result[(id)kSecAttrKeyType] = (id)kSecAttrKeyTypeECSECPrimeRandom;
	result[(id)kSecAttrKeyClass] = (id)kSecAttrKeyClassPrivate;
	result[(id)kSecAttrSynchronizable] = @NO;
	return result;
}

// Ours are the private keys whose application tag parses as our metadata.
[[nodiscard]] std::vector<StoredCredential> LoadCredentials(
		const QString &rpId,
		std::optional<bool> testServer) {
	auto result = std::vector<StoredCredential>();
	auto *query = LookupQuery();
	query[(id)kSecMatchLimit] = (id)kSecMatchLimitAll;
	query[(id)kSecReturnAttributes] = @YES;

	CFTypeRef found = nullptr;
	if (SecItemCopyMatching((CFDictionaryRef)query, &found) != errSecSuccess) {
		return result;
	}
	const auto guard = gsl::finally([&] { CFRelease(found); });
	for (NSDictionary *item in (NSArray*)found) {
		NSData *tag = item[(id)kSecAttrApplicationTag];
		if (!tag) {
			continue;
		}
		const auto serialized = QByteArray::fromNSData(tag);
		const auto parsed = ParseCredential(serialized);
		if (!parsed
			|| (!rpId.isEmpty() && parsed->rpId != rpId)
			|| (testServer && parsed->testServer != *testServer)) {
			continue;
		}
		result.push_back({ .data = *parsed, .tag = serialized });
	}
	return result;
}

[[nodiscard]] SecKeyRef LookupKey(const QByteArray &tag) {
	auto *query = LookupQuery();
	query[(id)kSecAttrApplicationTag] = tag.toNSData();
	query[(id)kSecMatchLimit] = (id)kSecMatchLimitOne;
	query[(id)kSecReturnRef] = @YES;

	CFTypeRef found = nullptr;
	if (SecItemCopyMatching((CFDictionaryRef)query, &found) != errSecSuccess) {
		return nullptr;
	}
	return (SecKeyRef)found;
}

// Each option is more portable than the previous one, all stay on this Mac.
enum class Storage {
	SecureEnclave,
	ProtectedKeychain,
	Keychain,
};

[[nodiscard]] SecKeyRef CreateKey(
		const QByteArray &tag,
		NSString *label,
		Storage storage) {
	auto *privateAttributes = [NSMutableDictionary dictionary];
	privateAttributes[(id)kSecAttrIsPermanent] = @YES;
	privateAttributes[(id)kSecAttrLabel] = label;
	privateAttributes[(id)kSecAttrApplicationTag] = tag.toNSData();
	privateAttributes[(id)kSecAttrSynchronizable] = @NO;

	auto *attributes = [NSMutableDictionary dictionary];
	attributes[(id)kSecAttrKeyType] = (id)kSecAttrKeyTypeECSECPrimeRandom;
	attributes[(id)kSecAttrKeySizeInBits] = @256;
	attributes[(id)kSecPrivateKeyAttrs] = privateAttributes;

	auto access = (SecAccessControlRef)nullptr;
	const auto accessGuard = gsl::finally([&] {
		if (access) {
			CFRelease(access);
		}
	});
	if (storage == Storage::SecureEnclave) {
		auto accessError = (CFErrorRef)nullptr;
		access = SecAccessControlCreateWithFlags(
			kCFAllocatorDefault,
			kSecAttrAccessibleWhenUnlockedThisDeviceOnly,
			kSecAccessControlPrivateKeyUsage,
			&accessError);
		if (accessError) {
			CFRelease(accessError);
		}
		if (!access) {
			return nullptr;
		}
		attributes[(id)kSecAttrTokenID] = (id)kSecAttrTokenIDSecureEnclave;
		privateAttributes[(id)kSecAttrAccessControl] = (id)access;
	} else if (storage == Storage::ProtectedKeychain) {
		privateAttributes[(id)kSecAttrAccessible]
			= (id)kSecAttrAccessibleWhenUnlockedThisDeviceOnly;
	}

	auto error = (CFErrorRef)nullptr;
	const auto result = SecKeyCreateRandomKey(
		(CFDictionaryRef)attributes,
		&error);
	if (error) {
		NSLog(@"Passkey key creation failed: %@", (NSError*)error);
		CFRelease(error);
	}
	return result;
}

[[nodiscard]] SecKeyRef CreateKey(const QByteArray &tag, NSString *label) {
	const auto options = {
		Storage::SecureEnclave,
		Storage::ProtectedKeychain,
		Storage::Keychain,
	};
	for (const auto storage : options) {
		if (const auto result = CreateKey(tag, label, storage)) {
			return result;
		}
	}
	return nullptr;
}

[[nodiscard]] QByteArray ExportPublicKey(SecKeyRef privateKey) {
	const auto publicKey = SecKeyCopyPublicKey(privateKey);
	if (!publicKey) {
		return QByteArray();
	}
	const auto keyGuard = gsl::finally([&] { CFRelease(publicKey); });

	auto error = (CFErrorRef)nullptr;
	const auto exported = SecKeyCopyExternalRepresentation(publicKey, &error);
	if (error) {
		CFRelease(error);
	}
	if (!exported) {
		return QByteArray();
	}
	const auto exportedGuard = gsl::finally([&] { CFRelease(exported); });

	const auto result = QByteArray::fromNSData((NSData*)exported);
	return (result.size() == kPublicKeySize) ? result : QByteArray();
}

[[nodiscard]] QByteArray SignData(SecKeyRef key, const QByteArray &data) {
	auto error = (CFErrorRef)nullptr;
	const auto signature = SecKeyCreateSignature(
		key,
		kSecKeyAlgorithmECDSASignatureMessageX962SHA256,
		(CFDataRef)data.toNSData(),
		&error);
	if (error) {
		NSLog(@"Passkey signing failed: %@", (NSError*)error);
		CFRelease(error);
	}
	if (!signature) {
		return QByteArray();
	}
	const auto guard = gsl::finally([&] { CFRelease(signature); });
	return QByteArray::fromNSData((NSData*)signature);
}

void DeleteKey(const QByteArray &tag) {
	auto *query = LookupQuery();
	query[(id)kSecAttrApplicationTag] = tag.toNSData();
	SecItemDelete((CFDictionaryRef)query);
}

void RequestUserVerification(
		const QString &reason,
		Fn<void(bool confirmed, bool cancelled)> done) {
	// Touched inside the block so the copy retains it until the reply.
	auto *context = [[LAContext alloc] init];
	[context
		evaluatePolicy:LAPolicyDeviceOwnerAuthentication
		localizedReason:reason.toNSString()
		reply:^(BOOL success, NSError *error) {
			[context invalidate];
			const auto code = error ? error.code : 0;
			const auto cancelled = (code == LAErrorUserCancel)
				|| (code == LAErrorSystemCancel)
				|| (code == LAErrorAppCancel);
			crl::on_main([=] { done(success, cancelled); });
		}];
	[context release];
}

[[nodiscard]] QString CredentialTitle(const Credential &credential) {
	const auto name = credential.displayName.isEmpty()
		? credential.name
		: credential.displayName;
	if (name.isEmpty()) {
		return tr::lng_settings_passkey_unknown(tr::now);
	} else if (credential.name.isEmpty() || credential.name == name) {
		return name;
	}
	return name + u" ("_q + credential.name + ')';
}

void ChooseCredential(
		const std::vector<StoredCredential> &list,
		Fn<void(int index)> done) {
	Expects(!list.empty());

	if (list.size() == 1) {
		done(0);
		return;
	}
	auto *popup = [[NSPopUpButton alloc]
		initWithFrame:NSZeroRect
		pullsDown:NO];
	for (const auto &entry : list) {
		[popup addItemWithTitle:CredentialTitle(entry.data).toNSString()];
	}
	[popup sizeToFit];

	auto *alert = [[NSAlert alloc] init];
	alert.messageText
		= tr::lng_fork_passkeys_choose_title(tr::now).toNSString();
	alert.informativeText
		= tr::lng_fork_passkeys_choose_about(tr::now).toNSString();
	alert.accessoryView = popup;
	[alert addButtonWithTitle:tr::lng_continue(tr::now).toNSString()];
	[alert addButtonWithTitle:tr::lng_cancel(tr::now).toNSString()];
	[popup release];

	const auto handler = ^(NSModalResponse response) {
		const auto index = (response == NSAlertFirstButtonReturn)
			? int([popup indexOfSelectedItem])
			: -1;
		crl::on_main([=] { done(index); });
		[alert release];
	};
	if (NSWindow *anchor = ResolveAnchorWindow()) {
		[alert beginSheetModalForWindow:anchor completionHandler:handler];
	} else {
		handler([alert runModal]);
	}
}

[[nodiscard]] bool SupportsEs256(
		const std::vector<Data::Passkey::CredentialParameter> &params) {
	if (params.empty()) {
		return true;
	}
	for (const auto &param : params) {
		if (param.alg == kAlgorithmEs256) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] RegisterResult CreateCredential(
		const QString &rpId,
		const QByteArray &clientDataJSON,
		const Credential &credential) {
	const auto tag = SerializeCredential(credential);
	const auto key = CreateKey(tag, ItemLabel(credential));
	if (!key) {
		return { .error = Error::Other };
	}
	const auto keyGuard = gsl::finally([&] { CFRelease(key); });

	const auto exported = ExportPublicKey(key);
	if (exported.isEmpty()) {
		DeleteKey(tag);
		return { .error = Error::Other };
	}
	const auto attested = AttestedCredentialData(
		credential.id,
		CosePublicKey(exported));
	const auto authData = AuthenticatorData(
		rpId,
		kFlagUserPresent | kFlagUserVerified | kFlagAttestedData,
		attested);
	return {
		.credentialId = credential.id,
		.attestationObject = AttestationObject(authData),
		.clientDataJSON = clientDataJSON,
		.success = true,
	};
}

[[nodiscard]] LoginResult AssertCredential(
		const QByteArray &clientDataJSON,
		const StoredCredential &stored) {
	const auto key = LookupKey(stored.tag);
	if (!key) {
		return { .error = Error::Other };
	}
	const auto keyGuard = gsl::finally([&] { CFRelease(key); });

	const auto authData = AuthenticatorData(
		stored.data.rpId,
		kFlagUserPresent | kFlagUserVerified,
		QByteArray());
	const auto signature = SignData(
		key,
		authData + Sha256(clientDataJSON));
	if (signature.isEmpty()) {
		return { .error = Error::Other };
	}
	return {
		.clientDataJSON = clientDataJSON,
		.credentialId = stored.data.id,
		.authenticatorData = authData,
		.signature = signature,
		.userHandle = stored.data.userHandle,
	};
}

} // namespace

bool IsSupported() {
	return true;
}

bool HasKeys(bool testServer) {
	return !LoadCredentials(QString(), testServer).empty();
}

void RegisterKey(
		const Data::Passkey::RegisterData &data,
		bool testServer,
		Fn<void(RegisterResult result)> callback) {
	if (data.rp.id.isEmpty()
		|| data.user.id.isEmpty()
		|| data.challenge.isEmpty()
		|| !SupportsEs256(data.pubKeyCredParams)) {
		callback({ .error = Error::Other });
		return;
	}
	const auto credential = Credential{
		.id = RandomBytes(kCredentialIdSize),
		.rpId = data.rp.id,
		.userHandle = data.user.id,
		.name = data.user.name,
		.displayName = data.user.displayName,
		.testServer = testServer,
	};
	if (credential.id.isEmpty()) {
		callback({ .error = Error::Other });
		return;
	}
	const auto clientDataJSON = QByteArray::fromStdString(
		Data::Passkey::SerializeClientDataCreate(data.challenge));
	const auto rpId = data.rp.id;
	RequestUserVerification(
		tr::lng_fork_passkeys_create_reason(tr::now),
		[=](bool confirmed, bool cancelled) {
			if (!confirmed) {
				callback({
					.error = cancelled ? Error::Cancelled : Error::Other,
				});
				return;
			}
			crl::async([=] {
				@autoreleasepool {
					const auto result = CreateCredential(
						rpId,
						clientDataJSON,
						credential);
					crl::on_main([=] { callback(result); });
				}
			});
		});
}

void Login(
		const Data::Passkey::LoginData &data,
		bool testServer,
		Fn<void(LoginResult result)> callback) {
	if (data.rpId.isEmpty() || data.challenge.isEmpty()) {
		callback({ .error = Error::Other });
		return;
	}
	auto list = LoadCredentials(data.rpId, testServer);
	if (!data.allowCredentials.empty()) {
		auto allowed = std::vector<StoredCredential>();
		for (const auto &entry : list) {
			for (const auto &credential : data.allowCredentials) {
				if (credential.id == entry.data.id) {
					allowed.push_back(entry);
					break;
				}
			}
		}
		list = std::move(allowed);
	}
	if (list.empty()) {
		callback({ .error = Error::Other });
		return;
	}
	const auto clientDataJSON = QByteArray::fromStdString(
		Data::Passkey::SerializeClientDataGet(data.challenge));
	ChooseCredential(list, [=](int index) {
		if (index < 0 || index >= int(list.size())) {
			callback({ .error = Error::Cancelled });
			return;
		}
		const auto stored = list[index];
		RequestUserVerification(
			tr::lng_fork_passkeys_login_reason(tr::now),
			[=](bool confirmed, bool cancelled) {
				if (!confirmed) {
					callback({
						.error = cancelled ? Error::Cancelled : Error::Other,
					});
					return;
				}
				crl::async([=] {
					@autoreleasepool {
						const auto result = AssertCredential(
							clientDataJSON,
							stored);
						crl::on_main([=] { callback(result); });
					}
				});
			});
	});
}

void RemoveKey(const QString &credentialId) {
	auto normalized = credentialId;
	normalized.replace('-', '+').replace('_', '/');
	const auto id = QByteArray::fromBase64(normalized.toUtf8());
	if (id.isEmpty()) {
		return;
	}
	for (const auto &entry : LoadCredentials(QString(), std::nullopt)) {
		if (entry.data.id == id) {
			DeleteKey(entry.tag);
		}
	}
}

} // namespace Platform::WebAuthn::Local
