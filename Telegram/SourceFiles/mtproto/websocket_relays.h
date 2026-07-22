/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_map.h"
#include "base/timer.h"

#include <QtCore/QMutex>

class QNetworkAccessManager;
class QNetworkReply;

namespace MTP {

extern const char kOptionWebSocketTransport[];
extern const char kOptionWebSocketDomain[];

[[nodiscard]] QString DecodeRelayDomain(const QString &encoded);

class WebSocketRelays final {
public:
	[[nodiscard]] static WebSocketRelays &Instance();

	void applyConfig();
	void refresh(bool force = false);

	[[nodiscard]] bool enabled() const;
	[[nodiscard]] bool isUserDomain(const QString &domain) const;
	[[nodiscard]] QString domainForDc(int bareDcId);
	void markResult(const QString &domain, bool success);

private:
	struct RelayState {
		int strikes = 0;
		crl::time cooldownUntil = 0;
	};

	WebSocketRelays();

	void rebuildPool();
	[[nodiscard]] bool suppressedLocked();
	void fetchNext();
	void fetchFailed();
	bool applyFetched(const QByteArray &content);

	mutable QMutex _mutex;
	bool _enabled = false;
	QString _userDomain;
	std::vector<QString> _pool;
	base::flat_map<QString, RelayState> _states;
	base::flat_map<int, QString> _dcPick;
	int _rotateIndex = 0;
	int _consecutiveFailures = 0;
	crl::time _suppressedUntil = 0;

	std::unique_ptr<QNetworkAccessManager> _manager;
	QNetworkReply *_reply = nullptr;
	base::Timer _timeoutTimer;
	QByteArray _fetchedList;
	crl::time _fetchedAt = 0;
	crl::time _lastFetchStartedAt = 0;
	int _mirrorIndex = 0;
	bool _fetching = false;

};

} // namespace MTP
