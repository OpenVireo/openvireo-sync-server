# Development

## Layout

| Path | What it is |
|---|---|
| `src/main.cpp` | Command line (`users`, `devices`, `migrate`, `import`, `sweep`, `migrate-to-vaults`) and server start-up |
| `src/Config.*` | The `SYNC_*` environment variables |
| `src/SyncApi.*` | Every HTTP route, preconditions, events (`EventHub`), CORS |
| `src/Jwt.*` | Access token verification and the JWKS cache |
| `src/Storage.*` | The storage interfaces (`UserRepository`, `DatabaseRepository`) and `openStorage` |
| `src/UserStore.*`, `src/DatabaseStore.*` | The data directory backend (SQLite and files) |
| `src/Postgres.*`, `src/Pg*Repository.*`, `src/BlobStore.*` | The Postgres and S3 backend (SigV4 signing, migrations, sweep) |
| `src/Import.*`, `src/Migrate.*` | `import` and `migrate-to-vaults` |
| `test/` | API tests and the shared account test vectors |
| `docs/` | These docs, the [OpenAPI description](openapi.yaml) and the [stateless design notes](stateless-design.md) |

## Building and testing

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build && ctest --test-dir build --output-on-failure
```

- `sync_server_test` covers the self-hosted API.
- `sync_server_jwt_test` and `sync_server_jwt_only_test` cover the account side in `both` and `jwt` mode, and
  `sync_server_jwt_no_internal_test` `both` mode without `SYNC_INTERNAL_TOKEN`, against the shared test vectors
  in `test/vectors/test-vectors.json` (a copy of OpenVireo's `docs/plans/accounts/test-vectors.json`).
- `docs_coverage` fails when a route registered in `src/SyncApi.cpp` is missing from `docs/openapi.yaml`, or an
  environment variable read in `src/Config.cpp` is missing from `docs/configuration.md`.

The API tests use the data directory, or Postgres and S3 when `SYNC_DATABASE_URL` and `SYNC_S3_*` are set. To run
them against both locally:

```sh
docker compose -f compose.cloud.yaml run --rm --build test
```

## Docs

The docs in `docs/` are Markdown, published on [openvireo.com](https://openvireo.com/docs/sync-server/) by the
site's `npm run docs:dev` from a release tag. Keep links between pages relative (`api.md#events`) so they work
on GitHub and on the site. A user-visible change (a route, a variable, a command) belongs in the same pull
request as its docs.

## Releasing

Push a version tag: `git tag v1.2.3 && git push origin v1.2.3`. `.github/workflows/release.yml` runs the API tests,
builds the image for linux/amd64 and linux/arm64, publishes it to GHCR as `:1.2.3`, `:1.2`, `:1` and `:latest`
(a pre-release only as itself), and creates the GitHub release.
