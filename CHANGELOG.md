# Changelog

What changed in each release of the OpenVireo sync server. Releases are 0.x while the API settles, so a
minor release can change it; the notes say when one does.

## Unreleased

## 0.3.0

2026-11-24

- **Added** `SYNC_HSTS`: send `Strict-Transport-Security`.
- Web apps can read the `Date` header (CORS), for clock-skew warnings.
- Account deletion events pass through `/internal/notify`.
- Container images are on `ghcr.io/openvireo`.
- Docs moved to [developer.openvireo.com](https://developer.openvireo.com/self-hosting/), and the quick start
  leads with the release binary.

## 0.2.0

2026-10-29

- Signed macOS and Windows binaries, and a static Linux binary with a systemd unit.
- A vault size quota from the sync token.
- `DELETE /internal/vaults/{vault}` for the account server.
- The whole API in an OpenAPI description, and the README split into docs pages.

## 0.1.0

2026-10-24

The first release.

- A REST sync server for `.kdbx` files, written in C++ on Drogon.
- An access token per device, and one-time codes to pair new devices.
- Changes pushed to devices, and older versions to open.
- Account mode: JWT access tokens, vaults, account event streams and CORS.
- Stateless on Postgres and S3, or a data directory.
- Multi-arch Docker images.
