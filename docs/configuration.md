# Configuration

The server reads its settings from environment variables when it starts; it has no config file. An
invalid value (a word where a number belongs, an unknown `SYNC_AUTH_MODE`, S3 settings without
`SYNC_DATABASE_URL`) stops it with a message naming the variable. Flags take `1`/`true` or `0`/`false`.

## Server

| Variable | Default | Meaning |
|---|---|---|
| `SYNC_HOST` | `127.0.0.1` | Address to listen on (the Docker image sets `0.0.0.0`) |
| `SYNC_PORT` | `8787` | Port |
| `SYNC_THREADS` | `0` | Number of I/O threads, `0` means one per CPU core |
| `SYNC_BEHIND_PROXY` | `0` | `1` silences the warning about listening on a non-loopback address, when TLS is terminated in front of the server |
| `SYNC_MAX_DB_BYTES` | `52428800` | Upload size limit in bytes (50 MiB); larger uploads get `413` |
| `SYNC_HISTORY_SIZE` | `10` | How many previous versions of each database or vault to keep |
| `SYNC_ALLOWED_ORIGINS` | *(none)* | Comma-separated web origins allowed to call the API from a browser, e.g. `https://app.example.org`; `*` allows any. See [CORS](api.md#cors) |

## Storage

There are two ways to store users and databases:

- **Data directory** (the default): everything lives in `SYNC_DATA_DIR`: users, devices and pairing codes in
  `users.sqlite`, and one folder of `.kdbx` files per user. One server process, nothing else to run. A
  `users.json` from an older server is copied into `users.sqlite` on the first start and renamed to
  `users.json.imported`; old tokens keep working.
- **Postgres and an S3-compatible bucket**: users, devices, pairing codes and database metadata live in Postgres,
  the encrypted `.kdbx` files in a bucket (DigitalOcean Spaces, AWS S3, …). The server keeps nothing on its own
  disk, so containers can be replaced at will and several instances can run behind a load balancer: Postgres
  decides every upload, so two devices uploading at once through different instances still can't overwrite each
  other, and change notifications reach devices connected to any instance (through `LISTEN`/`NOTIFY`).

| Variable | Default | Meaning |
|---|---|---|
| `SYNC_DATA_DIR` | `./data` | The data directory, relative to the working directory. Not used when `SYNC_DATABASE_URL` is set |

Setting `SYNC_DATABASE_URL` picks Postgres and a bucket; it then needs all of the `SYNC_S3_*` settings except
the prefix and path style:

| Variable | Meaning |
|---|---|
| `SYNC_DATABASE_URL` | Postgres connection string, e.g. `postgresql://user:password@host:25060/db?sslmode=require` |
| `SYNC_DATABASE_SCHEMA` | Schema for the tables, created if missing (default: the connection's, usually `public`). Lower case letters, digits and `_` |
| `SYNC_DATABASE_CONNECTIONS` | Connections per instance (default: one per I/O thread) |
| `SYNC_S3_ENDPOINT` | e.g. `https://nyc3.digitaloceanspaces.com` |
| `SYNC_S3_REGION` | e.g. `nyc3` |
| `SYNC_S3_BUCKET` | Bucket name |
| `SYNC_S3_PREFIX` | Optional key prefix, so several servers can share a bucket, e.g. `openvireo/` |
| `SYNC_S3_ACCESS_KEY`, `SYNC_S3_SECRET_KEY` | Access key for the bucket |
| `SYNC_S3_PATH_STYLE` | `1` for `endpoint/bucket/key` URLs instead of `bucket.endpoint/key` (MinIO, Versity Gateway, …) |

The server creates and upgrades its tables when it starts (instances starting together take turns), or run
`openvireo-sync-server migrate`. Every six hours one instance deletes files in the bucket that no version
refers to any more, left behind by uploads that lost a race or crashed; `openvireo-sync-server sweep` does it
at once.

Postgres and S3 support needs a build with Drogon's PostgreSQL support; see
[Installing](install.md#from-source). Moving from a data directory to Postgres is described under
[Operations](operations.md#moving-to-postgres).

### What is stored where

| | Data directory | Postgres and bucket |
|---|---|---|
| Users, devices, pairing codes (token and code hashes only) | `users.sqlite` | tables `users`, `devices`, `pairing_codes` |
| A user's databases | `databases/<user>/<id>.kdbx` + `<id>.meta.json` | table `databases`, files `<prefix><user>/<id>/…` |
| Kept versions | `databases/<user>/.history/<id>/<revision>.kdbx` + `.json` | table `versions` |
| Vaults (account mode) | `vaults/<uuid>.kdbx`, history in `vaults/.history/<uuid>/` | tables `vaults`, `vault_versions`, files under `<prefix>_vaults/<uuid>/` |

## Accounts

These are only for running the server behind an OpenVireo account server. A self-hosted server needs none
of them. See [Accounts and vaults](accounts.md).

| Variable | Default | Meaning |
|---|---|---|
| `SYNC_AUTH_MODE` | `standalone` | Which tokens the server accepts: `standalone`, `jwt` or `both` (`legacy` is an old name for `standalone`) |
| `SYNC_AUTH_JWKS_URL` | *(required for `jwt`/`both`)* | The account server's `/.well-known/jwks.json` |
| `SYNC_AUTH_ISSUER` | *(required for `jwt`/`both`)* | Its issuer URL, the tokens' `iss` (as in its `/.well-known/openvireo-configuration`) |
| `SYNC_AUTH_AUDIENCE` | `ov-sync` | The audience every token's `aud` must contain |
| `SYNC_INTERNAL_TOKEN` | *(none)* | Shared secret the account server uses for the `/api/v1/internal/*` routes. Unset: those routes are off (`404`) |

## Docker Compose

| Variable | Default | Meaning |
|---|---|---|
| `SYNC_HOST_PORT` | `8787` | The host port `compose.yaml` and `compose.cloud.yaml` publish on `127.0.0.1`. Read by Compose, not by the server |
