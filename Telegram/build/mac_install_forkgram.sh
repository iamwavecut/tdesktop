#!/usr/bin/env bash
set -euo pipefail
umask 022

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
bundle_tool="$script_dir/mac_forkgram_bundle.py"
source_app="$repo_root/out/Release/Forkgram.app"
installed_app="/Applications/Forkgram.app"
verify_user=""

Error() {
  echo "error: $*" >&2
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --verify-user|--source|--app)
      [[ $# -ge 2 && -n "$2" ]] || Error "$1 requires a value"
      case "$1" in
        --verify-user) verify_user="$2" ;;
        --source) source_app="$2" ;;
        --app) installed_app="$2" ;;
      esac
      shift 2
      ;;
    --help|-h)
      echo "Usage: $0 --verify-user USER [--source Forkgram.app] [--app Forkgram.app]"
      exit 0
      ;;
    *) Error "unknown option: $1" ;;
  esac
done
[[ -n "$verify_user" ]] || Error "--verify-user is required for access and GUI verification"
installed_app="$(cd "$(dirname "$installed_app")" && pwd -P)/$(basename "$installed_app")"
[[ "$(basename "$installed_app")" == Forkgram.app ]] || Error "destination must be Forkgram.app"

if pgrep -x Forkgram >/dev/null 2>&1; then
  Error "quit Forkgram in all user sessions before installation"
fi
source_real="$(cd "$source_app" && pwd -P)"
[[ "$source_real" != "$installed_app" && "$installed_app" != "$source_real/"* && "$source_real" != "$installed_app/"* ]] \
  || Error "source and destination must be separate, non-nested bundles"
python3 "$bundle_tool" finalize "$source_app" --deep-sign

if [[ -e "$installed_app" || -L "$installed_app" ]]; then
  python3 "$bundle_tool" validate "$installed_app"
  backup_dir="$(mktemp -d "$(dirname "$installed_app")/Forkgram-backup.XXXXXX")"
  mv "$installed_app" "$backup_dir/Forkgram.app"
  echo "Previous bundle retained at: $backup_dir/Forkgram.app"
fi

ditto "$source_app" "$installed_app"
python3 "$bundle_tool" finalize "$installed_app" --deep-sign
python3 "$bundle_tool" verify-install "$installed_app" --user "$verify_user"
echo "Installed and verified: $installed_app"
