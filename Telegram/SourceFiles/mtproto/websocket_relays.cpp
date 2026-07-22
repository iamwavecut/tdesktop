/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/websocket_relays.h"

#include "base/const_string.h"
#include "base/invoke_queued.h"
#include "base/options.h"

#include <QtCore/QCoreApplication>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace MTP {
namespace {

base::options::toggle OptionWebSocketTransport({
	.id = kOptionWebSocketTransport,
	.name = "Connect through a WebSocket relay",
	.description = "Tunnel MTProto inside ordinary HTTPS to a"
		" Cloudflare-fronted relay, which can get through where a direct"
		" connection is blocked.",
});

base::options::option<QString> OptionWebSocketDomain({
	.id = kOptionWebSocketDomain,
	.name = "WebSocket relay domain",
	.description = "A Cloudflare worker of your own. Empty means the"
		" automatic public relay list.",
});

constexpr auto kCooldownBase = crl::time(45 * 1000);
constexpr auto kCooldownMax = crl::time(300 * 1000);
constexpr auto kFailuresBeforeSuppress = 6;
constexpr auto kSuppressDuration = crl::time(60 * 1000);
constexpr auto kFetchTimeout = crl::time(8 * 1000);
constexpr auto kFetchTtl = crl::time(12 * 60 * 60 * 1000);
constexpr auto kForcedFetchInterval = crl::time(5 * 60 * 1000);

const auto kMirrors = std::array{
	"https://raw.githubusercontent.com/Flowseal/tg-ws-proxy/main/.github/cfproxy-domains.txt"_cs,
	"https://gcore.jsdelivr.net/gh/Flowseal/tg-ws-proxy@main/.github/cfproxy-domains.txt"_cs,
	"https://fastly.jsdelivr.net/gh/Flowseal/tg-ws-proxy@main/.github/cfproxy-domains.txt"_cs,
	"https://cdn.jsdelivr.net/gh/Flowseal/tg-ws-proxy@main/.github/cfproxy-domains.txt"_cs,
	"https://raw.githack.com/Flowseal/tg-ws-proxy/main/.github/cfproxy-domains.txt"_cs,
};

const auto kFallbackDomains = std::array{
	"virkgj.com"_cs,
	"vmmzovy.com"_cs,
	"mkuosckvso.com"_cs,
	"zaewayzmplad.com"_cs,
	"twdmbzcm.com"_cs,
	"awzwsldi.com"_cs,
	"clngqrflngqin.com"_cs,
	"tjacxbqtj.com"_cs,
	"bxaxtxmrw.com"_cs,
	"dmohrsgmohcrwb.com"_cs,
	"vwbmtmoi.com"_cs,
	"khgrre.com"_cs,
	"ulihssf.com"_cs,
	"tmhqsdqmfpmk.com"_cs,
	"xwuwoqbm.com"_cs,
	"orgcnunpj.com"_cs,
	"zhkuldz.com"_cs,
	"zypoljnslxa.com"_cs,
	"efabnxaowuzs.com"_cs,
	"zaftuzsftqdq.com"_cs,
};

[[nodiscard]] std::vector<QString> DecodeList(const QByteArray &content) {
	auto result = std::vector<QString>();
	const auto lines = QString::fromUtf8(content).split(
		'\n',
		Qt::SkipEmptyParts);
	for (const auto &line : lines) {
		const auto decoded = DecodeRelayDomain(line);
		if (!decoded.isEmpty()) {
			result.push_back(decoded);
		}
	}
	return result;
}

} // namespace

const char kOptionWebSocketTransport[] = "websocket-transport";
const char kOptionWebSocketDomain[] = "websocket-relay-domain";

QString DecodeRelayDomain(const QString &encoded) {
	const auto value = encoded.trimmed().toLower();
	if (value.endsWith(u".co.uk"_q)) {
		return value;
	} else if (!value.endsWith(u".com"_q)) {
		return QString();
	}
	const auto body = value.chopped(4);
	auto letters = 0;
	for (const auto ch : body) {
		if (ch >= u'a' && ch <= u'z') {
			++letters;
		}
	}
	const auto shift = letters % 26;
	auto result = QString();
	result.reserve(body.size() + 6);
	for (const auto ch : body) {
		if (ch >= u'a' && ch <= u'z') {
			const auto index = ch.unicode() - u'a';
			result.append(QChar(u'a' + ((index - shift + 26) % 26)));
		} else {
			result.append(ch);
		}
	}
	return result.isEmpty() ? QString() : (result + u".co.uk"_q);
}

WebSocketRelays::WebSocketRelays()
// Instance() may well be first reached from a session thread, while the
// timer is only ever used from the main one, so bind it explicitly.
: _timeoutTimer(QCoreApplication::instance()->thread()) {
}

WebSocketRelays &WebSocketRelays::Instance() {
	static auto result = WebSocketRelays();
	return result;
}

void WebSocketRelays::applyConfig() {
	const auto enabled = OptionWebSocketTransport.value();
	const auto userDomain = OptionWebSocketDomain.value().trimmed().toLower();

	auto locker = QMutexLocker(&_mutex);
	const auto changed = (_enabled != enabled) || (_userDomain != userDomain);
	_enabled = enabled;
	_userDomain = userDomain;
	if (changed) {
		rebuildPool();

		_consecutiveFailures = 0;
		_suppressedUntil = 0;
	}
}

void WebSocketRelays::rebuildPool() {
	auto pool = DecodeList(_fetchedList);
	for (const auto &entry : kFallbackDomains) {
		const auto decoded = DecodeRelayDomain(entry.utf16());
		if (!decoded.isEmpty() && !ranges::contains(pool, decoded)) {
			pool.push_back(decoded);
		}
	}
	_pool = std::move(pool);
	_dcPick.clear();
	_states.clear();
	_rotateIndex = 0;
}

bool WebSocketRelays::enabled() const {
	auto locker = QMutexLocker(&_mutex);
	return _enabled;
}

bool WebSocketRelays::isUserDomain(const QString &domain) const {
	auto locker = QMutexLocker(&_mutex);
	return !domain.isEmpty() && (domain == _userDomain);
}

bool WebSocketRelays::suppressedLocked() {
	if (!_suppressedUntil) {
		return false;
	} else if (crl::now() < _suppressedUntil) {
		return true;
	}
	_suppressedUntil = 0;
	return false;
}

QString WebSocketRelays::domainForDc(int bareDcId) {
	auto locker = QMutexLocker(&_mutex);
	if (!_enabled || suppressedLocked()) {
		return QString();
	} else if (!_userDomain.isEmpty()) {
		return _userDomain;
	} else if (_pool.empty()) {
		return QString();
	}
	const auto now = crl::now();
	const auto cooled = [&](const QString &domain) {
		const auto i = _states.find(domain);
		return (i != _states.end()) && (now < i->second.cooldownUntil);
	};
	const auto i = _dcPick.find(bareDcId);
	if (i != _dcPick.end() && !cooled(i->second)) {
		return i->second;
	}
	for (auto j = 0; j != int(_pool.size()); ++j) {
		const auto &domain = _pool[(_rotateIndex + j) % _pool.size()];
		if (!cooled(domain)) {
			_rotateIndex = (_rotateIndex + j + 1) % _pool.size();
			_dcPick[bareDcId] = domain;
			return domain;
		}
	}
	auto best = _pool.front();
	auto bestUntil = std::numeric_limits<crl::time>::max();
	for (const auto &domain : _pool) {
		const auto k = _states.find(domain);
		const auto until = (k != _states.end()) ? k->second.cooldownUntil : 0;
		if (until < bestUntil) {
			bestUntil = until;
			best = domain;
		}
	}
	_dcPick[bareDcId] = best;
	return best;
}

void WebSocketRelays::markResult(const QString &domain, bool success) {
	auto locker = QMutexLocker(&_mutex);
	if (success) {
		_consecutiveFailures = 0;
		_suppressedUntil = 0;
		if (!domain.isEmpty()) {
			_states.remove(domain);
		}
		return;
	}
	if (!domain.isEmpty()) {
		auto &state = _states[domain];
		++state.strikes;
		const auto backoff = kCooldownBase * (crl::time(1) << std::min(
			state.strikes - 1,
			8));
		state.cooldownUntil = crl::now() + std::min(backoff, kCooldownMax);
		for (auto i = _dcPick.begin(); i != _dcPick.end();) {
			if (i->second == domain) {
				i = _dcPick.erase(i);
			} else {
				++i;
			}
		}
	}
	if (++_consecutiveFailures >= kFailuresBeforeSuppress) {
		_suppressedUntil = crl::now() + kSuppressDuration;
		if (_userDomain.isEmpty()) {
			// The whole pool keeps failing, it has likely rotated.
			InvokeQueued(QCoreApplication::instance(), [] {
				Instance().refresh(true);
			});
		}
	}
}

void WebSocketRelays::refresh(bool force) {
	if (_fetching) {
		return;
	} else if (force) {
		if (_lastFetchStartedAt
			&& (crl::now() - _lastFetchStartedAt) < kForcedFetchInterval) {
			return;
		}
	} else if (!_fetchedList.isEmpty()
		&& (crl::now() - _fetchedAt) < kFetchTtl) {
		return;
	}
	{
		auto locker = QMutexLocker(&_mutex);
		if (!_enabled) {
			return;
		}
	}
	if (!_manager) {
		_manager = std::make_unique<QNetworkAccessManager>();
	}
	_fetching = true;
	_lastFetchStartedAt = crl::now();
	_mirrorIndex = 0;
	_timeoutTimer.setCallback([=] { fetchFailed(); });
	fetchNext();
}

void WebSocketRelays::fetchNext() {
	if (_mirrorIndex >= int(kMirrors.size())) {
		_fetching = false;
		_timeoutTimer.cancel();
		return;
	}
	const auto url = QUrl(kMirrors[_mirrorIndex++].utf16());
	auto request = QNetworkRequest(url);
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	_reply = _manager->get(request);
	const auto reply = _reply;
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		if (_reply != reply) {
			return;
		}
		_reply = nullptr;
		_timeoutTimer.cancel();
		const auto content = (reply->error() == QNetworkReply::NoError)
			? reply->readAll()
			: QByteArray();
		reply->deleteLater();
		if (!applyFetched(content)) {
			fetchNext();
		} else {
			_fetching = false;
		}
	});
	_timeoutTimer.callOnce(kFetchTimeout);
}

void WebSocketRelays::fetchFailed() {
	if (const auto reply = base::take(_reply)) {
		reply->abort();
		reply->deleteLater();
	}
	fetchNext();
}

bool WebSocketRelays::applyFetched(const QByteArray &content) {
	if (content.isEmpty() || DecodeList(content).empty()) {
		return false;
	}
	_fetchedList = content;
	_fetchedAt = crl::now();

	auto locker = QMutexLocker(&_mutex);
	const auto was = _pool;
	rebuildPool();
	if (_pool != was) {
		// An unchanged list must not lift the suppression, or the client
		// would hammer the same dead pool right away.
		_consecutiveFailures = 0;
		_suppressedUntil = 0;
	}
	return true;
}

} // namespace MTP
