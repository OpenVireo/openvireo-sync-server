#!/usr/bin/env bash
# Signs a macOS executable in place with the hardened runtime and a secure timestamp, and optionally notarizes it
# (binaries-native.yml):
#
#   .github/scripts/sign-macos.sh out/bin/openvireo-sync-server [identity]   # identity default "-" (ad hoc)
#
# With NOTARY_PROFILE set (a `xcrun notarytool store-credentials` profile; NOTARY_KEYCHAIN for a keychain other
# than the default) it submits the signed file to Apple's notary service and fails unless Apple accepts it. A bare
# executable can't be stapled: Gatekeeper looks the ticket up online the first time it sees the file.
# The server is native code that needs none of the hardened runtime's exceptions, so it has no entitlements.
set -euo pipefail

exe=${1:?usage: $0 <executable> [identity]}
identity=${2:--}

args=(--force --options runtime --identifier "com.openvireo.$(basename "$exe")")
# A secure timestamp needs a real identity; the notary service requires one
[[ "$identity" != "-" ]] && args+=(--timestamp)
codesign "${args[@]}" --sign "$identity" "$exe"
codesign --verify --strict --verbose=2 "$exe"
codesign -dvv "$exe" 2>&1 | grep -E '^(Identifier=|CodeDirectory |Authority=|TeamIdentifier=|Timestamp=)'

if [[ -n "${NOTARY_PROFILE:-}" ]]; then
  work=$(mktemp -d)
  trap 'rm -rf "$work"' EXIT
  ditto -c -k --keepParent "$exe" "$work/notarize.zip"
  notary=(--keychain-profile "$NOTARY_PROFILE")
  [[ -n "${NOTARY_KEYCHAIN:-}" ]] && notary+=(--keychain "$NOTARY_KEYCHAIN")
  xcrun notarytool submit "$work/notarize.zip" "${notary[@]}" --wait --timeout 30m --output-format json \
    | tee "$work/result.json"
  echo
  id=$(plutil -extract id raw "$work/result.json")
  status=$(plutil -extract status raw "$work/result.json")
  xcrun notarytool log "$id" "${notary[@]}" || true
  [[ "$status" == Accepted ]] || { echo "notarization: $status" >&2; exit 1; }
fi
