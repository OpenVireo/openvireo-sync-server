# Release signing

Releases are signed with the OpenVireo desktop app's identities (OpenVireo/openvireo, `.github/RELEASE-SIGNING.md`
there); only the secrets have to be set again here, since secret values can't be copied between repositories.

| What | How | Where |
| --- | --- | --- |
| macOS binaries (arm64, x64) | Developer ID Application signature, hardened runtime, secure timestamp, notarized | `binaries-native.yml` (`.github/scripts/sign-macos.sh`) |
| Windows binary (x64) | Azure Artifact Signing (Authenticode, RFC 3161 timestamp) through SignTool's dlib | `binaries-native.yml` |
| Linux binaries, image | none; covered by the signed checksums (the image has its build provenance) | |
| `SHA256SUMS` | detached GPG signature `SHA256SUMS.asc` with the OpenVireo release key | `release.yml`, job `release` |

`binaries-native.yml` signs `out/bin/openvireo-sync-server` (macOS) or `out/bin/openvireo-sync-server.exe`
(Windows) after the build and its tests, before it packs the archive, so the archive, the smoke test and
`SHA256SUMS` all have the signed file. After the smoke test it checks the signature in the archive:
`codesign --verify --strict`, a Developer ID with the hardened runtime, and `spctl --assess --type install`
reporting `source=Notarized Developer ID`; on Windows `Get-AuthenticodeSignature` must be `Valid`. The `release` job
checks that `SHA256SUMS.asc` comes from a subkey of the release key `23F6 403E 9392 E946 475E 7FD3 B556 83A4 3E98 C11C`.

The server is native code and needs none of the hardened runtime's exceptions, so it is signed without
entitlements; the full smoke test (HTTPS with the statically linked OpenSSL) passes on a hardened build.
Apple can't staple a ticket to a bare executable, so Gatekeeper looks it up online the first time it sees the file.
The Azure login happens right before SignTool, after the long vcpkg build, so the OIDC token is still fresh and,
unlike the desktop app, no login refresh is needed.

## When signing runs

All the secrets live in the repository's **`signing` environment**, which only allows `v*` tags.

- **Release tag:** `binaries-native` (called by `release.yml` with `secrets: inherit` and `id-token: write`) and
  `release` run in the environment, and a missing secret or variable fails them with an error: no release ships
  unsigned binaries. The image is pushed before the `release` job runs; set the secret and re-run the failed jobs.
- **`gh workflow run binaries-native.yml --ref <branch>`:** no environment and no secrets, so signing is skipped
  with a warning and the binaries are built and tested as usual. The Windows job then signs a copy with a
  throwaway self-signed certificate and runs it, to check that a signed binary still starts.
- **Pull requests and forks** never get the environment.

To try signing from a branch, allow it in the environment for the test, start it with `sign`, then remove it:

```sh
R=OpenVireo/openvireo-sync-server
gh api -X POST repos/$R/environments/signing/deployment-branch-policies -f name=my-branch -f type=branch
gh workflow run binaries-native.yml -R $R --ref my-branch -f sign=true
gh api repos/$R/environments/signing/deployment-branch-policies   # find its id, then:
gh api -X DELETE repos/$R/environments/signing/deployment-branch-policies/<id>
```

## Secrets and variables (environment `signing`)

| Name | Kind | Value |
| --- | --- | --- |
| `MACOS_CERTIFICATE_P12` | secret | base64 of the exported "Developer ID Application" certificate and key (.p12) |
| `MACOS_CERTIFICATE_PASSWORD` | secret | the .p12 password |
| `APPLE_ID` | secret | Apple ID for notarization |
| `APPLE_TEAM_ID` | secret | `DP5U2UP42V` |
| `APPLE_APP_PASSWORD` | secret | app-specific password of that Apple ID |
| `AZURE_CLIENT_ID` | secret | app registration `openvireo-github-signing`: `39cd540b-8c65-4104-87de-f5684440546f` |
| `AZURE_TENANT_ID` | secret | `186097fa-79cb-495c-8336-195a445eae9f` |
| `AZURE_SUBSCRIPTION_ID` | secret | `a4b5b3c9-6a08-46b7-a61c-f4cc21c19aa7` |
| `ARTIFACT_SIGNING_ENDPOINT` | variable | `https://eus.codesigning.azure.net` |
| `ARTIFACT_SIGNING_ACCOUNT` | variable | `openvireo` |
| `ARTIFACT_SIGNING_PROFILE` | variable | `openvireo-public` |
| `GPG_PRIVATE_KEY` | secret | the release key's signing subkey without a passphrase (`ci-signing-subkey.asc`) |

These are the same values as the desktop app's repository secrets of the same names.

## One-time setup

### 1. The `signing` environment, limited to `v*` tags

```sh
R=OpenVireo/openvireo-sync-server
gh api -X PUT repos/$R/environments/signing --input - <<'EOF'
{"deployment_branch_policy": {"protected_branches": false, "custom_branch_policies": true}}
EOF
gh api -X POST repos/$R/environments/signing/deployment-branch-policies -f name='v*' -f type=tag
gh api repos/$R/environments/signing/deployment-branch-policies --jq '.branch_policies[] | {name, type}'
```

Anyone who can push a `v*` tag can then sign. To limit that to admins, add a tag ruleset (optional):

```sh
gh api -X POST repos/$R/rulesets --input - <<'EOF'
{"name": "Release tags", "target": "tag", "enforcement": "active",
 "conditions": {"ref_name": {"include": ["refs/tags/v*"], "exclude": []}},
 "rules": [{"type": "creation"}, {"type": "update"}, {"type": "deletion"}],
 "bypass_actors": [{"actor_id": 5, "actor_type": "RepositoryRole", "bypass_mode": "always"}]}
EOF
```

### 2. Secrets and variables

From the same files and values used for OpenVireo/openvireo (`gh secret set` without `--body` prompts):

```sh
R=OpenVireo/openvireo-sync-server
base64 -i DeveloperIDApplication.p12 | gh secret set MACOS_CERTIFICATE_P12 -R $R --env signing
gh secret set MACOS_CERTIFICATE_PASSWORD -R $R --env signing
gh secret set APPLE_ID -R $R --env signing
gh secret set APPLE_TEAM_ID -R $R --env signing --body DP5U2UP42V
gh secret set APPLE_APP_PASSWORD -R $R --env signing
gh secret set AZURE_CLIENT_ID -R $R --env signing --body 39cd540b-8c65-4104-87de-f5684440546f
gh secret set AZURE_TENANT_ID -R $R --env signing --body 186097fa-79cb-495c-8336-195a445eae9f
gh secret set AZURE_SUBSCRIPTION_ID -R $R --env signing --body a4b5b3c9-6a08-46b7-a61c-f4cc21c19aa7
gh secret set GPG_PRIVATE_KEY -R $R --env signing < ci-signing-subkey.asc
gh variable set ARTIFACT_SIGNING_ENDPOINT -R $R --env signing --body https://eus.codesigning.azure.net
gh variable set ARTIFACT_SIGNING_ACCOUNT -R $R --env signing --body openvireo
gh variable set ARTIFACT_SIGNING_PROFILE -R $R --env signing --body openvireo-public
gh secret list -R $R --env signing && gh variable list -R $R --env signing
```

`ci-signing-subkey.asc` is the same export as for the desktop app:
`gpg --armor --export-secret-subkeys 21E5ED156C062545CC3D285BB3ABCB66FF7AE649! > ci-signing-subkey.asc`, with the
passphrase removed from the exported copy only (for example in a temporary `GNUPGHOME`).

Keep them in the environment rather than as repository or organization secrets: those would be readable by a
workflow on any branch, while the environment's tag rule keeps them to release tags.

### 3. Azure federated credential for this repository

The app registration already has the "Artifact Signing Certificate Profile Signer" role on `openvireo-public`; it
only needs to trust this repository's `signing` environment. The organization sends immutable OIDC subjects
(`gh api repos/OpenVireo/openvireo-sync-server/actions/oidc/customization/sub`), so the subject has the owner and
repository ids. A job of a reusable workflow gets the caller's repository and its own environment in the subject,
so this one credential covers both `release.yml` and a direct `binaries-native.yml` run.

```sh
az ad app federated-credential create --id 39cd540b-8c65-4104-87de-f5684440546f --parameters '{
  "name": "github-openvireo-sync-server-signing",
  "issuer": "https://token.actions.githubusercontent.com",
  "subject": "repo:OpenVireo@347287727/openvireo-sync-server@1432261424:environment:signing",
  "audiences": ["api://AzureADTokenExchange"]
}'
az ad app federated-credential list --id 39cd540b-8c65-4104-87de-f5684440546f --query '[].subject'
```

## Pinned downloads

`binaries-native.yml` pins the Artifact Signing client (dlib) by version and SHA-256 in its `env` block, the same as
the desktop app's `build.yml`. Update the version and checksum together.
