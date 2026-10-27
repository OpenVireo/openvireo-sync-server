# Installing and deploying

## Release binary

Each [release](https://github.com/OpenVireo/openvireo-sync-server/releases) has the server as one binary that
needs nothing installed, with Postgres and S3 support, for:

| Platform | Archive |
|---|---|
| Linux x64, arm64 (any distribution, statically linked) | `openvireo-sync-server-VERSION-linux-x64.tar.gz`, `…-linux-arm64.tar.gz` |
| macOS 13.3 or later, Apple silicon and Intel | `openvireo-sync-server-VERSION-darwin-arm64.tar.gz`, `…-darwin-x64.tar.gz` |
| Windows 10, 11, Server 2016 or later, x64 | `openvireo-sync-server-VERSION-windows-x64.zip` |

Each archive has a directory `openvireo-sync-server-VERSION` with the binary, the licence, the README and these
docs. `SHA256SUMS` on the release lists their checksums, and `SHA256SUMS.asc` is its signature with the OpenVireo
release key, the same one as the desktop app's releases: fingerprint
`23F6 403E 9392 E946 475E 7FD3 B556 83A4 3E98 C11C`. Import it once, then `gpg --verify SHA256SUMS.asc SHA256SUMS`
must say "Good signature" with that primary key fingerprint (see
[Verifying Signatures](https://openvireo.com/verifying-signatures)):

```sh
curl -sSL https://openvireo.com/openvireo-release.asc | gpg --import
# or: gpg --keyserver hkps://keys.openpgp.org --recv-keys 23F6403E9392E946475E7FD3B55683A43E98C11C
```
 For an always-on server, Linux (with the systemd unit
below) or [Docker](#docker) is the best supported; on macOS and Windows the binary is as good for a server that
runs while you are logged in, for trying things out and for the `users` and `devices` commands.

### Linux

```sh
v=VERSION a=x64     # VERSION: the release without the v, e.g. 0.2.0; a: x64 or arm64
curl -LO "https://github.com/OpenVireo/openvireo-sync-server/releases/download/v$v/openvireo-sync-server-$v-linux-$a.tar.gz"
curl -LO "https://github.com/OpenVireo/openvireo-sync-server/releases/download/v$v/SHA256SUMS"
curl -LO "https://github.com/OpenVireo/openvireo-sync-server/releases/download/v$v/SHA256SUMS.asc"
gpg --verify SHA256SUMS.asc SHA256SUMS
sha256sum --ignore-missing -c SHA256SUMS
tar -xzf "openvireo-sync-server-$v-linux-$a.tar.gz" && cd "openvireo-sync-server-$v"
sudo install openvireo-sync-server /usr/local/bin/
```

The archive also has a systemd unit, `openvireo-sync-server.service`, which runs the server as the user
`openvireo-sync` with its data in `/var/lib/openvireo-sync-server` and reads settings from
`/etc/openvireo-sync-server.env`:

```sh
sudo useradd --system --home-dir /var/lib/openvireo-sync-server --shell /usr/sbin/nologin openvireo-sync
sudo install -m 0644 openvireo-sync-server.service /etc/systemd/system/
echo SYNC_BEHIND_PROXY=1 | sudo tee /etc/openvireo-sync-server.env    # see Configuration
sudo systemctl enable --now openvireo-sync-server
sudo -u openvireo-sync env SYNC_DATA_DIR=/var/lib/openvireo-sync-server openvireo-sync-server users add alice laptop
```

For HTTPS connections (the account server's JWKS, S3) it uses the distribution's CA certificates in
`/etc/ssl` (the `ca-certificates` package). It's built by `Dockerfile.static`: `docker build -f Dockerfile.static -o dist .`

### macOS

```sh
v=VERSION a=arm64   # a: arm64 (Apple silicon) or x64 (Intel)
curl -LO "https://github.com/OpenVireo/openvireo-sync-server/releases/download/v$v/openvireo-sync-server-$v-darwin-$a.tar.gz"
curl -LO "https://github.com/OpenVireo/openvireo-sync-server/releases/download/v$v/SHA256SUMS"
curl -LO "https://github.com/OpenVireo/openvireo-sync-server/releases/download/v$v/SHA256SUMS.asc"
gpg --verify SHA256SUMS.asc SHA256SUMS    # gpg from GPG Suite or brew install gnupg
shasum -a 256 --ignore-missing -c SHA256SUMS
tar -xzf "openvireo-sync-server-$v-darwin-$a.tar.gz" && cd "openvireo-sync-server-$v"
./openvireo-sync-server users add alice laptop
./openvireo-sync-server                # http://127.0.0.1:8787, data in ./data
```

The binary is signed with OpenVireo's Developer ID (team `DP5U2UP42V`) and notarized by Apple, so it runs even
when downloaded with a browser. A bare executable can't carry its notarization ticket, so macOS checks it online
the first time. It uses only libraries that are part of macOS. To check the signature by hand:

```sh
codesign --verify --strict --verbose=2 openvireo-sync-server   # valid on disk, satisfies its Designated Requirement
codesign -dv openvireo-sync-server 2>&1 | grep TeamIdentifier  # TeamIdentifier=DP5U2UP42V
spctl --assess --type install -vv openvireo-sync-server         # accepted, source=Notarized Developer ID
```

For HTTPS connections it trusts the CA certificates in `/etc/ssl/cert.pem`, the bundle macOS ships and updates
with the system. Certificates you added to the Keychain are not used: for a private CA (say, of your own account
server) set `SSL_CERT_FILE` to a PEM file with that CA and, if needed, the public ones (e.g. a copy of
`/etc/ssl/cert.pem` with your CA appended). To keep it running, use a launchd job (a LaunchAgent or LaunchDaemon
with `SYNC_DATA_DIR` in its `EnvironmentVariables`), or Docker.

### Windows

In PowerShell:

```powershell
$v = "VERSION"
$base = "https://github.com/OpenVireo/openvireo-sync-server/releases/download/v$v"
Invoke-WebRequest "$base/openvireo-sync-server-$v-windows-x64.zip" -OutFile "openvireo-sync-server-$v-windows-x64.zip"
Invoke-WebRequest "$base/SHA256SUMS" -OutFile SHA256SUMS
Invoke-WebRequest "$base/SHA256SUMS.asc" -OutFile SHA256SUMS.asc
gpg --verify SHA256SUMS.asc SHA256SUMS       # gpg from Gpg4win
(Get-FileHash "openvireo-sync-server-$v-windows-x64.zip").Hash   # compare with its line in SHA256SUMS
Expand-Archive "openvireo-sync-server-$v-windows-x64.zip" . ; cd "openvireo-sync-server-$v"
.\openvireo-sync-server.exe users add alice laptop
.\openvireo-sync-server.exe           # http://127.0.0.1:8787, data in .\data
```

The binary is signed with Azure Artifact Signing, the same publisher as the OpenVireo desktop app's installers,
so SmartScreen knows its publisher. To check the signature by hand (`Status` must be `Valid`):

```powershell
Get-AuthenticodeSignature .\openvireo-sync-server.exe | Format-List Status, SignerCertificate, TimeStamperCertificate
```

Windows Firewall asks for permission only if you make it listen on the network
(`SYNC_HOST`). It needs no runtime or DLLs beyond Windows itself.

Windows has no file modes: the files in the data directory inherit the directory's access control list, so keep
`SYNC_DATA_DIR` somewhere only you or the server's account can read, such as a folder in your user profile. For
HTTPS connections it trusts the certificates in Windows' *Trusted Root Certification Authorities* store; add a
private CA there (Windows ignores `SSL_CERT_FILE`). Windows installs some public roots only when a Windows
program first needs them, so if a fetch fails with `Invalid certificate` for a public server, opening its URL
once in Edge installs the missing root.

It's a console program, not a Windows service, so `sc.exe create` can't run it. To keep it running without a
logged-in user, wrap it with a service wrapper such as [WinSW](https://github.com/winsw/winsw) or
[NSSM](https://nssm.cc) and set `SYNC_DATA_DIR` there, or run it in Docker or WSL.

### All platforms

The binary ignores the system's `openssl.cnf`, which belongs to another OpenSSL (the distribution's, macOS's
LibreSSL); set `OPENSSL_CONF` to use one anyway. The macOS and Windows binaries are built and signed by
`.github/workflows/binaries-native.yml` (see `.github/RELEASE-SIGNING.md`); see [From source](#from-source) to build one yourself.

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
release binaries do.

### With vcpkg (macOS and Windows)

The release binaries for macOS and Windows link static libraries from [vcpkg](https://vcpkg.io): the manifest
in `packaging/vcpkg` pins the versions (Drogon with PostgreSQL, OpenSSL, libpq, SQLite) and has triplets for
release-only static builds. You need CMake, a C++17 compiler (Xcode's command line tools, or Visual Studio 2022
or later with *Desktop development with C++*), Git, Python 3.10 or later (vcpkg builds libpq with Meson) and a
clone of vcpkg:

```sh
git clone https://github.com/microsoft/vcpkg && ./vcpkg/bootstrap-vcpkg.sh    # bootstrap-vcpkg.bat on Windows
export VCPKG_ROOT=$PWD/vcpkg
```

On macOS (`arm64-osx-release-static` on Apple silicon, `x64-osx-release-static` on Intel):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_MANIFEST_DIR="$PWD/packaging/vcpkg" -DVCPKG_TARGET_TRIPLET=arm64-osx-release-static \
  -DSYNC_SERVER_WITH_CLOUD=ON -DSYNC_SERVER_PORTABLE=ON -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3 \
  "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local"
cmake --build build && ctest --test-dir build
```

`CMAKE_IGNORE_PREFIX_PATH` keeps Homebrew's libraries out of the binary. On Windows, in PowerShell:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_MANIFEST_DIR="$PWD/packaging/vcpkg" -DVCPKG_TARGET_TRIPLET=x64-windows-release-static `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DSYNC_SERVER_WITH_CLOUD=ON -DSYNC_SERVER_PORTABLE=ON
cmake --build build --config Release; ctest --test-dir build -C Release
```

The first configure builds the libraries, which takes a while; vcpkg caches them for the next. The binary is
`build/openvireo-sync-server` (on Windows `build\Release\openvireo-sync-server.exe`).

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
