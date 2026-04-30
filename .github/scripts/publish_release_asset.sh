#!/usr/bin/env bash
set -euo pipefail

PublishRelease="${PUBLISH_RELEASE:-true}"
if [[ "$GITHUB_EVENT_NAME" == "pull_request" || "$GITHUB_EVENT_NAME" == "pull_request_target" || "$PublishRelease" != "true" ]]; then
  echo "Skip GitHub Release publishing for event '$GITHUB_EVENT_NAME'."
  exit 0
fi

if [[ -z "${GH_TOKEN:-}" && -n "${GITHUB_TOKEN:-}" ]]; then
  export GH_TOKEN="$GITHUB_TOKEN"
fi
if [[ -z "${GH_TOKEN:-}" ]]; then
  echo "::error::GH_TOKEN or GITHUB_TOKEN is required to publish a release asset."
  exit 1
fi

AssetPath="${RELEASE_ASSET_PATH:?RELEASE_ASSET_PATH is required}"
AssetName="${RELEASE_ASSET_NAME:?RELEASE_ASSET_NAME is required}"
AssetLabel="${RELEASE_ASSET_LABEL:-}"
VersionFile="${RELEASE_VERSION_FILE:-Telegram/build/version}"
ChangelogFile="${RELEASE_CHANGELOG_FILE:-changelog.txt}"
WorkflowSha="${GITHUB_SHA:?GITHUB_SHA is required}"
ReleaseChannel="${RELEASE_CHANNEL:-}"
DevSuffix="${RELEASE_DEV_SUFFIX:-dev}"

if [[ -z "$ReleaseChannel" ]]; then
  if [[ "$GITHUB_EVENT_NAME" == "push" ]]; then
    ReleaseChannel="dev"
  else
    ReleaseChannel="release"
  fi
fi

if [[ ! -f "$AssetPath" ]]; then
  echo "::error::Release asset does not exist: $AssetPath"
  exit 1
fi
if [[ ! -f "$VersionFile" ]]; then
  echo "::error::Version file does not exist: $VersionFile"
  exit 1
fi

AppVersionStr=""
BetaChannel="0"
AlphaVersion="0"
while read -r Key Value Rest; do
  Value="${Value//$'\r'/}"
  case "$Key" in
    AppVersionStr) AppVersionStr="$Value" ;;
    BetaChannel) BetaChannel="$Value" ;;
    AlphaVersion) AlphaVersion="$Value" ;;
  esac
done < "$VersionFile"

if [[ -z "$AppVersionStr" ]]; then
  echo "::error::AppVersionStr was not found in $VersionFile."
  exit 1
fi
if [[ "$AlphaVersion" != "0" ]]; then
  echo "::notice::Closed alpha version $AlphaVersion is not published as a GitHub Release."
  exit 0
fi

Tag="v$AppVersionStr"
ReleaseName="Forkgram $AppVersionStr"
PrereleaseArgs=()
LatestArgs=()
if [[ "$ReleaseChannel" == "dev" ]]; then
  Tag="$Tag-$DevSuffix"
  ReleaseName="Forkgram $AppVersionStr $DevSuffix"
  PrereleaseArgs=(--prerelease)
  LatestArgs=(--latest=false)
elif [[ "$BetaChannel" != "0" ]]; then
  ReleaseName="Forkgram $AppVersionStr beta"
  PrereleaseArgs=(--prerelease)
  LatestArgs=(--latest=false)
fi

if [[ "$ReleaseChannel" == "dev" ]]; then
  git tag -f "$Tag" "$WorkflowSha"
  git push origin "refs/tags/$Tag" --force
fi

NotesFile="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/forkgram-release-notes-$AppVersionStr.md"
if [[ "$ReleaseChannel" == "dev" ]]; then
  {
    printf 'Development build for v%s.\n\n' "$AppVersionStr"
    printf 'Commit: %s\n' "$WorkflowSha"
    if [[ -n "${GITHUB_REF_NAME:-}" ]]; then
      printf 'Ref: %s\n' "$GITHUB_REF_NAME"
    fi
  } > "$NotesFile"
elif [[ -f "$ChangelogFile" ]]; then
  awk -v version="$AppVersionStr" '
    BEGIN { found = 0 }
    /^[0-9]+\.[0-9]+/ {
      if (found == 1) {
        exit
      }
      if (index($0, version " ") == 1 || $0 == version) {
        found = 1
        next
      }
    }
    found == 1 { print }
  ' "$ChangelogFile" > "$NotesFile.raw"
  sed -e '/./,$!d' "$NotesFile.raw" \
    | awk '{ lines[NR] = $0; if ($0 ~ /[^[:space:]]/) last = NR } END { for (i = 1; i <= last; ++i) print lines[i] }' \
    > "$NotesFile"
fi
if [[ ! -s "$NotesFile" ]]; then
  printf 'Automated build for %s.\n\nCommit: %s\n' "$Tag" "$WorkflowSha" > "$NotesFile"
fi

if ! gh release view "$Tag" >/dev/null 2>&1; then
  if ! gh release create "$Tag" \
    --target "$WorkflowSha" \
    --title "$ReleaseName" \
    --notes-file "$NotesFile" \
    "${PrereleaseArgs[@]}" \
    "${LatestArgs[@]}"; then
    gh release view "$Tag" >/dev/null
  fi
fi
if [[ "$ReleaseChannel" == "dev" ]]; then
  gh release edit "$Tag" \
    --target "$WorkflowSha" \
    --title "$ReleaseName" \
    --notes-file "$NotesFile" \
    --prerelease
fi

UploadDir="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/forkgram-release-assets"
mkdir -p "$UploadDir"
UploadPath="$UploadDir/$AssetName"
rm -f "$UploadPath"
cp "$AssetPath" "$UploadPath"

UploadArg="$UploadPath"
if [[ -n "$AssetLabel" ]]; then
  UploadArg="$UploadPath#$AssetLabel"
fi

gh release upload "$Tag" "$UploadArg" --clobber
