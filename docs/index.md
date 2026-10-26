# OpenVireo Sync Server

A small REST server that stores OpenVireo databases so several devices can stay in sync. It runs on your
own machine or server (self-hosted, with a token per device), or as the storage behind OpenVireo accounts
(hosted, with access tokens from the account server).

The server only ever sees the **encrypted** `.kdbx` file. OpenVireo downloads it, decrypts and merges it
locally, then uploads the merged result. Your master password and keys never leave the client.

The client side lives in [OpenVireo](https://github.com/openvireo/openvireo) (`src/gui/remote/`), the web app
and the [SDK](https://github.com/openvireo/openvireo-sdk). OpenVireo is a fork of
[KeePassXC](https://keepassxc.org); this server is not part of or endorsed by the KeePassXC project.

## How sync works

1. A device downloads the server's copy with `GET`, together with its `ETag` (the SHA-256 of the file).
2. It merges that copy into its local database (the same merge as *Merge from Database*) and saves locally.
3. It uploads the result with `If-Match: "<etag>"`. If another device uploaded in the meantime, the server
   answers `412` and the device starts again at step 1, so two devices can never overwrite each other.
4. The server keeps the previous versions, and tells every other connected device about the new one over
   a server-sent events stream, so they sync right away instead of waiting for their next poll.

The server never parses the database beyond checking its file signature. It can't read entries or merge
anything itself.

## Quick start

The server is written in C++17 on [Drogon](https://github.com/drogonframework/drogon).

```sh
brew install drogon sqlite     # or: apt install libdrogon-dev libsqlite3-dev, vcpkg install drogon sqlite3, …
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/openvireo-sync-server users add alice laptop   # prints laptop's access token, shown only once
./build/openvireo-sync-server                          # http://127.0.0.1:8787
```

Or with Docker, see [Installing and deploying](install.md).

Then in OpenVireo:

1. Open your database and go to **Database → Database Settings → Remote Sync**.
2. Enter a name, set **Type** to *OpenVireo Sync Server*, and fill in:
   - **Server URL**: `http://127.0.0.1:8787`, or your `https://` URL
   - **Database ID**: any name, e.g. `personal`
   - **Access token**: the token printed for this device. It is kept on this device only (in the Keychain on
     macOS, the Credential Manager on Windows, the desktop keyring on Linux), not in the database
3. Click **Test Connection**, then **Save**, then **OK**.
4. Use **Database → Remote Sync → &lt;name&gt;** to sync.

The first sync uploads your local database. Later syncs download the server copy, merge it with your local
database, save it locally, and upload the result. Other devices use the same Database ID; see
[Users and devices](users-and-devices.md) for adding them.

## Contents

- [Installing and deploying](install.md): from source, Docker and GHCR images, DigitalOcean, HTTPS
- [Configuration](configuration.md): every `SYNC_*` variable, and the two storage backends
- [Users and devices](users-and-devices.md): the command line, pairing, revoking a lost device
- [Accounts and vaults](accounts.md): running the server behind an OpenVireo account server
- [HTTP API](api.md): routes, concurrency, history, events, CORS and errors, with the
  [OpenAPI description](openapi.yaml)
- [Operations](operations.md): backups, upgrades, scaling, monitoring
- [Development](development.md): building, tests, releases

## License

See [LICENSE](../LICENSE).
