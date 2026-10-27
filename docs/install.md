# Installing and deploying

## Release binary

Each [release](https://github.com/OpenVireo/openvireo-sync-server/releases) has the server as one statically
linked binary for Linux x64 and arm64. It runs on any distribution, needs nothing installed, and includes
Postgres and S3 support:

```sh
v=VERSION a=x64     # VERSION: the release without the v, e.g. 0.2.0; a: x64 or arm64
curl -LO "https://github.com/OpenVireo/openvireo-sync-server/releases/download/v$v/openvireo-sync-server-$v-linux-$a.tar.gz"
curl -LO "https://github.com/OpenVireo/openvireo-sync-server/releases/download/v$v/SHA256SUMS"
sha256sum --ignore-missing -c SHA256SUMS
tar -xzf "openvireo-sync-server-$v-linux-$a.tar.gz" && cd "openvireo-sync-server-$v"
sudo install openvireo-sync-server /usr/local/bin/
```

The archive also has the docs and a systemd unit, `openvireo-sync-server.service`, which runs the server as
the user `openvireo-sync` with its data in `/var/lib/openvireo-sync-server` and reads settings from
`/etc/openvireo-sync-server.env`:

```sh
sudo useradd --system --home-dir /var/lib/openvireo-sync-server --shell /usr/sbin/nologin openvireo-sync
sudo install -m 0644 openvireo-sync-server.service /etc/systemd/system/
echo SYNC_BEHIND_PROXY=1 | sudo tee /etc/openvireo-sync-server.env    # see Configuration
sudo systemctl enable --now openvireo-sync-server
sudo -u openvireo-sync env SYNC_DATA_DIR=/var/lib/openvireo-sync-server openvireo-sync-server users add alice laptop
```

For HTTPS connections (the account server's JWKS, S3) it uses the distribution's CA certificates in
`/etc/ssl` (the `ca-certificates` package). It ignores the system's `openssl.cnf`, which belongs to the
distribution's own OpenSSL; set `OPENSSL_CONF` to use one anyway. It's built by `Dockerfile.static`:
`docker build -f Dockerfile.static -o dist .`

## From source

The server is C++17 on [Drogon](https://github.com/drogonframework/drogon), with SQLite for the default
storage.

```sh
brew install drogon sqlite     # or: apt install libdrogon-dev libsqlite3-dev, vcpkg install drogon sqlite3, …
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/openvireo-sync-server  # http://127.0.0.1:8787, data in ./data
```

Postgres and S3 support (see [Storage](configuration.md#storage)) needs Drogon built with PostgreSQL
(`-DBUILD_POSTGRESQL=ON`), which Homebrew's and most distributions' packages are not. CMake turns it on
when it finds such a Drogon; `-DSYNC_SERVER_WITH_CLOUD=ON` makes it required. The Docker image always has it.

`cmake --install build` installs the single binary, `openvireo-sync-server`. It has no other files.
`-DSYNC_SERVER_PORTABLE=ON` makes it ignore the system's `openssl.cnf` unless `OPENSSL_CONF` is set, as the
release binary does.

## Docker

Released images for linux/amd64 and linux/arm64 are on GHCR:

```sh
docker run -d --name openvireo-sync -p 127.0.0.1:8787:8787 -v openvireo-sync:/data \
  -e SYNC_BEHIND_PROXY=1 ghcr.io/openvireo/openvireo-sync-server:1
docker exec openvireo-sync openvireo-sync-server users add alice laptop
```

Tags: `:latest`, `:1`, `:1.2`, `:1.2.3` (a pre-release such as `1.3.0-rc.1` gets only its own tag). The image
runs as an unprivileged user (uid 10001), listens on `0.0.0.0:8787` and keeps the data directory in `/data`; a
bind-mounted directory must be writable by that uid. Publish the port on `127.0.0.1` only and put HTTPS in
front of it (see [below](#https-and-reverse-proxies)); `SYNC_BEHIND_PROXY=1` says you did.

To build from this repository instead:

```sh
docker compose up -d --build                                  # data directory in a volume, on 127.0.0.1:8787
docker compose run --rm sync-server users add alice laptop
```

`SYNC_HOST_PORT=18787` publishes on another port.

`compose.cloud.yaml` runs the stateless setup locally: Postgres, an S3 gateway standing in for Spaces, two
server instances and nginx in front of them:

```sh
docker compose -f compose.cloud.yaml up -d --build
docker compose -f compose.cloud.yaml run --rm migrate users add alice laptop
docker compose -f compose.cloud.yaml run --rm --build test    # the API tests against Postgres and the bucket
```

## DigitalOcean App Platform

`.do/app.yaml` deploys two instances from the Dockerfile with a managed Postgres database and a Spaces
bucket. Create the bucket and a Spaces access key first, fill in the placeholders, then:

```sh
doctl apps create --spec .do/app.yaml
```

App Platform terminates HTTPS. Add users from the app's console (`openvireo-sync-server users add alice
laptop`) or from any machine with the same `SYNC_*` settings.

## HTTPS and reverse proxies

The access token travels in the `Authorization` header, so **use HTTPS** for anything other than localhost.
Keep the server on `127.0.0.1` and put a TLS-terminating reverse proxy (Caddy, nginx, …) in front of it. For
example, with Caddy:

```
sync.example.com {
    reverse_proxy 127.0.0.1:8787
}
```

With nginx, turn off buffering for the events stream and allow long-lived requests:

```nginx
location / {
    proxy_pass http://127.0.0.1:8787;
    proxy_http_version 1.1;
    proxy_set_header Connection "";
    proxy_buffering off;          # the server also sends X-Accel-Buffering: no
    proxy_read_timeout 1h;        # events streams stay open; the server pings every 25 seconds
    client_max_body_size 50m;     # at least SYNC_MAX_DB_BYTES
}
```

Things to keep in mind behind a proxy:

- The server warns when it listens on a non-loopback address; `SYNC_BEHIND_PROXY=1` silences that once TLS is
  terminated in front of it.
- OpenVireo does not follow redirects, so the token is never forwarded to another host. Enter the final
  `https://` URL.
- The pairing rate limit is per client address, and behind a proxy every client has the proxy's address
  (see [Pairing](api.md#pairing)).
- If web apps call the server (see [CORS](api.md#cors)), let the proxy pass `OPTIONS` requests through
  rather than answering them itself.
