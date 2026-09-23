#!/usr/bin/env bash
set -euo pipefail
umask 022

Usage() {
  cat <<'USAGE'
Usage: Telegram/build/mac_fast_install_forkgram.sh [options]

Builds Forkgram Release and fast-installs it into an already packaged
/Applications/Forkgram.app by replacing only Contents/MacOS/Forkgram.

Options:
  --skip-build       Install the current out/Release executable without building.
  --copy-resources   Also copy Resources and Info.plist from out/Release.
  --deep-sign        Re-sign nested code too; verification is always deep/strict.
  --verify-user USER Required non-owner user with a logged-in graphical session.
  --app PATH         Installed app path. Default: /Applications/Forkgram.app.
  --source PATH      Source app path. Default: out/Release/Forkgram.app.
  --help             Show this help.
USAGE
}

Error() {
  echo "error: $*" >&2
  exit 1
}

NeedCommand() {
  command -v "$1" >/dev/null 2>&1 || Error "$1 not found"
}

RewriteExecutableDeps() {
  local app="$1"
  local binary="$2"
  local local_prefix="$repo_root/out/macos-local/prefix"
  local changes=()
  local dep
  while IFS= read -r dep; do
    local target
    local bundled
    if [[ "$dep" == *".framework/"* ]]; then
      local rel="${dep#*/lib/}"
      target="@executable_path/../Frameworks/$rel"
      bundled="$app/Contents/Frameworks/$rel"
    else
      local base
      base="$(basename "$dep")"
      target="@executable_path/../Frameworks/$base"
      bundled="$app/Contents/Frameworks/$base"
    fi
    [[ -e "$bundled" ]] || Error "missing bundled dependency for $dep at $bundled; run full packaging first"
    changes+=(-change "$dep" "$target")
  done < <(otool -L "$binary" \
    | awk -v local_prefix="$local_prefix" \
      '($1 ~ "^/" && ($1 ~ "^/opt/homebrew" || index($1, local_prefix) == 1)) { print $1 }')
  if [[ ${#changes[@]} -gt 0 ]]; then
    install_name_tool "${changes[@]}" "$binary"
  fi
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
source_app="$repo_root/out/Release/Forkgram.app"
installed_app="/Applications/Forkgram.app"
build=1
copy_resources=0
deep_sign=0
verify_user=""
bundle_tool="$script_dir/mac_forkgram_bundle.py"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-build)
      build=0
      shift
      ;;
    --copy-resources)
      copy_resources=1
      shift
      ;;
    --deep-sign)
      deep_sign=1
      shift
      ;;
    --verify-user)
      [[ $# -ge 2 ]] || Error "--verify-user requires a user"
      verify_user="$2"
      shift 2
      ;;
    --app)
      [[ $# -ge 2 ]] || Error "--app requires a path"
      installed_app="$2"
      shift 2
      ;;
    --source)
      [[ $# -ge 2 ]] || Error "--source requires a path"
      source_app="$2"
      shift 2
      ;;
    --help|-h)
      Usage
      exit 0
      ;;
    *)
      Error "unknown option: $1"
      ;;
  esac
done

NeedCommand cmake
NeedCommand codesign
NeedCommand install_name_tool
NeedCommand otool
NeedCommand shasum
NeedCommand python3
[[ -n "$verify_user" ]] || Error "--verify-user is required for access and GUI verification"

cd "$repo_root"

if [[ "$build" == 1 ]]; then
  cmake --build out --config Release --target Forkgram
fi

source_app="$(cd "$(dirname "$source_app")" && pwd)/$(basename "$source_app")"
installed_app="$(cd "$(dirname "$installed_app")" && pwd)/$(basename "$installed_app")"
source_binary="$source_app/Contents/MacOS/Forkgram"
installed_binary="$installed_app/Contents/MacOS/Forkgram"
source_info="$source_app/Contents/Info.plist"
installed_info="$installed_app/Contents/Info.plist"

[[ -x "$source_binary" ]] || Error "source executable not found: $source_binary"
[[ -d "$installed_app/Contents/Frameworks" ]] || Error "$installed_app is not a packaged app; run the full macOS packaging/install flow first"
[[ -d "$installed_app/Contents/MacOS" ]] || Error "installed app has no Contents/MacOS directory: $installed_app"
python3 "$bundle_tool" validate "$source_app"
python3 "$bundle_tool" validate "$installed_app"

if pgrep -x Forkgram >/dev/null 2>&1; then
  Error "Forkgram is running. Quit it before replacing $installed_binary"
fi

install -m 755 "$source_binary" "$installed_binary"
RewriteExecutableDeps "$installed_app" "$installed_binary"

if [[ "$copy_resources" == 1 ]]; then
  [[ -d "$source_app/Contents/Resources" ]] || Error "source resources not found: $source_app/Contents/Resources"
  [[ -f "$source_info" ]] || Error "source Info.plist not found: $source_info"
  ditto "$source_app/Contents/Resources" "$installed_app/Contents/Resources"
  install -m 644 "$source_info" "$installed_info"
fi

if [[ "$deep_sign" == 1 ]]; then
  python3 "$bundle_tool" finalize "$installed_app" --deep-sign
else
  python3 "$bundle_tool" finalize "$installed_app"
fi

leftovers="$(otool -L "$installed_binary" \
  | awk -v local_prefix="$repo_root/out/macos-local/prefix" \
    '($1 ~ "^/" && ($1 ~ "^/opt/homebrew" || index($1, local_prefix) == 1)) { print $1 }')"
[[ -z "$leftovers" ]] || Error "installed executable still has absolute local deps:
$leftovers"
python3 "$bundle_tool" verify-install "$installed_app" --user "$verify_user"

echo "Source executable SHA-256:"
shasum -a 256 "$source_binary"
echo "Installed executable SHA-256:"
shasum -a 256 "$installed_binary"
