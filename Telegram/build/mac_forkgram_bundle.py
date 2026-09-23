#!/usr/bin/env python3
import argparse
import os
from pathlib import Path
import plistlib
import pwd
import stat
import subprocess
import sys
import time


def bundle_paths(app):
    app = Path(os.path.abspath(app))
    if app.name != "Forkgram.app" or app.is_symlink() or not app.is_dir():
        raise ValueError(f"expected a real Forkgram.app directory: {app}")
    app = app.resolve(strict=True)
    paths = [app]
    for parent, directories, files in os.walk(app, onerror=raise_error):
        for name in directories + files:
            path = Path(parent) / name
            if name == "tdata" or name.endswith("TelegramForcePortable"):
                raise ValueError(f"refusing to change profile permissions: {path}")
            target = path.resolve(strict=True)
            if not target.is_relative_to(app):
                raise ValueError(f"bundle symlink escapes the app: {path}")
            if not path.is_symlink():
                mode = path.stat()
                if not (stat.S_ISREG(mode.st_mode) or stat.S_ISDIR(mode.st_mode)):
                    raise ValueError(f"unsupported bundle entry: {path}")
                if stat.S_ISREG(mode.st_mode) and mode.st_nlink != 1:
                    raise ValueError(f"refusing to chmod a hard-linked file: {path}")
            paths.append(path)
    with (app / "Contents/Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    if info.get("CFBundleExecutable") != "Forkgram" or info.get("CFBundleName") != "Forkgram":
        raise ValueError(f"not a Forkgram bundle: {app}")
    if not (app / "Contents/MacOS/Forkgram").is_file():
        raise ValueError(f"Forkgram executable missing: {app}")
    return app, paths


def raise_error(error):
    raise error


def check_permissions(app, paths):
    for path in paths:
        mode = path.stat().st_mode
        required = 0o555 if stat.S_ISDIR(mode) or mode & 0o111 else 0o444
        if mode & required != required:
            raise ValueError(f"not readable/traversable by every user: {path}")
        access = os.R_OK | (os.X_OK if required == 0o555 else 0)
        if not os.access(path, access):
            raise ValueError(f"access denied for {pwd.getpwuid(os.geteuid()).pw_name}: {path}")
        if path.is_file():
            with path.open("rb") as stream:
                stream.read(1)
    if not os.access(app / "Contents/MacOS/Forkgram", os.X_OK):
        raise ValueError(f"Forkgram is not executable: {app}")


def verify(app, paths):
    check_permissions(app, paths)
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True)


def finalize(app, paths, deep_sign):
    subprocess.run(["chmod", "-R", "a+rX", str(app)], check=True)
    check_permissions(app, paths)
    if deep_sign:
        subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(app)], check=True)
    else:
        subprocess.run(["codesign", "--force", "--sign", "-", str(app / "Contents/MacOS/Forkgram")], check=True)
        subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True)
    verify(*bundle_paths(app))


WINDOW_PROBE = r'''
ObjC.import('AppKit');
ObjC.import('CoreGraphics');
function run(argv) {
    const apps = $.NSWorkspace.sharedWorkspace.runningApplications;
    const windows = ObjC.deepUnwrap(ObjC.castRefToObject(
        $.CGWindowListCopyWindowInfo($.kCGWindowListOptionAll, $.kCGNullWindowID)));
    for (let i = 0; i < apps.count; ++i) {
        const app = apps.objectAtIndex(i);
        if (app.bundleURL.isNil() || ObjC.unwrap(app.bundleURL.path) !== argv[0] || !app.isFinishedLaunching) continue;
        const pid = app.processIdentifier;
        if (windows.some(w => w.kCGWindowOwnerPID === pid && w.kCGWindowLayer === 0
                && w.kCGWindowBounds.Width > 0 && w.kCGWindowBounds.Height > 0)) return String(pid);
    }
    return '';
}
'''


def verify_install(app, paths, user):
    account = pwd.getpwnam(user)
    if account.pw_uid == 0 or account.pw_uid == app.stat().st_uid:
        raise ValueError("verification user must be a non-root user other than the bundle owner")
    if os.geteuid() != account.pw_uid:
        subprocess.run([
            "sudo", "-n", "-u", user, "/usr/bin/python3", str(Path(__file__).resolve()),
            "verify-install", str(app), "--user", user,
        ], check=True)
        return
    verify(app, paths)
    subprocess.run(["launchctl", "print", f"gui/{account.pw_uid}"], check=True, stdout=subprocess.DEVNULL)
    session = ["launchctl", "asuser", str(account.pw_uid)]
    subprocess.run(session + ["/usr/bin/open", "-a", str(app)], check=True, timeout=10)
    deadline = time.monotonic() + 30
    stable_pid = None
    stable_since = None
    while time.monotonic() < deadline:
        result = subprocess.run(session + ["/usr/bin/osascript", "-l", "JavaScript", "-e", WINDOW_PROBE, str(app)],
                                check=True, capture_output=True, text=True, timeout=5)
        pid = result.stdout.strip()
        if pid.isdigit():
            process = subprocess.run(["ps", "-p", pid, "-o", "uid=,comm="], capture_output=True, text=True, check=True)
            uid, executable = process.stdout.strip().split(None, 1)
            if int(uid) != account.pw_uid or executable != str(app / "Contents/MacOS/Forkgram"):
                raise ValueError("GUI probe returned a different user or executable")
            if stable_pid != pid:
                stable_pid, stable_since = pid, time.monotonic()
            elif time.monotonic() - stable_since >= 10:
                print(f"Verified {app}: readable, signature valid, GUI window present for {user}, PID {pid} stable for 10s")
                return
        else:
            stable_pid, stable_since = None, None
        time.sleep(1)
    raise ValueError(f"no stable Forkgram GUI window for {user} within 30s")


def main():
    os.umask(0o022)
    parser = argparse.ArgumentParser(description="Finalize and verify a shared macOS Forkgram bundle.")
    parser.add_argument("action", choices=["validate", "finalize", "verify", "verify-install"])
    parser.add_argument("app")
    parser.add_argument("--deep-sign", action="store_true")
    parser.add_argument("--user", help="non-owner local user with a logged-in graphical session")
    args = parser.parse_args()
    app, paths = bundle_paths(args.app)
    if args.action == "finalize":
        finalize(app, paths, args.deep_sign)
    elif args.action == "verify":
        verify(app, paths)
    elif args.action == "verify-install":
        if not args.user:
            parser.error("verify-install requires --user")
        verify_install(app, paths, args.user)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        sys.exit(f"error: {error}")
