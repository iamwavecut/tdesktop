/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/link_rewrite.h"

#include "base/basic_types.h"
#include "base/qt/qt_string_view.h"

#include <QtCore/QUrl>

#include <optional>

namespace Core {
namespace {

struct ParsedPrefix {
	QString host;
	QString path;
};

struct ParsedLink {
	int hostStart = 0;
	QString normalizedHost;
	QString authoritySuffix;
	QString path;
	QString tail;
};

[[nodiscard]] bool SupportedProtocol(QStringView protocol) {
	return protocol.isEmpty()
		|| (protocol.compare(u"http"_q, Qt::CaseInsensitive) == 0)
		|| (protocol.compare(u"https"_q, Qt::CaseInsensitive) == 0);
}

[[nodiscard]] QString NormalizeHost(QString value) {
	value = value.trimmed().toLower();
	if (value.isEmpty()
		|| value.contains(QChar(u':'))
		|| value.contains(QChar(u'?'))
		|| value.contains(QChar(u'#'))
		|| value.contains(QChar(u'@'))) {
		return QString();
	}
	const auto parsed = QUrl(u"https://"_q + value + u"/"_q);
	const auto host = parsed.host().toLower();
	return (parsed.isValid() && !host.isEmpty() && host == value)
		? host
		: QString();
}

[[nodiscard]] ParsedPrefix SplitPrefix(const QString &prefix) {
	const auto slash = prefix.indexOf('/');
	return {
		.host = (slash < 0) ? prefix : prefix.left(slash),
		.path = (slash < 0) ? QString() : prefix.mid(slash),
	};
}

[[nodiscard]] std::optional<ParsedLink> ParseLink(QStringView url) {
	auto hostStart = 0;
	if (const auto separator = url.indexOf(u"://"_q); separator > 0) {
		if (!SupportedProtocol(base::StringViewMid(url, 0, separator))) {
			return std::nullopt;
		}
		hostStart = separator + 3;
	}
	auto hostEnd = hostStart;
	while (hostEnd < url.size()) {
		const auto ch = url[hostEnd];
		if (ch == QChar(u'/')
			|| ch == QChar(u':')
			|| ch == QChar(u'?')
			|| ch == QChar(u'#')) {
			break;
		}
		++hostEnd;
	}
	if (hostEnd <= hostStart) {
		return std::nullopt;
	}
	const auto normalizedHost = NormalizeHost(
		base::StringViewMid(url, hostStart, hostEnd - hostStart).toString());
	if (normalizedHost.isEmpty()) {
		return std::nullopt;
	}
	auto pathStart = hostEnd;
	if (pathStart < url.size() && url[pathStart] == QChar(u':')) {
		while (pathStart < url.size()
			&& url[pathStart] != QChar(u'/')
			&& url[pathStart] != QChar(u'?')
			&& url[pathStart] != QChar(u'#')) {
			++pathStart;
		}
	}
	auto pathEnd = pathStart;
	if (pathStart < url.size() && url[pathStart] == QChar(u'/')) {
		while (pathEnd < url.size()
			&& url[pathEnd] != QChar(u'?')
			&& url[pathEnd] != QChar(u'#')) {
			++pathEnd;
		}
	}
	return ParsedLink{
		.hostStart = hostStart,
		.normalizedHost = normalizedHost,
		.authoritySuffix = base::StringViewMid(
			url,
			hostEnd,
			pathStart - hostEnd).toString(),
		.path = base::StringViewMid(
			url,
			pathStart,
			pathEnd - pathStart).toString(),
		.tail = base::StringViewMid(url, pathEnd).toString(),
	};
}

[[nodiscard]] bool MatchesPathPrefix(
		const QString &path,
		const QString &prefix) {
	return prefix.isEmpty()
		|| (path.startsWith(prefix)
			&& (path.size() == prefix.size()
				|| path[prefix.size()] == QChar(u'/')));
}

} // namespace

QString NormalizeLinkRewritePrefix(QString value) {
	value = value.trimmed();
	if (value.isEmpty()
		|| value.contains(u"://"_q)
		|| value.contains(QChar(u'?'))
		|| value.contains(QChar(u'#'))
		|| value.contains(QChar(u'@'))
		|| value.contains(QChar(u'\\'))) {
		return QString();
	}
	for (const auto ch : value) {
		if (ch.isSpace() || ch.category() == QChar::Other_Control) {
			return QString();
		}
	}
	const auto slash = value.indexOf('/');
	const auto host = NormalizeHost(
		(slash < 0) ? value : value.left(slash));
	if (host.isEmpty()) {
		return QString();
	}
	auto path = (slash < 0) ? QString() : value.mid(slash);
	while (path.endsWith(QChar(u'/'))) {
		path.chop(1);
	}
	return host + path;
}

const std::vector<LinkRewriteRule> &DefaultLinkRewriteRules() {
	static const auto result = std::vector<LinkRewriteRule>{
		{ u"x.com"_q, u"fixupx.com"_q },
		{ u"www.instagram.com"_q, u"eeinstagram.com"_q },
		{ u"instagram.com"_q, u"eeinstagram.com"_q },
		{ u"youtu.be"_q, u"koutu.be"_q },
		{ u"www.youtube.com"_q, u"koutu.be"_q },
		{ u"youtube.com"_q, u"koutu.be"_q },
	};
	return result;
}

QString RewriteLink(
		QString url,
		const std::vector<LinkRewriteRule> &rules) {
	const auto parsed = ParseLink(url);
	if (!parsed) {
		return url;
	}
	const auto selected = [&]() -> const LinkRewriteRule* {
		auto result = static_cast<const LinkRewriteRule*>(nullptr);
		auto length = 0;
		for (const auto &rule : rules) {
			const auto source = SplitPrefix(rule.sourceHost);
			if (source.host == parsed->normalizedHost
				&& MatchesPathPrefix(parsed->path, source.path)
				&& rule.sourceHost.size() > length) {
				result = &rule;
				length = rule.sourceHost.size();
			}
		}
		return result;
	}();
	if (!selected) {
		return url;
	}
	const auto source = SplitPrefix(selected->sourceHost);
	const auto target = SplitPrefix(selected->targetHost);
	const auto remainingPath = source.path.isEmpty()
		? parsed->path
		: parsed->path.mid(source.path.size());
	return url.left(parsed->hostStart)
		+ target.host
		+ parsed->authoritySuffix
		+ target.path
		+ remainingPath
		+ parsed->tail;
}

} // namespace Core
