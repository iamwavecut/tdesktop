---
name: tdesktop-macos-build
description: Build this Forkgram or Telegram Desktop repository on macOS using packaged Homebrew dependencies plus a local prefix for rnnoise, tde2e, and tg_owt. Use when asked to build this repo on macOS, to recreate the local build environment after a clean checkout, or to diagnose the specific Xcode 26, CMake 4, minizip, and test-API issues seen in this tree.
---

# tdesktop macOS build

Use this skill for this repository only.

It captures the packaged macOS path that was proven locally on:

- macOS 26.4.1
- Xcode 26.4.1
- CMake 4.3.1
- Homebrew Qt 6.11.0
- Homebrew `qtsvg` 6.11.0
- Homebrew `qtimageformats` 6.11.0
- Homebrew `openssl@3` 3.6.2
- Homebrew `ffmpeg` 8.1_1
- Homebrew `opus` 1.6.1
- Homebrew `libvpx` 1.16.0
- Homebrew `minizip` 1.3.2_1
- Homebrew `openal-soft` 1.25.1
- Homebrew `openh264` 2.6.0
- Homebrew `boost` 1.90.0_1
- Homebrew `abseil` 20260107.1
- Homebrew `ada-url` 3.4.4

The successful result was:

- configure in `out/`
- build with `cmake --build out --config Debug --target Forkgram`
- output app at `out/Debug/Forkgram.app`
- build with `cmake --build out --config Release --target Forkgram`
- package a portable ad-hoc-signed app at `out/Release/Forkgram.app`
- verify `codesign --verify --deep --strict --verbose=2 out/Release/Forkgram.app`
- verify there are no leftover absolute references to `/opt/homebrew` or `out/macos-local/prefix`
- for routine code-only iterations after a full packaged install exists, fast-install with `Telegram/build/mac_fast_install_forkgram.sh`
- install the packaged release app into `/Applications/Forkgram.app` once the running app is closed
- package a standalone unsigned DMG at `out/Release/Forkgram-macos-arm64.dmg`

## Quick rules

- Build from the repository root.
- Pull latest first if the user asked for it.
- Do not use `force` with `Telegram/configure.sh` once `out/macos-local` exists. `force` clears most of `out/` and will wipe the local dependency prefix.
- Use `-DTDESKTOP_API_TEST=ON` for a local non-deployment build unless the user provides real API credentials.
- On Xcode 26 SDKs, use `-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0` for app configure and for locally built dependencies. This is the key workaround for the obsolete desktop-capture APIs in `tg_owt`.
- If `/Applications/Forkgram.app` is running, do not replace the bundle in place. Close the app first, then copy the packaged release bundle into `/Applications`.
- Prefer the fast-install script for repeated local UI/code iterations when `/Applications/Forkgram.app` is already fully packaged and dependencies/resources/signing layout did not change.
- Use full packaging instead of fast install after dependency, Qt plugin, CMake/configure, `Info.plist`, entitlement, signing, or bundle-resource changes, or before handing off a final distributable build.

## Homebrew deps

Make sure these are available through Homebrew before configuring:

```bash
brew install pkg-config qt qtsvg qtimageformats openssl@3 ffmpeg opus libvpx minizip openal-soft openh264 boost abseil ada-url
```

If some are already installed, do not reinstall them unnecessarily.

`qtimageformats` matters on this tree because the default emoji sprites are `.webp` assets loaded through Qt image plugins. Without it, `macdeployqt` will not bundle `libqwebp.dylib`.

## Local prefix

Use a local prefix inside the repo for the Telegram-specific libraries:

```bash
export PREFIX="$PWD/out/macos-local/prefix"
export BUILD_ROOT="$PWD/out/macos-local/build"
mkdir -p "$PREFIX" "$BUILD_ROOT"
```

## rnnoise

Build `rnnoise` from the pinned Telegram commit and install it into the local prefix:

```bash
git clone https://github.com/desktop-app/rnnoise.git /tmp/tdesktop-macos-deps/rnnoise
git -C /tmp/tdesktop-macos-deps/rnnoise checkout d8ea2b0

cmake -S /tmp/tdesktop-macos-deps/rnnoise \
  -B "$BUILD_ROOT/rnnoise" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DCMAKE_INSTALL_PREFIX="$PREFIX"

cmake --build "$BUILD_ROOT/rnnoise" -j8
cmake --install "$BUILD_ROOT/rnnoise"
```

`tdesktop` expects `pkg-config` to find `rnnoise`, so provide a local `rnnoise.pc` if needed.

## tde2e

Build the pinned `td` checkout in `E2E_ONLY` mode and install it into the same prefix:

```bash
git clone https://github.com/tdlib/td.git /tmp/tdesktop-macos-deps/tde2e
git -C /tmp/tdesktop-macos-deps/tde2e checkout 51743df

cmake -S /tmp/tdesktop-macos-deps/tde2e \
  -B "$BUILD_ROOT/tde2e" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_PREFIX_PATH="/opt/homebrew;/opt/homebrew/opt/openssl@3;$PREFIX" \
  -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 \
  -DTD_E2E_ONLY=ON \
  -DTD_INSTALL_STATIC_LIBRARIES=ON \
  -DTD_INSTALL_SHARED_LIBRARIES=OFF \
  -DBUILD_TESTING=OFF

cmake --build "$BUILD_ROOT/tde2e" -j8
cmake --install "$BUILD_ROOT/tde2e"
```

## tg_owt

Build the pinned `tg_owt` checkout and install it into the same prefix:

```bash
git clone https://github.com/desktop-app/tg_owt.git /tmp/tdesktop-macos-deps/tg_owt
git -C /tmp/tdesktop-macos-deps/tg_owt checkout 89df288dd6ba5b2ec95b3c5eaf1e7e0c3a870fc4
git -C /tmp/tdesktop-macos-deps/tg_owt submodule update --init --recursive

env PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:/opt/homebrew/lib/pkgconfig:/opt/homebrew/share/pkgconfig" \
cmake -S /tmp/tdesktop-macos-deps/tg_owt \
  -B "$BUILD_ROOT/tg_owt" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_PREFIX_PATH="/opt/homebrew;/opt/homebrew/opt/openssl@3;$PREFIX" \
  -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 \
  -DCMAKE_CXX_STANDARD=20 \
  -DCMAKE_CXX_STANDARD_REQUIRED=ON \
  -DTG_OWT_BUILD_AUDIO_BACKENDS=OFF

cmake --build "$BUILD_ROOT/tg_owt" -j8
cmake --install "$BUILD_ROOT/tg_owt"
```

Why `14.0` matters:

- on Xcode 26 SDKs, `tg_owt` otherwise fails in `screen_capturer_mac.mm` and `desktop_frame_cgimage.mm`
- the old CoreGraphics capture APIs become hard-obsoleted when targeting macOS 15+

## Local fixes that were required

### 1. CMake 4 import-check dev errors

With CMake 4 and the repo's `-Werror=dev`, the installed `tde2e` and `tg_owt` export files can fail on uninitialized `_cmake_import_check_xcframework_for_*` variables.

Patch these installed files:

- `$PREFIX/lib/cmake/tde2e/tde2eStaticTargets.cmake`
- `$PREFIX/lib/cmake/tg_owt/tg_owtTargets.cmake`

Inside the `foreach(_cmake_target IN LISTS _cmake_import_check_targets)` loop, initialize the variable before the existing `if(...)`:

```cmake
if(NOT DEFINED "_cmake_import_check_xcframework_for_${_cmake_target}")
  set("_cmake_import_check_xcframework_for_${_cmake_target}" "")
endif()
```

Do not patch the repository for this. Patch the installed files in the local prefix.

### 2. Homebrew minizip header layout

Homebrew's `minizip` exposes headers under `include/minizip/`, but this tree includes `<unzip.h>`.

The proven workaround was to add:

```bash
-DCMAKE_C_FLAGS="-I/opt/homebrew/opt/minizip/include/minizip"
-DCMAKE_CXX_FLAGS="-I/opt/homebrew/opt/minizip/include/minizip"
-DCMAKE_OBJC_FLAGS="-I/opt/homebrew/opt/minizip/include/minizip"
-DCMAKE_OBJCXX_FLAGS="-I/opt/homebrew/opt/minizip/include/minizip"
```

to the app configure step.

### 3. Image viewer crash or blank overlay after a successful launch

If the app launches but crashes when opening an image, or the overlay opens without showing the image, and the stack points into `Media::View::OverlayWidget::RendererRhi::render(...)` through Metal or `QRhiWidget`, check QRhi shader availability before chasing plugins or dylibs.

One proven failure mode in this tree was missing precompiled `.qsb` shaders:

- `Telegram/cmake/qrhi_shaders.cmake` failed to find `qsb` in the Homebrew Qt layout
- configure completed with `QSB_EXECUTABLE-NOTFOUND`
- the app was built without the shader resources expected by `media_view_overlay_rhi.*`
- on macOS this showed up either as a Metal crash near `setRenderPipelineState:` or as an empty image overlay, depending on the runtime fallback path

The working fix is:

- make `Telegram/cmake/qrhi_shaders.cmake` resolve `qsb` from Homebrew Qt, including the `qtpaths` / `QT_HOST_LIBEXECS` path
- rerun configure after changing that CMake logic, otherwise the cached `QSB_EXECUTABLE-NOTFOUND` value in `out/` is reused
- at runtime, use `RendererRhi::Available()` to skip QRhi when the required shaders are unavailable
- when QRhi is unavailable for the media overlay, fall back to `Ui::GL::Backend::Raster`, not `OpenGL`

`platform/mac/text_recognition_mac.mm` is unrelated to this failure and should not be changed as a workaround.

If the packaged `Release/Forkgram.app` already contains `Contents/PlugIns/imageformats/libqwebp.dylib` and its bundled `libwebp*` dependencies, this issue is more likely shader/build-config related than packaging related.

## Configure the app

Run configure from the repository root:

```bash
env \
  OPENSSL_ROOT_DIR="/opt/homebrew/opt/openssl@3" \
  PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:/opt/homebrew/lib/pkgconfig:/opt/homebrew/share/pkgconfig:/opt/homebrew/opt/openal-soft/lib/pkgconfig" \
  ./Telegram/configure.sh \
  -DDESKTOP_APP_USE_PACKAGED=ON \
  -DDESKTOP_APP_DISABLE_CRASH_REPORTS=ON \
  -DTDESKTOP_API_TEST=ON \
  -DOpenAL_DIR=/opt/homebrew/opt/openal-soft/lib/cmake/OpenAL \
  -DCMAKE_PREFIX_PATH="$PREFIX;/opt/homebrew/opt/qt;/opt/homebrew;/opt/homebrew/opt/openssl@3;/opt/homebrew/opt/openal-soft" \
  -Dtde2e_DIR="$PREFIX/lib/cmake/tde2e" \
  -Dtg_owt_DIR="$PREFIX/lib/cmake/tg_owt" \
  -DCMAKE_C_FLAGS="-I/opt/homebrew/opt/minizip/include/minizip" \
  -DCMAKE_CXX_FLAGS="-I/opt/homebrew/opt/minizip/include/minizip" \
  -DCMAKE_OBJC_FLAGS="-I/opt/homebrew/opt/minizip/include/minizip" \
  -DCMAKE_OBJCXX_FLAGS="-I/opt/homebrew/opt/minizip/include/minizip" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  '-GNinja Multi-Config'
```

Expected result:

- configure succeeds
- build files are written to `out/`
- warnings about Qt private modules are acceptable here

## Build

Use the standard repo build command:

```bash
cmake --build out --config Debug --target Forkgram
```

Expected output location:

- `out/Debug/Forkgram.app`
- executable at `out/Debug/Forkgram.app/Contents/MacOS/Forkgram`

For a release app, use:

```bash
cmake --build out --config Release --target Forkgram
```

Expected output location:

- `out/Release/Forkgram.app`
- executable at `out/Release/Forkgram.app/Contents/MacOS/Forkgram`

## Fast local install for iteration

After one full packaged install exists in `/Applications/Forkgram.app`, routine C++/UI iterations do not need to rerun `macdeployqt`, copy all frameworks, or deep-sign the entire bundle. Use the local helper:

```bash
Telegram/build/mac_fast_install_forkgram.sh
```

What it does:

- builds `cmake --build out --config Release --target Forkgram`
- requires an existing packaged `/Applications/Forkgram.app`
- refuses to replace the app while `Forkgram` is running
- copies only `out/Release/Forkgram.app/Contents/MacOS/Forkgram`
- rewrites `/opt/homebrew` and `out/macos-local/prefix` executable deps to the already bundled `Contents/Frameworks` paths, preserving Qt framework paths
- signs the executable and the outer app bundle, verifies them with shallow `codesign --verify --strict --verbose=1`, checks the installed executable for leftover absolute local deps, and prints source/installed SHA-256 hashes for traceability. The hashes can differ because the installed executable is rewritten and signed.

Useful options:

```bash
Telegram/build/mac_fast_install_forkgram.sh --skip-build
Telegram/build/mac_fast_install_forkgram.sh --copy-resources
Telegram/build/mac_fast_install_forkgram.sh --deep-sign
```

Use `--copy-resources` when the Release build updated app resources that are not embedded in the executable. Use `--deep-sign` for a stronger but slower local check when time is less important. If the installed app is missing, does not contain bundled frameworks/plugins, or the helper reports missing bundled dependencies, run the full packaging/install flow below.

## Release packaging and DMG

The raw `Release/Forkgram.app` is not portable right after the build. It still links against Homebrew Qt and many Homebrew dylibs, so package it before handing it off.

### 1. Bundle frameworks, plugins, and third-party dylibs

Run `macdeployqt` from the repository root:

```bash
macdeployqt out/Release/Forkgram.app \
  -verbose=1 \
  -always-overwrite \
  -libpath=/opt/homebrew/lib \
  -libpath=/opt/homebrew/opt/openal-soft/lib \
  -libpath=/opt/homebrew/opt/openssl@3/lib \
  -libpath=/opt/homebrew/opt/ffmpeg/lib \
  -libpath=/opt/homebrew/opt/minizip/lib \
  -libpath=/opt/homebrew/opt/boost/lib \
  -libpath=/opt/homebrew/opt/opus/lib \
  -libpath=/opt/homebrew/opt/openh264/lib \
  -libpath=/opt/homebrew/opt/libvpx/lib \
  -libpath=/opt/homebrew/opt/jpeg-turbo/lib
```

Notes:

- this may take several minutes and stay mostly quiet while it runs `install_name_tool`
- it copies non-Qt dylibs too, not just Qt frameworks
- it writes `out/Release/Forkgram.app/Contents/Resources/qt.conf`
- it deploys plugins into `out/Release/Forkgram.app/Contents/PlugIns`
- by default it ad-hoc signs the bundle

After a good deploy, make sure `out/Release/Forkgram.app/Contents/PlugIns/imageformats/libqwebp.dylib` exists. If it does not, `qtimageformats` is missing locally or `macdeployqt` did not see it.

If you later replace only `Contents/MacOS/Forkgram` inside an already deployed app in `/Applications`, do not rewrite Qt framework dependencies to a bare basename like `@executable_path/../Frameworks/QtNetwork`. That breaks dyld lookup. Qt frameworks must keep the framework path, for example:

- `@executable_path/../Frameworks/QtCore.framework/QtCore`
- `@executable_path/../Frameworks/QtGui.framework/QtGui`
- `@executable_path/../Frameworks/QtNetwork.framework/QtNetwork`

### 2. Fix the residual install IDs left by `macdeployqt`

One local run still left a few bundled libraries and frameworks with absolute Homebrew install IDs.

Patch them and then re-sign the app:

```bash
install_name_tool -id @executable_path/../Frameworks/libbrotlicommon.1.dylib \
  out/Release/Forkgram.app/Contents/Frameworks/libbrotlicommon.1.dylib

install_name_tool -id @executable_path/../Frameworks/libjxl_cms.0.11.dylib \
  out/Release/Forkgram.app/Contents/Frameworks/libjxl_cms.0.11.dylib

install_name_tool -id @executable_path/../Frameworks/QtDBus.framework/Versions/A/QtDBus \
  out/Release/Forkgram.app/Contents/Frameworks/QtDBus.framework/Versions/A/QtDBus

install_name_tool -id @executable_path/../Frameworks/QtWidgets.framework/Versions/A/QtWidgets \
  out/Release/Forkgram.app/Contents/Frameworks/QtWidgets.framework/Versions/A/QtWidgets

install_name_tool -id @executable_path/../Frameworks/QtNetwork.framework/Versions/A/QtNetwork \
  out/Release/Forkgram.app/Contents/Frameworks/QtNetwork.framework/Versions/A/QtNetwork

install_name_tool -id @executable_path/../Frameworks/QtSvg.framework/Versions/A/QtSvg \
  out/Release/Forkgram.app/Contents/Frameworks/QtSvg.framework/Versions/A/QtSvg

codesign --force --deep --sign - out/Release/Forkgram.app
```

### 2a. If `libqwebp.dylib` is missing, bundle it manually

This fallback was needed locally when `qtimageformats` was installed after the first packaging attempt.

```bash
cp -f /opt/homebrew/share/qt/plugins/imageformats/libqwebp.dylib \
  out/Release/Forkgram.app/Contents/PlugIns/imageformats/

cp -f /opt/homebrew/opt/webp/lib/libwebpdemux.2.dylib \
  out/Release/Forkgram.app/Contents/Frameworks/
cp -f /opt/homebrew/opt/webp/lib/libwebpmux.3.dylib \
  out/Release/Forkgram.app/Contents/Frameworks/
cp -f /opt/homebrew/opt/webp/lib/libwebp.7.dylib \
  out/Release/Forkgram.app/Contents/Frameworks/

install_name_tool -id @executable_path/../Frameworks/libwebpdemux.2.dylib \
  out/Release/Forkgram.app/Contents/Frameworks/libwebpdemux.2.dylib
install_name_tool -id @executable_path/../Frameworks/libwebpmux.3.dylib \
  out/Release/Forkgram.app/Contents/Frameworks/libwebpmux.3.dylib
install_name_tool -id @executable_path/../Frameworks/libwebp.7.dylib \
  out/Release/Forkgram.app/Contents/Frameworks/libwebp.7.dylib

install_name_tool -change @rpath/libwebp.7.dylib \
  @executable_path/../Frameworks/libwebp.7.dylib \
  out/Release/Forkgram.app/Contents/Frameworks/libwebpdemux.2.dylib
install_name_tool -change @rpath/libsharpyuv.0.dylib \
  @executable_path/../Frameworks/libsharpyuv.0.dylib \
  out/Release/Forkgram.app/Contents/Frameworks/libwebpdemux.2.dylib

install_name_tool -change @rpath/libwebp.7.dylib \
  @executable_path/../Frameworks/libwebp.7.dylib \
  out/Release/Forkgram.app/Contents/Frameworks/libwebpmux.3.dylib
install_name_tool -change @rpath/libsharpyuv.0.dylib \
  @executable_path/../Frameworks/libsharpyuv.0.dylib \
  out/Release/Forkgram.app/Contents/Frameworks/libwebpmux.3.dylib

install_name_tool -change @rpath/libsharpyuv.0.dylib \
  @executable_path/../Frameworks/libsharpyuv.0.dylib \
  out/Release/Forkgram.app/Contents/Frameworks/libwebp.7.dylib

install_name_tool -change /opt/homebrew/opt/qtbase/lib/QtGui.framework/Versions/A/QtGui \
  @executable_path/../Frameworks/QtGui.framework/Versions/A/QtGui \
  out/Release/Forkgram.app/Contents/PlugIns/imageformats/libqwebp.dylib
install_name_tool -change /opt/homebrew/opt/qtbase/lib/QtCore.framework/Versions/A/QtCore \
  @executable_path/../Frameworks/QtCore.framework/Versions/A/QtCore \
  out/Release/Forkgram.app/Contents/PlugIns/imageformats/libqwebp.dylib
install_name_tool -change /opt/homebrew/opt/webp/lib/libwebpdemux.2.dylib \
  @executable_path/../Frameworks/libwebpdemux.2.dylib \
  out/Release/Forkgram.app/Contents/PlugIns/imageformats/libqwebp.dylib
install_name_tool -change /opt/homebrew/opt/webp/lib/libwebpmux.3.dylib \
  @executable_path/../Frameworks/libwebpmux.3.dylib \
  out/Release/Forkgram.app/Contents/PlugIns/imageformats/libqwebp.dylib
install_name_tool -change /opt/homebrew/opt/webp/lib/libwebp.7.dylib \
  @executable_path/../Frameworks/libwebp.7.dylib \
  out/Release/Forkgram.app/Contents/PlugIns/imageformats/libqwebp.dylib
install_name_tool -change /opt/homebrew/opt/webp/lib/libsharpyuv.0.dylib \
  @executable_path/../Frameworks/libsharpyuv.0.dylib \
  out/Release/Forkgram.app/Contents/PlugIns/imageformats/libqwebp.dylib

codesign --force --deep --sign - out/Release/Forkgram.app
```

### 3. Verify the packaged app

Verify the final ad-hoc signature:

```bash
codesign --verify --deep --strict --verbose=2 out/Release/Forkgram.app
```

The proven result was:

```text
out/Release/Forkgram.app: valid on disk
out/Release/Forkgram.app: satisfies its Designated Requirement
```

Then scan every Mach-O in the bundle for leftover absolute references to Homebrew or the local prefix:

```bash
find out/Release/Forkgram.app -type f | while read -r f; do
  if file "$f" | grep -q "Mach-O"; then
    refs=$(otool -L "$f" 2>/dev/null | grep -E "^[[:space:]]+(/opt/homebrew|$PWD/out/macos-local/prefix)" || true)
    if [ -n "$refs" ]; then
      printf 'FILE: %s\n%s\n' "$f" "$refs"
    fi
  fi
done
```

Expected result:

- no output

### 3a. Install the packaged app into `/Applications`

Do this only if user asks you to, after the packaged `out/Release/Forkgram.app` passes the verification steps above.

If `/Applications/Forkgram.app` is running, close it first. Then replace the installed bundle from the repository root:

```bash
rm -rf /Applications/Forkgram.app
ditto out/Release/Forkgram.app /Applications/Forkgram.app
codesign --verify --deep --strict --verbose=2 /Applications/Forkgram.app
```

Expected result:

- `/Applications/Forkgram.app` exists
- the installed bundle passes `codesign --verify`
- the next launch uses the freshly packaged `Release` build

If that scan still prints only `out/Release/Forkgram.app/Contents/MacOS/Forkgram`, rewrite each leftover `/opt/homebrew/...` dependency to the matching `@executable_path/../Frameworks/...` path, then re-sign again. This happened locally after a fresh `Release` relink.

`spctl -a -t exec -vv out/Release/Forkgram.app` was still rejected locally, which is expected for an ad-hoc-signed, non-notarized app. This skill produces a local distributable DMG, not a notarized production build.

### 4. Build the DMG

Create a simple install DMG with the app and an `Applications` symlink:

```bash
rm -rf out/Release/dmg-root
mkdir -p out/Release/dmg-root
cp -R out/Release/Forkgram.app out/Release/dmg-root/Forkgram.app
ln -s /Applications out/Release/dmg-root/Applications

hdiutil create \
  -volname Forkgram \
  -srcfolder out/Release/dmg-root \
  -ov \
  -format UDZO \
  out/Release/Forkgram-macos-arm64.dmg
```

Expected result:

- DMG at `out/Release/Forkgram-macos-arm64.dmg`

Optional mount check:

```bash
mountpoint=$(hdiutil attach -nobrowse -readonly out/Release/Forkgram-macos-arm64.dmg | awk -F "\t" 'END {print $3}')
ls -la "$mountpoint"
hdiutil detach "$mountpoint"
```

Expected contents:

- `Forkgram.app`
- `Applications -> /Applications`

## Verification

After the full build succeeds, run the same build command again:

```bash
cmake --build out --config Debug --target Forkgram
```

The expected no-op result is:

```text
ninja: no work to do.
```

## Warnings seen during the successful build

These warnings appeared but did not block the successful build on the local macOS 26 machine:

- Homebrew dylibs "built for newer version 26.0"
- locally built static archives built for newer macOS than the `14.0` link target

If you want those warnings gone, rebuild every locally built dependency with the same deployment target and avoid linking against Homebrew bottles built for a newer SDK. They were not required to get a successful local Debug app build.

## Startup crash note

One local packaged app crashed immediately on launch in `Ui::Emoji::UniversalImages::generate()` because `_sprites` was empty while the code still tried to generate the emoji cache. The root cause was a missing Qt WebP image plugin. On this branch, `Telegram/lib_ui/ui/emoji_config.cpp` was patched so the app no longer crashes when the default universal emoji sprites fail to load, but you should still bundle `libqwebp.dylib` so emoji render correctly.

## Fast failure guide

If configure fails with missing API credentials:

- use `-DTDESKTOP_API_TEST=ON` for a local test build

If configure fails on `_cmake_import_check_xcframework_for_...`:

- patch the installed `tde2e` and `tg_owt` export files in `$PREFIX/lib/cmake/...`

If `tg_owt` fails on desktop-capture APIs:

- rebuild `tg_owt` with `-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0`

If app compile fails on `unzip.h`:

- reconfigure with the explicit `minizip/include/minizip` compiler flags above

If the final build command says `ninja: no work to do.` and `out/Debug/Forkgram.app` exists:

- the macOS build path is established
