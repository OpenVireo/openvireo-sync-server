# Installing and deploying

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
