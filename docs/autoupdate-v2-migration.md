# Auto-update: migrating Forkgram from v1 to v2

Upstream's v2 update package is a signed envelope carrying an explicit target
and channel, signed by keys from a manifest that an offline root key signs and
the binary embeds. The fork has shipped the v2 client stack for a while —
`core/update_verify.cpp`, `core/update_keys.cpp`, `Telegram/Resources/update/`
— but carried Telegram's anchors and packed every release as v1 with the RSA
check commented out, installing whatever the feed held, unverified.

## The bridge release

7.1.5 is unpacked by 7.1.4 clients, which embed the old anchors, so a v2 7.1.5
would reach nobody. It therefore carries the new trust and the new feed
address but is itself published the old way.

| | 7.1.4 and older | 7.1.5 | 7.2.5 and later |
|---|---|---|---|
| reads feed | `frkgrmfeed2` | `frkgrmfeed` | `frkgrmfeed` |
| embedded anchors | Telegram's | Forkgram's | Forkgram's |
| published as | v1 | v1, to `frkgrmfeed2` | v2, to `frkgrmfeed` |

Afterwards `frkgrmfeed2` is frozen at 7.1.5 and never posted to again, and the
7.1.5 packages in `frkgrmfiles` must never be deleted — a feed entry is a
`<version>:<files channel>#<message id>` reference into that channel, and
those messages carry any client, however old, onto the new feed.

`frkgrmfeed` is seeded in the same release with the same JSON and the same
uploads. A client that resolves an empty channel just fails its update check,
so without the seed every 7.1.5 install sits without an updater until the flip.

## Trust anchors

`Telegram/build/forkgram_update_keys.sh` regenerates the three embedded files.
Run it, commit the result, and the next build carries the new trust.

    Telegram/build/forkgram_update_keys.sh --root forkgram-root-private.pem \
        --manifest-version 2 fg-2026a=forkgram-release-2026a.pem

The root key signs only the manifest and never enters CI. The release key is
what Packer signs with, and since macOS and Linux pack locally it has to exist
both in the `UPDATE_LOCAL_KEY` secret and in `~/TBuild/DesktopPrivate/` on
every machine running a deploy script. Rotation is the same command with the
new id appended, so both keys are accepted while builds cut over; retiring one
drops it from the command line, burning one adds it to `revoked`. No expiry is
written: the client ignores the manifest's and hard-fails on a key's, so a
date could only ever cut users off.

## The flip

Lands in 7.2.5, and is done for the three producers that actually release
today: macOS from `~/TBuild/deploy.sh`, Windows and Linux from CI. The legacy
`~/TBuild/linux_deploy.sh` and the Windows VM batch scripts were left on v1
and the old feed, so they no longer work as a fallback.

Add to every Packer invocation:

    -channel stable -keys-loc <repo>/Telegram/Resources/update \
    -local-key <release key pem> -local-key-id fg-2026a

| producer | where | Packer call | done |
|---|---|---|---|
| macOS | `~/TBuild/deploy.sh` (local) | `thin_and_package()` | yes |
| Windows CI | `.github/workflows/win_release.yml` | `$packerArgs` | yes |
| Linux CI | `.github/workflows/linux_release.yml` | the `./Packer` step | yes |
| Linux | `~/TBuild/linux_deploy.sh` (local) | the docker `bash -c` line | no |
| Windows VM | `C:\TBuild\0python_tg\*.bat` | not in this repository | no |

CI reads the release key from the `UPDATE_LOCAL_KEY` secret and writes it into
`DesktopPrivate/` alongside the packer keys; the local scripts read it from
there directly.

Packer then writes `td-update-{win,mac,linux}-{x86,x64,arm}-<AppVersion>`
instead of `tx64upd` / `tupdate` / `tlinuxupd` / `tmacupd` / `tarmacupd`, so
every name follows it: `UPD_PREFIX` and `$update` in both workflows,
`pack_arm` / `pack_x86` in `deploy.sh`, the `mv` in `linux_deploy.sh`,
`NAME_TO_PLATFORM` in `.github/scripts/publish_telegram.py`, and `PLATFORMS`
in `feed_bot.py` (both copies).

The feed channel is the `TG_FEED_CHANNEL` secret in CI and the `FEED_CHANNEL`
constant in `feed_bot.py`; both move to `frkgrmfeed` only after 7.1.5 has
shipped to the old one. `TG_FILES_CHANNEL` stays — both feeds share one files
channel. A legacy `winxp` key rides along in the feed, pointing at 7.0.7; the
publishers carry unknown keys through untouched and it is left that way.

Rehearse first: `win_release.yml`'s `telegram: testing` input writes the
`testing` key of the feed, which no client reads without an explicit
`Updater::test()`. Packer re-runs the client's own verification on what it
produced, so a bad key fails the build rather than the update.

## Beta releases

`BetaChannel` in `Telegram/build/version`, set by `set_version.py 7.2.6.beta`,
is the only switch. The packer signs for the `beta` channel and appends
`-beta` to the package name, `deploy.sh` marks the GitHub release a
prerelease, both workflows compute `UPD_CHANNEL` and `UPD_SUFFIX`, and the
publishers
write only the feed's `beta` keys, carrying `stable` over from the previous
post. A stable release writes both, so beta users move on once it is newer.
`publish_telegram.py` takes the channel from the artifact name rather than
the environment, so it cannot disagree with what the packer produced.

The git tag stays a plain `vX.Y.Z`, the way upstream tags its own betas: a
beta burns its version number and the next stable takes the following one, so
the two never collide. Nothing downstream can read the channel off the tag,
so the channel announcement asks the release whether it is a prerelease.

The client needs nothing: the opt-in is in Settings → Advanced, and
`ChannelPolicyAllows` installs a beta on a stable build only when it is set.
The manifest covers both channels with `fg-2026a`, so betas need no key of
their own — the v1 beta key was never replaced and is still upstream's, which
is why this was impossible before v2.
