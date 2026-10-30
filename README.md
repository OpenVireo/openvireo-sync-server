# OpenVireo Sync Server

A small REST server that stores OpenVireo databases so several devices can stay in sync.

The client side lives in [OpenVireo](https://github.com/openvireo/openvireo) (`src/gui/remote/`). OpenVireo is a fork of
[KeePassXC](https://keepassxc.org); this server is not part of or endorsed by the KeePassXC project.

The server only ever sees the **encrypted** `.kdbx` file. OpenVireo downloads it, decrypts and merges it locally, then uploads the merged result. Your master password and keys never leave the client.

## Quick start

Download the server from the [releases](https://github.com/OpenVireo/openvireo-sync-server/releases): one signed
binary that needs nothing else installed, for Linux (x64, arm64), macOS (Apple silicon, Intel) and Windows (x64).
On Linux, for example:

```sh
v=VERSION a=x64     # VERSION: the release without the v, e.g. 0.2.0; a: x64 or arm64
curl -fLO "https://github.com/OpenVireo/openvireo-sync-server/releases/download/v$v/openvireo-sync-server-$v-linux-$a.tar.gz"
tar -xzf "openvireo-sync-server-$v-linux-$a.tar.gz" && cd "openvireo-sync-server-$v"
./openvireo-sync-server users add alice laptop   # prints laptop's access token, shown only once
./openvireo-sync-server                          # http://127.0.0.1:8787
```

See [Release binary](docs/install.md#release-binary) for macOS and Windows, checking the signatures and a systemd unit.

To build from source, with C++17 and [Drogon](https://github.com/drogonframework/drogon) (more in
[From source](docs/install.md#from-source)):

```sh
brew install drogon sqlite     # or: apt install libdrogon-dev libsqlite3-dev, vcpkg install drogon sqlite3, …
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/openvireo-sync-server users add alice laptop
./build/openvireo-sync-server
```

Or use Docker:

```sh
docker run -d --name openvireo-sync -p 127.0.0.1:8787:8787 -v openvireo-sync:/data \
  -e SYNC_BEHIND_PROXY=1 ghcr.io/openvireo/openvireo-sync-server:1
docker exec openvireo-sync openvireo-sync-server users add alice laptop
```

Then in OpenVireo, open your database, go to **Database → Database Settings → Remote Sync**, add an *OpenVireo Sync
Server* with the server URL, a Database ID such as `personal` and the access token, and use **Database → Remote Sync**
to sync. Put HTTPS in front of the server before using it beyond localhost.

## Documentation

The docs are in [`docs/`](docs/index.md) and on [openvireo.com/docs/sync-server](https://openvireo.com/docs/sync-server/):

- [Overview](docs/index.md): how sync works, the quick start in full
- [Installing and deploying](docs/install.md): release binaries, from source, Docker and GHCR images, DigitalOcean, HTTPS
- [Configuration](docs/configuration.md): every `SYNC_*` variable, and the two storage backends
- [Users and devices](docs/users-and-devices.md): the command line, pairing, revoking a lost device
- [Accounts and vaults](docs/accounts.md): running the server behind an OpenVireo account server
- [HTTP API](docs/api.md) and its [OpenAPI description](docs/openapi.yaml)
- [Operations](docs/operations.md): backups, upgrades, scaling, monitoring
- [Development](docs/development.md): layout, tests, releases

## Tests

```sh
cmake --build build && ctest --test-dir build --output-on-failure
```
