# Release signing

The `release` job of `release.yml` signs `SHA256SUMS` with the OpenVireo release key, the same key as the
desktop app's releases (OpenVireo/openvireo, `.github/RELEASE-SIGNING.md` there), and attaches the detached
signature `SHA256SUMS.asc` to the GitHub release. It checks that the signature comes from a subkey of the
release key `23F6 403E 9392 E946 475E 7FD3 B556 83A4 3E98 C11C`.

The key lives in the repository's **`signing` environment**, which only allows `v*` tags. The `release` job runs
only for tags and fails if the key is missing: the image is already pushed by then, so set the secret and
re-run the failed job to create the release. Pull requests and branch runs never use the environment.

| Name | Kind | Value |
| --- | --- | --- |
| `GPG_PRIVATE_KEY` | secret (environment `signing`) | the release key's signing subkey without a passphrase (`ci-signing-subkey.asc`) |

## One-time setup

```sh
R=OpenVireo/openvireo-sync-server
gh api -X PUT repos/$R/environments/signing --input - <<'EOF'
{"deployment_branch_policy": {"protected_branches": false, "custom_branch_policies": true}}
EOF
gh api -X POST repos/$R/environments/signing/deployment-branch-policies -f name='v*' -f type=tag
gh secret set GPG_PRIVATE_KEY -R $R --env signing < ci-signing-subkey.asc
gh secret list -R $R --env signing
```

`ci-signing-subkey.asc` is the same export as for the desktop app:
`gpg --armor --export-secret-subkeys 21E5ED156C062545CC3D285BB3ABCB66FF7AE649! > ci-signing-subkey.asc`, with the
passphrase removed from the exported copy only (for example in a temporary `GNUPGHOME`).

Optionally limit who can push `v*` tags (and so sign) to admins:

```sh
gh api -X POST repos/$R/rulesets --input - <<'EOF'
{"name": "Release tags", "target": "tag", "enforcement": "active",
 "conditions": {"ref_name": {"include": ["refs/tags/v*"], "exclude": []}},
 "rules": [{"type": "creation"}, {"type": "update"}, {"type": "deletion"}],
 "bypass_actors": [{"actor_id": 5, "actor_type": "RepositoryRole", "bypass_mode": "always"}]}
EOF
```

## macOS and Windows binaries

Code signing of native macOS and Windows binaries is not set up yet. When it is, it will use the same
`signing` environment and the desktop app's identities: a Developer ID with notarization, and Azure Artifact
Signing through the app registration `openvireo-github-signing`, which needs a federated credential for this
repository's environment (the organization sends immutable OIDC subjects):

```sh
az ad app federated-credential create --id 39cd540b-8c65-4104-87de-f5684440546f --parameters '{
  "name": "github-openvireo-sync-server-signing",
  "issuer": "https://token.actions.githubusercontent.com",
  "subject": "repo:OpenVireo@347287727/openvireo-sync-server@1432261424:environment:signing",
  "audiences": ["api://AzureADTokenExchange"]
}'
```
