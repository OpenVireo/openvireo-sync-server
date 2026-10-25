# OpenVireo Sync Server

A small REST server that stores OpenVireo databases so several devices can stay in sync.

The client side lives in [OpenVireo](https://github.com/openvireo/openvireo) (`src/gui/remote/`). OpenVireo is a fork of
[KeePassXC](https://keepassxc.org); this server is not part of or endorsed by the KeePassXC project.

The server only ever sees the **encrypted** `.kdbx` file. OpenVireo downloads it, decrypts and merges it locally, then uploads the merged result. Your master password and keys never leave the client.

## Quick start

The server is written in C++17 on [Drogon](https://github.com/drogonframework/drogon).

```sh
brew install drogon sqlite     # or: apt install libdrogon-dev libsqlite3-dev, vcpkg install drogon sqlite3, …
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/openvireo-sync-server users add alice laptop   # prints laptop's access token, shown only once
./build/openvireo-sync-server                   # http://127.0.0.1:8787
```

Then in OpenVireo:

1. Open your database and go to **Database → Database Settings → Remote Sync**.
2. Enter a name, set **Type** to *OpenVireo Sync Server*, and fill in:
   - **Server URL**: `http://127.0.0.1:8787`, or your `https://` URL
   - **Database ID**: any name, e.g. `personal`
   - **Access token**: the token printed for this device. It is kept on this device only (in the Keychain on
     macOS, the Credential Manager on Windows, the desktop keyring on Linux), not in the database
3. Click **Test Connection**, then **Save**, then **OK**.
4. Use **Database → Remote Sync → <name>** to sync.

The first sync uploads your local database. Later syncs download the server copy, merge it with your local database (the same merge as *Merge from Database*), save it locally, and upload the result.

Other devices use the same Database ID. If a device's copy uses a different password or key file, OpenVireo asks you to unlock the server copy before merging.

### Setting up another device

Every device has its own access token, so one lost device can be shut out without touching the others. The easiest
way to give the new device one is a pairing code: on a device that already syncs, open **Remote Sync** in the
database settings, select the sync server and click **Pair Device…**. Or on the server:

```sh
./build/openvireo-sync-server devices pair alice   # prints a code such as WJDA-247W
```

On the new device, enter the code where OpenVireo asks for the access token. It trades the code for a token of its
own, named after the computer. A code adds one device and works for 10 minutes. If you'd rather copy a token by hand,
`devices add alice phone` prints one.

To get the database onto a new device:

1. Use **Database → Import…** and choose *OpenVireo Sync Server (.kdbx)*.
2. Enter the server URL and a pairing code (or the new device's access token), then click **Load** to pick a database,
   or type its ID.
3. Enter the database's password and/or key file.
4. Choose where to import it:
   - **Local Copy** opens the server database unchanged (same password) and asks where to save it. It is already linked to the server, so **Remote Sync** works right away.
   - **Existing Database** copies its entries into a group of a database you have open.
   - **Temporary Database** opens it without saving it anywhere first.

## Configuration

Set these as environment variables:

| Variable | Default | Meaning |
|---|---|---|
| `SYNC_HOST` | `127.0.0.1` | Address to listen on |
| `SYNC_PORT` | `8787` | Port |
| `SYNC_DATA_DIR` | `./data` | Where users and databases are stored (relative to the working directory), unless `SYNC_DATABASE_URL` is set |
| `SYNC_MAX_DB_BYTES` | `52428800` | Upload size limit (50 MiB) |
| `SYNC_HISTORY_SIZE` | `10` | How many previous versions of each database to keep |
| `SYNC_THREADS` | `0` | Number of I/O threads, `0` means one per CPU core |
| `SYNC_BEHIND_PROXY` | `0` | `1` silences the warning about listening on a non-loopback address, when TLS is terminated in front of the server |
| `SYNC_ALLOWED_ORIGINS` | *(none)* | Comma-separated web origins allowed to call the API from a browser (CORS), e.g. `https://app.example.org`; `*` allows any. See [Web apps](#web-apps-cors) |
| `SYNC_AUTH_MODE` | `standalone` | Which tokens the server accepts: `standalone`, `jwt` or `both` (`legacy` is an old name for `standalone`). See [Accounts](#accounts-jwt-and-vaults) |

The account variables (`SYNC_AUTH_JWKS_URL`, `SYNC_AUTH_ISSUER`, `SYNC_AUTH_AUDIENCE`, `SYNC_INTERNAL_TOKEN`) are
described under [Accounts](#accounts-jwt-and-vaults). A self-hosted server needs none of them.

### Storage

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

Setting `SYNC_DATABASE_URL` picks the second; it then needs all of these:

| Variable | Meaning |
|---|---|
| `SYNC_DATABASE_URL` | Postgres connection string, e.g. `postgresql://user:password@host:25060/db?sslmode=require` |
| `SYNC_DATABASE_SCHEMA` | Schema for the tables, created if missing (default: the connection's, usually `public`) |
| `SYNC_DATABASE_CONNECTIONS` | Connections per instance (default: one per I/O thread) |
| `SYNC_S3_ENDPOINT` | e.g. `https://nyc3.digitaloceanspaces.com` |
| `SYNC_S3_REGION` | e.g. `nyc3` |
| `SYNC_S3_BUCKET` | Bucket name |
| `SYNC_S3_PREFIX` | Optional key prefix, so several servers can share a bucket, e.g. `openvireo/` |
| `SYNC_S3_ACCESS_KEY`, `SYNC_S3_SECRET_KEY` | Access key for the bucket |
| `SYNC_S3_PATH_STYLE` | `1` for `endpoint/bucket/key` URLs instead of `bucket.endpoint/key` (MinIO, Versity Gateway, …) |

The server creates and upgrades its tables when it starts (instances starting together take turns), or run
`openvireo-sync-server migrate`. Every few hours one instance deletes files in the bucket that no version
refers to any more, left behind by uploads that lost a race or crashed; `openvireo-sync-server sweep` does it
at once.

The `users` and `devices` commands work on whichever storage is configured. With Postgres you can run them
from any machine or one-off container with the same settings, and the running instances see the change at
once.

To move from a data directory to Postgres, run `openvireo-sync-server import <data-dir>` with the Postgres
settings. It copies users, devices (old per-user tokens included), databases and their kept versions, keeping
revisions and ETags, so devices carry on syncing without noticing. It leaves users and databases that already
exist alone and says so.

Postgres and S3 support needs Drogon built with PostgreSQL (`-DBUILD_POSTGRESQL=ON`), which Homebrew's and
most distributions' packages are not; CMake turns it on when it finds such a Drogon
(`-DSYNC_SERVER_WITH_CLOUD=ON` makes it required). The Docker image always has it.

### Docker

```sh
docker compose up -d --build                                  # data directory in a volume, on 127.0.0.1:8787
docker compose run --rm sync-server users add alice laptop
```

`compose.cloud.yaml` runs the stateless setup locally: Postgres, an S3 gateway standing in for Spaces, two
server instances and nginx in front of them:

```sh
docker compose -f compose.cloud.yaml up -d --build
docker compose -f compose.cloud.yaml run --rm migrate users add alice laptop
docker compose -f compose.cloud.yaml run --rm --build test    # the API tests against Postgres and the bucket
```

Released images for linux/amd64 and linux/arm64 are on GHCR as `ghcr.io/openvireo/openvireo-sync-server`
(`:latest`, `:1`, `:1.2`, `:1.2.3`). Pushing a `v1.2.3` tag runs the tests and publishes them
(`.github/workflows/release.yml`).

`SYNC_HOST_PORT=18787` publishes either on another port. The image runs as an unprivileged user (uid 10001),
listens on `0.0.0.0:8787` and keeps the data directory in `/data`; a bind-mounted directory must be writable
by that uid.

### DigitalOcean App Platform

`.do/app.yaml` deploys two instances from the Dockerfile with a managed Postgres database and a Spaces
bucket. Create the bucket and a Spaces access key first, fill in the placeholders, then:

```sh
doctl apps create --spec .do/app.yaml
```

App Platform terminates HTTPS. Add users from the app's console (`openvireo-sync-server users add alice
laptop`) or from any machine with the same `SYNC_*` settings.

### Deploying beyond localhost

The access token travels in the `Authorization` header, so **use HTTPS** for anything other than localhost. Keep the server on `127.0.0.1` and put a TLS-terminating reverse proxy (Caddy, nginx, …) in front of it. For example, with Caddy:

```
sync.example.com {
    reverse_proxy 127.0.0.1:8787
}
```

OpenVireo does not follow redirects, so the token is never forwarded to another host. Enter the final `https://` URL.

## Users and devices

```sh
./build/openvireo-sync-server users add <name> [device-name]     # create a user with a first device
./build/openvireo-sync-server users remove <name>
./build/openvireo-sync-server users list

./build/openvireo-sync-server devices add <user> <device-name>   # a new device and its access token
./build/openvireo-sync-server devices pair <user>                # a one-time code a new device trades for a token
./build/openvireo-sync-server devices list <user>                # ids, names, when each was last seen
./build/openvireo-sync-server devices revoke <user> <device-id>  # its token stops working at once
```

Each user has their own namespace and cannot see other users' databases. All of a user's devices see the same
databases. Only SHA-256 hashes of tokens and pairing codes are stored, in `data/users.sqlite` (or in Postgres). Changes take effect
without a restart.

A device was lost? Revoke it. Its token can no longer reach the server. If whoever has it may also know the
database's password, change that password too, so copies uploaded from now on can't be opened with the old one.

### Upgrading from one token per user

Tokens from before devices existed keep working, as a device called `legacy` that all your devices share. Older
OpenVireo versions also stored that token inside the database. To move to per-device tokens:

1. `devices add <user> <device-name>` for each device, and enter each token in that device's Remote Sync settings.
   Saving the settings also removes the old token from the database.
2. `devices revoke <user> legacy`.

## API

All endpoints except `/health` and the pairing claim need `Authorization: Bearer <token>`, with the token of one of the user's devices. Errors are JSON: `{ "error": "..." }`.

| Method | Path | Description |
|---|---|---|
| `GET` | `/api/v1/health` | Liveness check |
| `GET` | `/api/v1/databases` | List your databases (`id`, `revision`, `etag`, `size`, `updatedAt`) |
| `GET` / `HEAD` | `/api/v1/databases/{id}` | Download a database. Supports `If-None-Match` (returns `304`) |
| `PUT` | `/api/v1/databases/{id}` | Upload a database (raw `.kdbx` body) |
| `DELETE` | `/api/v1/databases/{id}` | Delete a database and its history. Supports `If-Match` |
| `GET` | `/api/v1/databases/{id}/history` | The current and kept previous versions, newest first (`revision`, `etag`, `size`, `updatedAt`) |
| `GET` | `/api/v1/databases/{id}/history/{revision}` | Download one of those versions |
| `GET` | `/api/v1/events` | Change notifications as server-sent events (see below) |
| `POST` | `/api/v1/devices/pairing` | A pairing code for another device of the same user: `201 { "code", "expiresAt" }` |
| `POST` | `/api/v1/devices/pairing/claim` | No token. `{ "code", "deviceName" }` → `201 { "user", "deviceId", "token" }` (see below) |

`{id}` may contain letters, digits, `.`, `_` and `-`, up to 64 characters.

### Pairing

A pairing code is eight letters and digits (no `0`/`O` or `1`/`I`), shown as `ABCD-EFGH`; case, spaces and the dash
don't matter when claiming. It works once and for 10 minutes, and a user has at most 5 waiting (a new one replaces the
oldest). Wrong codes are limited to 10 per client address and 100 in all per 10 minutes, after which claims get `429`
with `Retry-After`. Behind a reverse proxy every client has the proxy's address, so the per-address limit applies to
everyone together.

### Concurrency

Every version has an `ETag`, the quoted SHA-256 of the file. Uploads must say which version they replace:

- `If-None-Match: *` creates the database. Returns `412` if it already exists.
- `If-Match: "<etag>"` replaces exactly that version. Returns `412` if another device uploaded in the meantime.
- An upload with neither header is rejected with `428` when the database already exists.

This means two devices can never silently overwrite each other's changes. When OpenVireo gets a `412`, your local changes are already saved. Sync again to merge in the other device's changes.

Uploads must start with the KDBX file signature (`415` otherwise). Uploads over the size limit get `413`; that response has no body because the upload is refused before it is read.

### History

Before each overwrite, the previous version is copied to `data/databases/<user>/.history/<id>/<revision>.kdbx`, with
its metadata next to it in `<revision>.json`. The newest `SYNC_HISTORY_SIZE` versions are kept.

In OpenVireo, **Database → Remote Sync → Versions on <name>…** lists them. Opening one downloads it and shows it in
its own tab, where you can copy entries from it or merge it into your database. Nothing on the server changes.

### Change notifications

`GET /api/v1/events` stays open and sends a server-sent event to all of a user's connected devices after every change
to one of their databases:

```
event: database
data: {"id":"personal","etag":"\"…\"","revision":12}
```

A deleted database sends `{"id":"personal","deleted":true}`. A comment line (`: ping`) every 25 seconds keeps idle
connections open. OpenVireo listens while automatic sync is on, syncs as soon as another device uploads, and only polls
every 5 minutes while it is connected (every minute otherwise). Behind a reverse proxy, make sure it doesn't buffer
responses (the server sends `X-Accel-Buffering: no` for nginx) and allows long-lived requests.

## Web apps (CORS)

A browser only lets a web app on another origin call the server when the server allows that origin. List the origins
in `SYNC_ALLOWED_ORIGINS`, separated by commas (no path or trailing slash). For those origins the server answers
preflight (`OPTIONS`) requests on every route with `204`, allowing `GET, HEAD, PUT, POST, DELETE` and the headers
`Authorization, Content-Type, If-Match, If-None-Match`, and every response carries `Access-Control-Allow-Origin` and
`Access-Control-Expose-Headers: ETag, X-Database-Revision, Last-Modified`, so the app can read ETags. Tokens travel in
the `Authorization` header, never in cookies, so credentials mode is not used. With the variable unset (the default)
the server sends no CORS headers at all.

**Using a self-hosted server from the hosted web app:** set `SYNC_ALLOWED_ORIGINS` to the origin the web app is
served from (the address in the browser's address bar, scheme and host only), keep `SYNC_AUTH_MODE` at `standalone`, and
pick *Use my own sync server* in the web app with your server's URL and a pairing code. The server must be reachable
over **HTTPS** (see [Deploying beyond localhost](#deploying-beyond-localhost)): a page served over HTTPS can't call a
plain `http://` server, except one on `localhost`. A server on `localhost` or the local network also works with
Chrome's Private Network Access checks: preflights asking for it get `Access-Control-Allow-Private-Network: true`. If a
reverse proxy answers `OPTIONS` itself, let it pass those requests through.

```sh
SYNC_ALLOWED_ORIGINS=https://app.example.org ./build/openvireo-sync-server
```

## Accounts (JWT) and vaults

With an account server (`openvireo-auth`), clients authenticate with short-lived **access tokens** (JWT, signed
with Ed25519) instead of device tokens, and databases become **vaults**: one encrypted `.kdbx` per vault, shared by
everyone the account server grants access to. This is off unless `SYNC_AUTH_MODE` says otherwise; a self-hosted
server stays exactly as described above.

| `SYNC_AUTH_MODE` | Accepted tokens | Routes |
|---|---|---|
| `standalone` (default) | device tokens | `databases/*`, `devices/pairing*`, `events` |
| `jwt` | access tokens | `vaults`, `vaults/*`, `events`, `internal/*` |
| `both` | either; a token with two `.` is an access token | all of the above |

Routes a mode doesn't have answer `404`. In `both` mode, an access token on a legacy route, or a device token on a
vault route, gets `403`.

| Variable | Default | Meaning |
|---|---|---|
| `SYNC_AUTH_JWKS_URL` | *(required for `jwt`/`both`)* | The account server's `/.well-known/jwks.json` |
| `SYNC_AUTH_ISSUER` | *(required for `jwt`/`both`)* | Its issuer URL, the tokens' `iss` (as in its `/.well-known/openvireo-configuration`) |
| `SYNC_AUTH_AUDIENCE` | `ov-sync` | The audience every token's `aud` must contain |
| `SYNC_INTERNAL_TOKEN` | *(none)* | Shared secret the account server uses for `POST /api/v1/internal/notify` and `DELETE /api/v1/internal/vaults/{vault}`. Unset: those routes are off (`404`) |

The server accepts only `EdDSA` tokens and checks the signature, `iss`, `aud`, `exp` and `nbf` (with 60 seconds of
clock skew). It fetches the JWKS on first use and caches it, fetches it again when a token names a key id it doesn't
know (at most once every 30 seconds) and refreshes it hourly, so a key rollover on the account server needs no
restart. The token's `vlt` claim lists the vaults it opens and the role for each: `read` < `write` < `manage`.

For the hosted setup:

```sh
SYNC_AUTH_MODE=both \
SYNC_AUTH_JWKS_URL=https://accounts.example.org/.well-known/jwks.json \
SYNC_AUTH_ISSUER=https://accounts.example.org \
SYNC_INTERNAL_TOKEN=<random secret shared with the account server> \
SYNC_ALLOWED_ORIGINS=https://app.example.org \
./build/openvireo-sync-server
```

### Vault routes

`{vault}` is the vault's id, a lowercase UUID. Errors on these routes also carry a machine-readable code:
`{"error": "...", "code": "forbidden"}` (`unauthorized`, `forbidden`, `not_found`, `invalid_request`,
`precondition_failed`, `precondition_required`).

| Method | Path | Role | Description |
|---|---|---|---|
| `GET` | `/api/v1/vaults` | any | `{"vaults": [{id, revision, etag, size, updatedAt}]}` for the token's vaults that exist |
| `GET` / `HEAD` | `/api/v1/vaults/{vault}` | read | Download, with `ETag`, `X-Database-Revision`, `Last-Modified`; `If-None-Match` gives `304` |
| `PUT` | `/api/v1/vaults/{vault}` | write / manage | `If-Match: "<etag>"` updates (write, `200`); `If-None-Match: *` creates (manage, `201`) |
| `DELETE` | `/api/v1/vaults/{vault}` | manage | Delete the vault and its history (`204`); supports `If-Match` |
| `GET` | `/api/v1/vaults/{vault}/history` | read | Kept versions, as for databases |
| `GET` | `/api/v1/vaults/{vault}/history/{revision}` | read | One kept version |

A vault the token doesn't list gets `403`, whether it exists or not. Conditional uploads work as for databases
(`412` on a mismatch), except that a vault `PUT` without `If-Match` or `If-None-Match` always gets `428`, even for a
vault that doesn't exist yet.

### Account events

`GET /api/v1/events` with an access token sends

```
event: vault
data: {"id":"<vault>","etag":"\"…\"","revision":3}        (or {"id":"<vault>","deleted":true})

event: access
data: {"change":"granted","vault":"<vault>"}               (vault may be null)
```

`vault` events only for the vaults in the token, `access` events when the account server reports an access change
for the token's subject. The server ends the stream once the token's `exp` has passed (checked every second); the
client reconnects with a fresh token, and does the same after any `access` event. Device-token streams keep sending
`event: database` as before.

The account server sends `access` events with `POST /api/v1/internal/notify`, `Authorization: Bearer
<SYNC_INTERNAL_TOKEN>` and `{"subjects": ["user:<uuid>", "sa:<uuid>"], "event": "access", "data": {"vault": <uuid or
null>, "change": "granted"|"revoked"|"rotating"|"rotated"|"org"}}`, answered with `204`. Delivery is best effort.
With Postgres the event reaches streams on every instance (through `NOTIFY`).

When no client deletes a vault, e.g. once a recovery archive expires, the account server sends `DELETE
/api/v1/internal/vaults/{vault}` with the same `Authorization: Bearer <SYNC_INTERNAL_TOKEN>`. It deletes the vault and
its history exactly as a `DELETE /api/v1/vaults/{vault}` with manage would, and the vault's streams get `{"id":
"<vault>", "deleted": true}`. It answers `204`, `404` (`not_found`) when there is no such vault, and `400` for an id
that isn't a lowercase UUID. An access or device token is never enough (`401`).

### Storage of vaults

Vaults never share a storage key with a user's databases: in the data directory they live in `vaults/<uuid>.kdbx`
(next to `databases/`), with history in `vaults/.history/<uuid>/`; with Postgres in the tables `vaults` and
`vault_versions` (created by the server's migrations), with files under `_vaults/<uuid>/` in the bucket, a prefix no
user name can have. `import <data-dir>` also copies a data directory's vaults.

To turn a user's database into a vault (for a user moving from a device token to an account), take the vault id
from the account server and run

```sh
./build/openvireo-sync-server migrate-to-vaults <user> <database-id> <vault-uuid>
```

It copies the database with its kept versions, revisions and ETags to `vaults/<vault-uuid>` and then deletes the
user's copy (devices of that user see it deleted). It refuses when the vault already exists and leaves everything as
it was if the database changes while it is copied. It works with either storage; with the data directory, stop the
server while it runs.

## Tests

```sh
cmake --build build && ctest --test-dir build --output-on-failure
```

`sync_server_test` covers the legacy API, `sync_server_jwt_test` and `sync_server_jwt_only_test` the account side in
`both` and `jwt` mode, and `sync_server_jwt_no_internal_test` `both` mode without `SYNC_INTERNAL_TOKEN`, against the
shared test vectors in `test/vectors/test-vectors.json` (a copy of OpenVireo's `docs/plans/accounts/test-vectors.json`).
