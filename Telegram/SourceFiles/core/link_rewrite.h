/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

#include <vector>

namespace Core {

struct LinkRewriteRule {
	QString sourceHost;
	QString targetHost;

	friend inline auto operator<=>(
		const LinkRewriteRule &,
		const LinkRewriteRule &) = default;
	friend inline bool operator==(
		const LinkRewriteRule &,
		const LinkRewriteRule &) = default;
};

[[nodiscard]] QString NormalizeLinkRewritePrefix(QString value);
[[nodiscard]] const std::vector<LinkRewriteRule> &DefaultLinkRewriteRules();
[[nodiscard]] QString RewriteLink(
	QString url,
	const std::vector<LinkRewriteRule> &rules);

} // namespace Core
