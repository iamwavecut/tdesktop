#!/usr/bin/env python3
import importlib.util
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import unittest


TOOL = Path(__file__).with_name("mac_forkgram_bundle.py")
spec = importlib.util.spec_from_file_location("bundle", TOOL)
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


@unittest.skipUnless(os.uname().sysname == "Darwin", "macOS codesign required")
class BundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="forkgram-bundle-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.app = self.root / "Forkgram.app"
        self.binary = self.app / "Contents/MacOS/Forkgram"
        self.binary.parent.mkdir(parents=True)
        shutil.copyfile("/usr/bin/true", self.binary)
        self.binary.chmod(0o700)
        self.library = self.app / "Contents/Frameworks/libfuture.999.dylib"
        self.library.parent.mkdir(mode=0o700)
        subprocess.run(["cc", "-dynamiclib", "-x", "c", "-", "-o", str(self.library)],
                       input="int fixture(void) { return 42; }", text=True, check=True, capture_output=True)
        self.library.chmod(0o600)
        with (self.app / "Contents/Info.plist").open("wb") as stream:
            plistlib.dump({"CFBundleExecutable": "Forkgram", "CFBundleName": "Forkgram",
                          "CFBundleIdentifier": "test.forkgram.permissions", "CFBundlePackageType": "APPL"}, stream)

    def run_tool(self, *args):
        return subprocess.run(["/usr/bin/python3", str(TOOL), *args], capture_output=True, text=True,
                              umask=0o077)

    def test_finalize_repairs_copied_modes_under_restrictive_umask(self):
        before = self.run_tool("verify", str(self.app))
        self.assertNotEqual(before.returncode, 0)
        link = self.library.parent / "libfuture.dylib"
        link.symlink_to(self.library.name)
        result = self.run_tool("finalize", str(self.app), "--deep-sign")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.library.stat().st_mode & 0o777, 0o644)
        self.assertEqual(self.library.parent.stat().st_mode & 0o777, 0o755)
        self.assertEqual(self.binary.stat().st_mode & 0o777, 0o755)
        for path in self.app.rglob("*"):
            self.assertEqual(path.stat().st_mode & 0o022, 0, str(path))
        self.assertEqual(self.run_tool("verify", str(self.app)).returncode, 0)

    def test_shallow_resign_still_rejects_invalid_nested_signature(self):
        self.assertEqual(self.run_tool("finalize", str(self.app), "--deep-sign").returncode, 0)
        subprocess.run(["codesign", "--remove-signature", str(self.library)], check=True, capture_output=True)
        result = self.run_tool("finalize", str(self.app))
        self.assertNotEqual(result.returncode, 0)

    def test_external_symlink_never_changes_target_permissions(self):
        target = self.root / "private-profile"
        target.mkdir(mode=0o700)
        (self.app / "escaped").symlink_to(target, target_is_directory=True)
        result = self.run_tool("finalize", str(self.app), "--deep-sign")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(target.stat().st_mode & 0o777, 0o700)
        self.assertEqual(self.library.stat().st_mode & 0o777, 0o600)

    def test_chmod_failure_aborts_finalization(self):
        subprocess.run(["chflags", "uchg", str(self.library)], check=True)
        try:
            result = self.run_tool("finalize", str(self.app), "--deep-sign")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(self.library.stat().st_mode & 0o777, 0o600)
            self.assertFalse((self.app / "Contents/_CodeSignature").exists())
        finally:
            subprocess.run(["chflags", "nouchg", str(self.library)], check=True)

    def test_rejects_wrong_root_symlink_hardlink_and_profile(self):
        for kind in ["wrong-root", "root-symlink", "hardlink", "profile"]:
            with self.subTest(kind=kind):
                app = self.app
                if kind == "wrong-root":
                    app = self.root
                    extra = None
                elif kind == "root-symlink":
                    folder = self.root / "alias"
                    folder.mkdir()
                    extra = folder / "Forkgram.app"
                    extra.symlink_to(self.app, target_is_directory=True)
                    app = extra
                elif kind == "hardlink":
                    extra = self.app / "linked"
                    os.link(self.library, extra)
                else:
                    extra = self.app / "tdata"
                    extra.mkdir()
                result = self.run_tool("finalize", str(app), "--deep-sign")
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.library.stat().st_mode & 0o777, 0o600)
                if extra is not None:
                    if extra.is_dir() and not extra.is_symlink():
                        extra.rmdir()
                    else:
                        extra.unlink()

    def test_owner_cannot_satisfy_cross_user_verification(self):
        user = bundle.pwd.getpwuid(os.getuid()).pw_name
        result = self.run_tool("verify-install", str(self.app), "--user", user)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("other than the bundle owner", result.stderr)

    def test_acl_denial_is_not_hidden_by_public_mode_bits(self):
        self.assertEqual(self.run_tool("finalize", str(self.app), "--deep-sign").returncode, 0)
        user = bundle.pwd.getpwuid(os.getuid()).pw_name
        subprocess.run(["chmod", "+a", f"user:{user} deny read", str(self.library)], check=True)
        try:
            self.assertEqual(self.library.stat().st_mode & 0o777, 0o644)
            self.assertNotEqual(self.run_tool("verify", str(self.app)).returncode, 0)
        finally:
            subprocess.run(["chmod", "-N", str(self.library)], check=True)

    def test_fast_install_normalizes_then_fails_if_cross_user_check_fails(self):
        source = self.root / "source/Forkgram.app"
        source.parent.mkdir()
        shutil.copytree(self.app, source)
        self.assertEqual(self.run_tool("finalize", str(self.app), "--deep-sign").returncode, 0)
        self.library.chmod(0o600)
        commands = self.root / "commands"
        commands.mkdir()
        pgrep = commands / "pgrep"
        pgrep.write_text("#!/bin/sh\nexit 1\n")
        pgrep.chmod(0o755)
        env = dict(os.environ, PATH=str(commands) + ":" + os.environ["PATH"])
        user = bundle.pwd.getpwuid(os.getuid()).pw_name
        installer = TOOL.with_name("mac_fast_install_forkgram.sh")
        result = subprocess.run(["bash", str(installer), "--skip-build", "--source", str(source),
                                 "--app", str(self.app), "--verify-user", user],
                                env=env, capture_output=True, text=True, umask=0o077)
        self.assertEqual(self.library.stat().st_mode & 0o777, 0o644)
        self.assertEqual(self.run_tool("verify", str(self.app)).returncode, 0)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("other than the bundle owner", result.stderr)

    def test_full_install_normalizes_copies_and_keeps_previous_bundle(self):
        source = self.root / "source/Forkgram.app"
        source.parent.mkdir()
        shutil.copytree(self.app, source)
        commands = self.root / "commands"
        commands.mkdir()
        pgrep = commands / "pgrep"
        pgrep.write_text("#!/bin/sh\nexit 1\n")
        pgrep.chmod(0o755)
        env = dict(os.environ, PATH=str(commands) + ":" + os.environ["PATH"])
        user = bundle.pwd.getpwuid(os.getuid()).pw_name
        installer = TOOL.with_name("mac_install_forkgram.sh")
        result = subprocess.run(["bash", str(installer), "--source", str(source),
                                 "--app", str(self.app), "--verify-user", user],
                                env=env, capture_output=True, text=True, umask=0o077)
        self.assertEqual(self.library.stat().st_mode & 0o777, 0o644)
        self.assertEqual(self.run_tool("verify", str(self.app)).returncode, 0)
        backups = list(self.root.glob("Forkgram-backup.*/Forkgram.app"))
        self.assertEqual(len(backups), 1)
        self.assertEqual((backups[0] / "Contents/Frameworks/libfuture.999.dylib").stat().st_mode & 0o777, 0o600)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("other than the bundle owner", result.stderr)


if __name__ == "__main__":
    unittest.main()
