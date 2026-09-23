# Shared macOS Forkgram installation

`/Applications/Forkgram.app` is shared by local macOS users. Each user keeps
their own Telegram login and settings. Never change permissions on user homes,
Application Support, `tdata`, or portable profiles to fix an application bundle.

All packaging and installation shells must start with `set -euo pipefail` and
`umask 022`. A copy can preserve restrictive source modes despite the umask.
After **all** copies, library updates, `install_name_tool` changes and stripping,
and **before** final signing, run:

```bash
python3 Telegram/build/mac_forkgram_bundle.py finalize "$APP" --deep-sign
```

`APP` must identify the actual prepared `Forkgram.app`, for example
`$PWD/out/Release/Forkgram.app`. The helper validates its name, plist, executable,
and contents, rejects external symlinks, hard links and embedded profiles, then
runs `chmod -R a+rX "$APP"`. This grants read access and directory traversal,
preserves executable files, and adds no write permissions. It covers every
library and resource without a list of versions or filenames. It then signs and
requires `codesign --verify --deep --strict` to succeed. Do not copy or modify
bundle contents afterward without repeating finalization.

## Build and full packaging

Follow the dependency deployment and install-name repair steps in the
[macOS build skill](../../.agents/skills/tdesktop-macos-build/SKILL.md).
Use Release builds on this machine. Run finalization after `macdeployqt` and
again after any manual plugin or library replacement. A DMG staging copy must
also be finalized before creating the image. CI uses this same helper.

## Installation and reinstallation

Choose a local user other than the owner of the bundle, and have that user log
in to a graphical macOS session. On this machine the verification user is
`wcard`, while the installed bundle is owned by `wavecut`.

For a full install of an already packaged Release bundle:

```bash
bash Telegram/build/mac_install_forkgram.sh --verify-user wcard
```

This retains an existing bundle in a printed `/Applications/Forkgram-backup.*`
directory, copies the complete packaged app, normalizes permissions, signs and
verifies it, then checks it in the other user's session. Keep the backup until
verification succeeds. If installation fails, leave the backup intact; do not
report deployment complete. The scripts never terminate running applications;
quit Forkgram in all user sessions immediately before installation, after the
Release build has finished.

For code-only updates with the existing dependencies:

```bash
Telegram/build/mac_fast_install_forkgram.sh --verify-user wcard
```

Use `--skip-build` after a successful Release build, `--copy-resources` when
resources or version metadata changed, and `--deep-sign` when nested signatures
need refreshing. Deep **verification is mandatory even without `--deep-sign`**.
Dependency changes require full packaging and installation.

## Required post-install checks

Both installers fail unless the selected non-owner user can read all bundle
files, traverse its directories, execute its executables, and verify the full
signature. The checker also opens the exact app in that user's GUI session and
requires its process and a normal window to remain present for ten seconds.
An accepted `open` request or a running process alone does not pass this check.

Cross-user verification uses non-interactive `sudo -u USER`. If that access is
unavailable, the installer exits unsuccessfully with the installed bundle left
in place. Do not change sudo policy or user/profile permissions to bypass it.
Finish the same verification from the selected user's own Terminal:

```bash
cd /Users/Shared/src/github.com/iamwavecut/tdesktop
/usr/bin/python3 Telegram/build/mac_forkgram_bundle.py verify-install \
  /Applications/Forkgram.app --user wcard
```

This command is read-only apart from launching the app. Its failure is a failed
deployment check. It checks actual user access (including ACL restrictions),
public read/traverse/execute mode bits, the signature, the exact executable and
UID, and a stable GUI window. It does not log chat data or window titles.

For a packaging-only check without launching:

```bash
python3 Telegram/build/mac_forkgram_bundle.py verify /Applications/Forkgram.app
```

That command does not replace the required non-owner GUI verification.
