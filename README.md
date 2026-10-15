# KeePassXC Sync Server

A small REST server that stores KeePassXC databases so several devices can stay in sync.

The server only ever sees the **encrypted** `.kdbx` file. KeePassXC downloads it, decrypts and merges it locally, then uploads the merged result. Your master password and keys never leave the client.

## Quick start

The server is written in C++17 on [Drogon](https://github.com/drogonframework/drogon).

```sh
brew install drogon            # or: apt install libdrogon-dev, vcpkg install drogon, …
cd sync_server
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/keepassxc-sync-server users add alice laptop   # prints laptop's access token, shown only once
./build/keepassxc-sync-server                   # http://127.0.0.1:8787
```

Then in KeePassXC:

1. Open your database and go to **Database → Database Settings → Remote Sync**.
2. Enter a name, set **Type** to *KeePassXC Sync Server*, and fill in:
   - **Server URL**: `http://127.0.0.1:8787`, or your `https://` URL
   - **Database ID**: any name, e.g. `personal`
   - **Access token**: the token printed for this device. It is kept on this device only (in the Keychain on
     macOS, the Credential Manager on Windows, the desktop keyring on Linux), not in the database
3. Click **Test Connection**, then **Save**, then **OK**.
4. Use **Database → Remote Sync → <name>** to sync.

The first sync uploads your local database. Later syncs download the server copy, merge it with your local database (the same merge as *Merge from Database*), save it locally, and upload the result.

Other devices use the same Database ID. If a device's copy uses a different password or key file, KeePassXC asks you to unlock the server copy before merging.

### Setting up another device

Every device has its own access token, so one lost device can be shut out without touching the others. The easiest
way to give the new device one is a pairing code: on a device that already syncs, open **Remote Sync** in the
database settings, select the sync server and click **Pair Device…**. Or on the server:

```sh
./build/keepassxc-sync-server devices pair alice   # prints a code such as WJDA-247W
```

On the new device, enter the code where KeePassXC asks for the access token. It trades the code for a token of its
own, named after the computer. A code adds one device and works for 10 minutes. If you'd rather copy a token by hand,
`devices add alice phone` prints one.

To get the database onto a new device:

1. Use **Database → Import…** and choose *KeePassXC Sync Server (.kdbx)*.
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
| `SYNC_DATA_DIR` | `./data` | Where users and databases are stored (relative to the working directory) |
| `SYNC_MAX_DB_BYTES` | `52428800` | Upload size limit (50 MiB) |
| `SYNC_HISTORY_SIZE` | `10` | How many previous versions of each database to keep |
| `SYNC_THREADS` | `0` | Number of I/O threads, `0` means one per CPU core |

### Deploying beyond localhost

The access token travels in the `Authorization` header, so **use HTTPS** for anything other than localhost. Keep the server on `127.0.0.1` and put a TLS-terminating reverse proxy (Caddy, nginx, …) in front of it. For example, with Caddy:

```
sync.example.com {
    reverse_proxy 127.0.0.1:8787
}
```

KeePassXC does not follow redirects, so the token is never forwarded to another host. Enter the final `https://` URL.

## Users and devices

```sh
./build/keepassxc-sync-server users add <name> [device-name]     # create a user with a first device
./build/keepassxc-sync-server users remove <name>
./build/keepassxc-sync-server users list

./build/keepassxc-sync-server devices add <user> <device-name>   # a new device and its access token
./build/keepassxc-sync-server devices pair <user>                # a one-time code a new device trades for a token
./build/keepassxc-sync-server devices list <user>                # ids, names, when each was last seen
./build/keepassxc-sync-server devices revoke <user> <device-id>  # its token stops working at once
```

Each user has their own namespace and cannot see other users' databases. All of a user's devices see the same
databases. Only SHA-256 hashes of tokens and pairing codes are stored, in `data/users.json`. Changes to that file take effect without a
restart.

A device was lost? Revoke it. Its token can no longer reach the server. If whoever has it may also know the
database's password, change that password too, so copies uploaded from now on can't be opened with the old one.

### Upgrading from one token per user

Tokens from before devices existed keep working, as a device called `legacy` that all your devices share. Older
KeePassXC versions also stored that token inside the database. To move to per-device tokens:

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

This means two devices can never silently overwrite each other's changes. When KeePassXC gets a `412`, your local changes are already saved. Sync again to merge in the other device's changes.

Uploads must start with the KDBX file signature (`415` otherwise). Uploads over the size limit get `413`; that response has no body because the upload is refused before it is read.

### History

Before each overwrite, the previous version is copied to `data/databases/<user>/.history/<id>/<revision>.kdbx`, with
its metadata next to it in `<revision>.json`. The newest `SYNC_HISTORY_SIZE` versions are kept.

In KeePassXC, **Database → Remote Sync → Versions on <name>…** lists them. Opening one downloads it and shows it in
its own tab, where you can copy entries from it or merge it into your database. Nothing on the server changes.

### Change notifications

`GET /api/v1/events` stays open and sends a server-sent event to all of a user's connected devices after every change
to one of their databases:

```
event: database
data: {"id":"personal","etag":"\"…\"","revision":12}
```

A deleted database sends `{"id":"personal","deleted":true}`. A comment line (`: ping`) every 25 seconds keeps idle
connections open. KeePassXC listens while automatic sync is on, syncs as soon as another device uploads, and only polls
every 5 minutes while it is connected (every minute otherwise). Behind a reverse proxy, make sure it doesn't buffer
responses (the server sends `X-Accel-Buffering: no` for nginx) and allows long-lived requests.

## Tests

```sh
cmake --build build && ./build/sync_server_test     # or: ctest --test-dir build
```
