# HTTP API

Everything is under `/api/v1`. The machine-readable description is [openapi.yaml](openapi.yaml); this page
explains how the pieces fit together. Clients for it exist in OpenVireo (`src/gui/remote/`) and in the
[SDK](https://github.com/openvireo/openvireo-sdk) (`SyncClient`, `SelfHostedSync`).

## Authentication

Every route except `/health` and the pairing claim needs `Authorization: Bearer <token>`:

- on the **database routes**, the token of one of the user's devices (self-hosted, `SYNC_AUTH_MODE`
  `standalone` or `both`);
- on the **vault routes**, an account access token (a JWT, `SYNC_AUTH_MODE` `jwt` or `both`, see
  [Accounts and vaults](accounts.md));
- on the **internal routes**, `SYNC_INTERNAL_TOKEN`.

A missing or wrong token gets `401`. Routes the configured mode doesn't have get `404`.

## Routes

| Method | Path | Token | Description |
|---|---|---|---|
| `GET` | `/health` | none | `{"status": "ok"}` |
| `GET` | `/databases` | device | The user's databases: `{"databases": [Meta]}` |
| `GET` / `HEAD` | `/databases/{id}` | device | Download; `If-None-Match` gives `304` |
| `PUT` | `/databases/{id}` | device | Upload (raw `.kdbx` body), see [Concurrency](#concurrency) |
| `DELETE` | `/databases/{id}` | device | Delete it and its history (`204`); supports `If-Match` |
| `GET` | `/databases/{id}/history` | device | `{"versions": [Meta]}`, the current and kept versions, newest first |
| `GET` | `/databases/{id}/history/{revision}` | device | One of those versions |
| `POST` | `/devices/pairing` | device | A pairing code for another device of the same user |
| `POST` | `/devices/pairing/claim` | none | Trade a pairing code for a device token |
| `GET` | `/vaults` | access | `{"vaults": [Meta]}` for the token's vaults that exist |
| `GET` / `HEAD` | `/vaults/{vault}` | access, `read` | Download |
| `PUT` | `/vaults/{vault}` | access, `write` / `manage` | Update (`If-Match`, write) or create (`If-None-Match: *`, manage) |
| `DELETE` | `/vaults/{vault}` | access, `manage` | Delete it and its history (`204`) |
| `GET` | `/vaults/{vault}/history` | access, `read` | Kept versions |
| `GET` | `/vaults/{vault}/history/{revision}` | access, `read` | One kept version |
| `GET` | `/events` | device or access | Change notifications, see [Events](#events) |
| `POST` | `/internal/notify` | internal | Account server: push `access` events |
| `DELETE` | `/internal/vaults/{vault}` | internal | Account server: delete a vault |

`{id}` may contain letters, digits, `.`, `_` and `-`, up to 64 characters. `{vault}` is a lowercase UUID.

`Meta` is `{"id", "revision", "etag", "size", "updatedAt"}`: `revision` counts up from 1 with every upload,
`etag` is the quoted SHA-256 of the file, `size` is in bytes and `updatedAt` is an ISO 8601 time.

Downloads (`GET`, `HEAD`, history versions) send the file as `application/octet-stream` with the headers
`ETag`, `X-Database-Revision` and `Last-Modified`. Uploads answer with the new `Meta` as JSON and the same
headers. Every response has `Cache-Control: no-store`.

## Concurrency

Uploads must say which version they replace:

- `If-None-Match: *` creates the database. `412` if it already exists.
- `If-Match: "<etag>"` replaces exactly that version. `412` if another device uploaded in the meantime.
- A database upload with neither header is rejected with `428` when the database already exists (and creates
  it otherwise). A **vault** upload with neither is always `428`.

So two devices can never silently overwrite each other's changes. On `412` a client downloads the new version,
merges, and uploads again with the new ETag; OpenVireo and the SDK do this up to five times.

A download with `If-None-Match: "<etag>"` gets `304` and no body when nothing changed, which is how clients poll
cheaply. `DELETE` with `If-Match` deletes only that version (`412` otherwise).

Uploads must start with the KDBX file signature (`415` otherwise, `400` for a body too short to have one).
Uploads over `SYNC_MAX_DB_BYTES` get `413` with no body, because the upload is refused before it is read.

## History

Before each overwrite, the previous version is kept. The newest `SYNC_HISTORY_SIZE` of them stay, older ones are
deleted. `/history` lists the current version and the kept ones, newest first; `/history/{revision}`
downloads one. Nothing on the server changes when a client opens an old version: to restore it, the client
uploads it again as a new version.

In OpenVireo, **Database → Remote Sync → Versions on &lt;name&gt;…** lists them.

## Pairing

A device asks for a code for another device of the same user:

```http
POST /api/v1/devices/pairing
Authorization: Bearer <device token>

201 {"code": "WJDA-247W", "expiresAt": "2026-10-26T12:10:00Z"}
```

The new device trades it for a token of its own, without any token:

```http
POST /api/v1/devices/pairing/claim
Content-Type: application/json

{"code": "wjda247w", "deviceName": "Firefox on Linux"}

201 {"user": "alice", "deviceId": "…", "token": "…"}
```

A code is eight letters and digits (no `0`/`O` or `1`/`I`), shown as `ABCD-EFGH`; case, spaces and the dash
don't matter when claiming. It works once and for 10 minutes, and a user has at most 5 waiting (a new one replaces
the oldest). An unknown or expired code gets `400`. Wrong codes are limited to 10 per client address and 100 in
all per 10 minutes, after which claims get `429` with `Retry-After`. Behind a reverse proxy every client has the
proxy's address, so the per-address limit applies to everyone together. With several instances, each counts on
its own.

## Events

`GET /api/v1/events` stays open and sends [server-sent events](https://html.spec.whatwg.org/multipage/server-sent-events.html).
It starts with `retry: 5000`, and a comment line (`: ping`) every 25 seconds keeps idle connections open.

With a **device token**, every change to one of the user's databases sends

```
event: database
data: {"id":"personal","etag":"\"…\"","revision":12}
```

and a deleted database sends `{"id":"personal","deleted":true}`.

### Account events

With an **access token** the stream sends

```
event: vault
data: {"id":"<vault>","etag":"\"…\"","revision":3}        (or {"id":"<vault>","deleted":true})

event: access
data: {"change":"granted","vault":"<vault>"}               (vault may be null)
```

`vault` events only for the vaults in the token, `access` events when the account server reports an access
change for the token's subject (`change` is `granted`, `revoked`, `rotating`, `rotated` or `org`). The server
ends the stream once the token's `exp` has passed (checked every second). The client reconnects with a fresh
token, and does the same after any `access` event, since its vaults or roles may have changed.

OpenVireo listens while automatic sync is on, syncs as soon as another device uploads, and only polls every 5
minutes while it is connected (every minute otherwise). Behind a reverse proxy, make sure it doesn't buffer
responses (the server sends `X-Accel-Buffering: no` for nginx) and allows long-lived requests.

## CORS

A browser only lets a web app on another origin call the server when the server allows that origin. List the
origins in `SYNC_ALLOWED_ORIGINS`, separated by commas (no path or trailing slash). For those origins the server
answers preflight (`OPTIONS`) requests on every route with `204`, allowing `GET, HEAD, PUT, POST, DELETE` and the
headers `Authorization, Content-Type, If-Match, If-None-Match`, and every response carries
`Access-Control-Allow-Origin` and `Access-Control-Expose-Headers: ETag, X-Database-Revision, Last-Modified`, so the
app can read ETags. Tokens travel in the `Authorization` header, never in cookies, so credentials mode is not
used. With the variable unset (the default) the server sends no CORS headers at all.

**Using a self-hosted server from the hosted web app:** set `SYNC_ALLOWED_ORIGINS` to the origin the web app is
served from (the address in the browser's address bar, scheme and host only), keep `SYNC_AUTH_MODE` at
`standalone`, and pick *Use my own sync server* in the web app with your server's URL and a pairing code. The
server must be reachable over **HTTPS** (see [HTTPS and reverse proxies](install.md#https-and-reverse-proxies)): a
page served over HTTPS can't call a plain `http://` server, except one on `localhost`. A server on `localhost` or
the local network also works with Chrome's Private Network Access checks: preflights asking for it get
`Access-Control-Allow-Private-Network: true`. If a reverse proxy answers `OPTIONS` itself, let it pass those
requests through.

```sh
SYNC_ALLOWED_ORIGINS=https://app.example.org openvireo-sync-server
```

## Internal routes

Only the account server calls these, with `Authorization: Bearer <SYNC_INTERNAL_TOKEN>`. They exist only when
`SYNC_AUTH_MODE` is `jwt` or `both` and `SYNC_INTERNAL_TOKEN` is set (`404` otherwise); an access or device token
is never enough (`401`).

`POST /api/v1/internal/notify` sends an `access` event to every open stream of the listed subjects, on every
instance:

```json
{"subjects": ["user:<uuid>", "sa:<uuid>"], "event": "access", "data": {"vault": "<uuid or null>", "change": "revoked"}}
```

It answers `204`. Delivery is best effort: a client that misses it still loses access when its token expires.

`DELETE /api/v1/internal/vaults/{vault}` deletes the vault and its history exactly as a
`DELETE /api/v1/vaults/{vault}` with `manage` would, and the vault's streams get `{"id": "<vault>", "deleted": true}`.
It answers `204`, `404` when there is no such vault, and `400` for an id that isn't a lowercase UUID.

## Errors

Errors are JSON, `{"error": "<message for people>"}`. On the vault and internal routes, and on `/events` when the
server accepts access tokens, they also carry a machine-readable `code`. A `401` has
`WWW-Authenticate: Bearer realm="openvireo-sync"`.

| Status | `code` | When |
|---|---|---|
| `400` | `invalid_request` | Bad id, bad JSON, body too short to be a database |
| `401` | `unauthorized` | Missing, invalid or expired token |
| `403` | `forbidden` | The token doesn't grant the vault, the role is too low, or the wrong kind of token for the route |
| `404` | `not_found` | No such database, vault, version or route |
| `412` | `precondition_failed` | `If-Match` / `If-None-Match` didn't hold |
| `413` | *(no body)* | Upload over `SYNC_MAX_DB_BYTES` |
| `415` | `invalid_request` | The body isn't a KDBX file |
| `428` | `precondition_required` | Upload without `If-Match` or `If-None-Match` |
| `429` | `rate_limited` | Too many wrong pairing codes; see `Retry-After` |
| `5xx` | `internal_error` | Something went wrong on the server; retry later |
