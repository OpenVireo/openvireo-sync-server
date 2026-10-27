# Accounts and vaults

This page is for running the sync server behind an OpenVireo **account server** (`openvireo-auth`), as the
hosted service does. A self-hosted
server needs none of it.

With an account server, clients authenticate with short-lived **access tokens** (JWT, signed with Ed25519)
instead of device tokens, and databases become **vaults**: one encrypted `.kdbx` per vault, shared by
everyone the account server grants access to. The sync server never decides who may open a vault; it only
checks what the token says. Sharing, organisations, key rotation and recovery all happen on the account
server and in the clients.

## Modes

| `SYNC_AUTH_MODE` | Accepted tokens | Routes |
|---|---|---|
| `standalone` (default) | device tokens | `databases/*`, `devices/pairing*`, `events` |
| `jwt` | access tokens | `vaults`, `vaults/*`, `events`, `internal/*` |
| `both` | either; a token with two `.` is an access token | all of the above |

Routes a mode doesn't have answer `404`. In `both` mode, an access token on a legacy route, or a device token
on a vault route, gets `403`. The hosted service runs `jwt`. `both` is for a server that already has device
token users who need to move their databases into account vaults (see [below](#moving-a-database-into-a-vault)).

## Settings

| Variable | Default | Meaning |
|---|---|---|
| `SYNC_AUTH_JWKS_URL` | *(required for `jwt`/`both`)* | The account server's `/.well-known/jwks.json` |
| `SYNC_AUTH_ISSUER` | *(required for `jwt`/`both`)* | Its issuer URL, the tokens' `iss` (as in its `/.well-known/openvireo-configuration`) |
| `SYNC_AUTH_AUDIENCE` | `ov-sync` | The audience every token's `aud` must contain |
| `SYNC_INTERNAL_TOKEN` | *(none)* | Shared secret the account server uses for the [internal routes](api.md#internal-routes). Unset: those routes are off (`404`) |

For the hosted setup (use `both` instead while device token users still have databases to move):

```sh
SYNC_AUTH_MODE=jwt \
SYNC_AUTH_JWKS_URL=https://accounts.example.org/.well-known/jwks.json \
SYNC_AUTH_ISSUER=https://accounts.example.org \
SYNC_INTERNAL_TOKEN=<random secret shared with the account server> \
SYNC_ALLOWED_ORIGINS=https://app.example.org \
openvireo-sync-server
```

Generate the internal token with something like `openssl rand -base64 32` and give the account server the same
value. Anyone who has it can push access events and delete vaults, so keep it out of the clients.

## Access tokens

The server accepts only `EdDSA` tokens and checks the signature, `iss`, `aud`, `exp` and `nbf` (with 60 seconds
of clock skew). It fetches the JWKS on first use and caches it, fetches it again when a token names a key id it
doesn't know (at most once every 30 seconds) and refreshes it hourly, so a key rollover on the account server
needs no restart.

The token's `vlt` claim lists the vaults it opens and the role for each:

| Role | Allows |
|---|---|
| `read` | list, download, history |
| `write` | also update an existing vault (`PUT` with `If-Match`) |
| `manage` | also create (`PUT` with `If-None-Match: *`) and delete |

A vault the token doesn't list gets `403`, whether it exists or not, so a token can't probe for other vaults.

An entry may also carry `q`, the largest database the vault may hold under its owner's plan (bytes; the
hosted service's plans, SPEC.md §19). A larger upload gets `413` with code `quota_exceeded` and `max`, before
anything is stored. Without `q` only `SYNC_MAX_DB_BYTES` applies, which is always the case on a self-hosted
account server.
The token's `sub` (`user:<uuid>` or `sa:<uuid>` for a service account) decides which
[access events](api.md#account-events) its events stream receives.

## Access changes

When the account server grants, revokes or rotates access, it tells the sync server with
`POST /api/v1/internal/notify`, and every events stream of the affected subjects gets an `access` event. The
clients then fetch a fresh access token (with the new `vlt`) and reconnect. A stream also ends once its token's
`exp` has passed, so a revoked grant stops working within one token lifetime even if the event is lost.

When a vault must go without any client deleting it, for example once an account recovery archive expires, the
account server sends `DELETE /api/v1/internal/vaults/{vault}`.

With Postgres, both reach streams on every instance (through `NOTIFY`).

## Moving a database into a vault

To turn a user's database into a vault (for a user moving from a device token to an account), take the vault
id from the account server and run

```sh
openvireo-sync-server migrate-to-vaults <user> <database-id> <vault-uuid>
```

It copies the database with its kept versions, revisions and ETags to `vaults/<vault-uuid>` and then deletes the
user's copy (devices of that user see it deleted). It refuses when the vault already exists and leaves
everything as it was if the database changes while it is copied. It works with either storage; with the data
directory, stop the server while it runs.

Clients that move a database themselves (OpenVireo's and the web app's *Move to account*) don't need this:
they upload a re-keyed copy as a new vault.

## Storage of vaults

Vaults never share a storage key with a user's databases: in the data directory they live in
`vaults/<uuid>.kdbx` (next to `databases/`), with history in `vaults/.history/<uuid>/`; with Postgres in the
tables `vaults` and `vault_versions`, with files under `_vaults/<uuid>/` in the bucket, a prefix no user name can
have. `import <data-dir>` also copies a data directory's vaults.
