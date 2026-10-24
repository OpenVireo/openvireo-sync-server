# Stateless Sync Server — Plan

**Status:** Implemented (in OpenVireo's `feature/auto-sync`, before the server moved to this repository), except as noted in 7
**Branch target:** `develop`
**Last updated:** 2026-10-16

## 1. Goal

Make the sync server run in a Docker container that keeps nothing on its own disk, with several instances behind a load
balancer. The target deployment is DigitalOcean App Platform with a Managed PostgreSQL database and a Spaces bucket.
Deploys use prebuilt images published from GitHub, so nothing is compiled at deploy time.

The guarantees stay the same: the server only stores encrypted `.kdbx` files and never decrypts them, conditional
uploads let exactly one of two simultaneous writers win, and a bounded history of previous versions is kept.

The filesystem mode stays as the default for self-hosting and development. The HTTP API doesn't change, so OpenVireo
needs no changes.

### Why stay in C++

A rewrite in Go was considered (an earlier version of this plan). Go would have made the new storage code simpler:
mature S3 and Postgres libraries, and blocking calls that don't tie up an event loop. Against that, it meant
rewriting a tested server and proving parity before removing this one. Prebuilt images take away the main cost of
C++, compiling Drogon and its dependencies, and also make a large dependency like the AWS SDK cheap. What remains
(blocking I/O threads) is fine at this server's scale, and C++20 coroutines are the way out later (phase 7).

## 2. What ties the server to one disk and one process today

| State | Where | Problem when stateless or with several instances |
|---|---|---|
| Users, devices, token hashes, pairing codes | `<dataDir>/users.json`, `UserStore` | Lost on redeploy. The CLI (`users`, `devices`) edits a file that a one-off container doesn't share with the running server. |
| Database blobs and metadata | `<dataDir>/databases/<user>/<id>.kdbx` and `.meta.json`, `DatabaseStore` | Lost on redeploy. |
| History | `<dataDir>/databases/<user>/.history/<id>/<rev>.kdbx` and `.json` | Lost on redeploy. |
| Atomic check-and-commit | An in-process `std::mutex` per database (`DatabaseStore::lockFor`) | Two instances can both pass the `If-Match` check and both commit. |
| Events (`GET /api/v1/events`) | `EventHub` in `SyncApi.cpp` keeps open streams in memory | A device only hears about changes uploaded through the same instance. |
| Pairing rate limit | `FailureLimiter` in `SyncApi.cpp`, in memory | Per instance: with N instances the global limit becomes up to N × 100 wrong codes per 10 minutes. Acceptable for codes that expire after 10 minutes; a shared limit is a follow-up (6). |
| Configuration | `SYNC_*` environment variables (`Config::fromEnvironment`) | Already fine for containers. |

## 3. Design

Postgres holds all state and decides every commit. The bucket only holds blob bytes, under keys that are never
overwritten. Nothing beyond plain PUT, GET, DELETE and LIST is needed from the bucket, so the design doesn't depend on
S3 conditional writes, which not every S3-compatible store supports.

### 3.1 Storage interfaces

Two interfaces go between `SyncApi` and storage, each with a filesystem implementation (today's code) and a Postgres
implementation:

- `UserRepository`: the current public interface of `UserStore` (authenticate, users, devices, pairing codes).
- `DatabaseRepository`: the current public interface of `DatabaseStore` (`meta`, `read`, `list`, `write` with a
  `Precondition`, `remove`, `history`, `readRevision`).

`SyncApi` and `main.cpp` pick the implementations from the configuration (3.6). The filesystem classes keep their
names and behaviour.

The interfaces stay synchronous, like the handlers that call them. The Postgres implementation uses a normal (not
"fast") `drogon::orm::DbClient` with `execSqlSync`, and the S3 client's synchronous calls. Both block the calling I/O
thread without deadlocking. Raise `SYNC_THREADS` above the core count if that becomes the bottleneck; phase 7 removes
the blocking.

### 3.2 Schema

```sql
CREATE TABLE users (
    name        text PRIMARY KEY,
    created_at  timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE devices (
    id            text PRIMARY KEY,
    user_name     text NOT NULL REFERENCES users(name) ON DELETE CASCADE,
    name          text NOT NULL,
    token_hash    text NOT NULL UNIQUE,           -- SHA-256 hex, as in users.json
    created_at    timestamptz NOT NULL DEFAULT now(),
    last_seen_at  timestamptz
);

CREATE TABLE pairing_codes (
    code        text PRIMARY KEY,                 -- normalised, as UserStore stores it
    user_name   text NOT NULL REFERENCES users(name) ON DELETE CASCADE,
    created_by  text,
    created_at  timestamptz NOT NULL DEFAULT now(),
    expires_at  timestamptz NOT NULL
);

CREATE TABLE databases (
    user_name   text NOT NULL,                    -- no foreign key: removing a user keeps their databases, as today
    id          text NOT NULL,
    revision    bigint NOT NULL,
    etag        text NOT NULL,                    -- quoted SHA-256, as today
    size        bigint NOT NULL,
    blob_key    text NOT NULL,
    updated_at  timestamptz NOT NULL,
    PRIMARY KEY (user_name, id)
);

CREATE TABLE versions (                         -- kept previous versions
    user_name   text NOT NULL,
    id          text NOT NULL,
    revision    bigint NOT NULL,
    etag        text NOT NULL,
    size        bigint NOT NULL,
    blob_key    text NOT NULL,
    updated_at  timestamptz NOT NULL,
    PRIMARY KEY (user_name, id, revision)
);
```

Migrations are numbered SQL files compiled into the binary. `migrate`, and the server on startup, take a Postgres
advisory lock, apply the missing ones and record them in `schema_migrations`, so instances starting together don't
race.

- **Authentication** looks a device up by `token_hash`. Today every hash is compared in constant time because they
  are all in memory. An indexed lookup by the SHA-256 of a 256-bit random token leaks nothing useful.
- **`last_seen_at`** keeps today's throttle: it is only written when older than `lastSeenInterval` (one hour), so
  authenticating stays a read most of the time.
- **Claiming a pairing code** is `DELETE FROM pairing_codes WHERE code = $1 AND expires_at > now() RETURNING
  user_name`, then the device insert, in one transaction, so two devices can't claim the same code. Creating a code
  deletes the user's oldest beyond `MaxPairingsPerUser`, as today.
- **Legacy tokens** (one `tokenHash` per user in old `users.json` files) only exist in the filesystem implementation.
  `import` (3.7) turns them into a device called "legacy", as `UserStore` already does when it reads them.

### 3.3 Blobs

Blobs go into the bucket under `<prefix><user>/<id>/<sha256>.kdbx`, where the hash is the ETag without quotes. The key
depends only on the content, so a key never gets different bytes, and a reader never sees a partly written blob.

The client is the AWS SDK for C++ (`aws-sdk-cpp[s3]` from vcpkg), wrapped in a small `BlobStore` class with `put`,
`get`, `remove` and `list`. It needs an endpoint override, the region, static credentials and, for MinIO, path-style
addressing. `Aws::InitAPI` and `Aws::ShutdownAPI` go in `main`.

Recent SDK versions send extra checksums by default, which some S3-compatible stores reject. Set the client's request
and response checksum calculation to "when required", and cover uploads and downloads against Spaces in the
deployment check (3.8).

### 3.4 Commit protocol

Upload (`PUT /api/v1/databases/{id}`), after the existing size and KDBX signature checks:

1. PUT the blob to its key.
2. In one transaction:
   1. `SELECT … FROM databases WHERE user_name = $1 AND id = $2 FOR UPDATE`. The row lock replaces
      `DatabaseStore::lockFor` and works across instances.
   2. Run the `Precondition` on the row, as today (`If-Match`, `If-None-Match: *`, `428`). It throws to abort, and the
      transaction rolls back.
   3. Copy the current row into `versions`, then upsert `databases` with `revision + 1`, the new ETag, size and blob
      key. A new database is inserted with `ON CONFLICT (user_name, id) DO NOTHING`; inserting nothing means another
      writer created it first, so the upload gets `412`, as when the precondition fails.
   4. Delete `versions` rows beyond `historySize`, returning their blob keys.
   5. `NOTIFY sync_events` with the event (3.5). Postgres only delivers it if the transaction commits.
3. After the commit, delete the pruned keys that no `databases` or `versions` row still references.

`DELETE` locks the row the same way, deletes it and its `versions`, and deletes the blobs after the commit.

`GET` reads the row, then the blob. If the blob was pruned in between, it reads the row again and retries once.

**Orphans.** A writer that loses (`412`) or crashes after step 1 leaves an unreferenced blob. A sweep lists the bucket
and deletes unreferenced blobs older than one hour (newer ones may belong to a commit in progress). It runs every few
hours on whichever instance holds an advisory lock, and on demand with `sweep`.

### 3.5 Events across instances

`DatabaseRepository` writes each event with `NOTIFY sync_events, '<json>'` inside the commit transaction, with the
user in the payload. Each instance has one listening connection (`drogon::orm::DbListener`; the Drogon version in the
vcpkg manifest must include it) and passes events to `EventHub::publish` for that user. The filesystem implementation
publishes directly, as today.

`EventHub` itself doesn't change. The 25-second ping stays.

### 3.6 Configuration

| Variable | Meaning |
|---|---|
| `SYNC_DATABASE_URL` | Postgres connection string. When set, users and databases are stored in Postgres and blobs in the bucket below, and `SYNC_DATA_DIR` is not used. App Platform can inject it from the managed database. |
| `SYNC_DATABASE_CONNECTIONS` | Connections per instance (default: the number of I/O threads) |
| `SYNC_S3_ENDPOINT` | For example `https://nyc3.digitaloceanspaces.com` |
| `SYNC_S3_REGION` | For example `nyc3` |
| `SYNC_S3_BUCKET` | Bucket name |
| `SYNC_S3_PREFIX` | Optional key prefix, so several servers can share a bucket |
| `SYNC_S3_ACCESS_KEY`, `SYNC_S3_SECRET_KEY` | Access key |
| `SYNC_S3_PATH_STYLE` | `1` for path-style URLs (MinIO) |
| `SYNC_BEHIND_PROXY` | `1` silences the warning about listening on a non-loopback address, when the platform terminates TLS |

Setting only some of the `SYNC_S3_*` variables, or `SYNC_DATABASE_URL` without them, is a startup error.
`SYNC_HOST`, `SYNC_PORT`, `SYNC_MAX_DB_BYTES`, `SYNC_HISTORY_SIZE` and `SYNC_THREADS` keep their meanings.

### 3.7 Command line

`users` and `devices` work unchanged against whichever storage is configured. With Postgres, running them in a
one-off container (App Platform console or a job) changes the same state the running instances see, so a pairing code
created that way works at once.

New commands:

- `migrate` applies the schema and exits, for a pre-deploy job.
- `import <data-dir>` copies a filesystem store (users, devices, legacy tokens, databases, kept versions) into
  Postgres and the bucket, keeping revisions and ETags so existing clients see no change. It refuses to overwrite
  databases that already exist.
- `sweep` runs the orphan sweep once.

### 3.8 Prebuilt images and the source-build fallback

**Dependencies** move into `vcpkg.json`: `drogon` with its `postgres` feature, and `aws-sdk-cpp` with
`s3`, at pinned versions. The README's `brew install drogon` / `apt install libdrogon-dev` route keeps working for the
filesystem mode; Postgres and S3 support are compiled in only when their dependencies are found
(`SYNC_SERVER_WITH_CLOUD`, on by default with vcpkg).

**Images**, published to GitHub Container Registry (`ghcr.io/<owner>/…`) for `linux/amd64` and `linux/arm64`:

| Image | Built from | When |
|---|---|---|
| `openvireo-sync-server-builder` | `docker/Dockerfile.builder`: Debian, compiler, CMake, vcpkg, and the manifest's dependencies already built | When `vcpkg.json` or `Dockerfile.builder` changes |
| `openvireo-sync-server` | `Dockerfile`: compiles the server on the builder image, copies the binary into a slim Debian runtime image, runs as a non-root user with `SYNC_HOST=0.0.0.0` and `SYNC_DATA_DIR=/data` | On every push to `develop` (tag `edge`) and on `sync-server-v*` tags (version tags and `latest`) |

The vcpkg Linux triplets build static libraries, so the server binary only needs glibc. Each `sync-server-v*` tag
also attaches `openvireo-sync-server-<version>-linux-<arch>.tar.gz` to a GitHub release, for running without Docker.

**The default** is to deploy the published image: App Platform pulls it and builds nothing. If App Platform can't pull
from GHCR, the workflow also pushes to DigitalOcean Container Registry.

**The fallback** is building from source:

- `docker build .` uses the published builder image, so only the server compiles (about a minute).
- `docker build -f docker/Dockerfile.builder -t builder .`, then
  `docker build --build-arg BUILDER_IMAGE=builder .`, builds everything from scratch with no prebuilt
  images, for anyone who doesn't want to trust them.
- CMake directly, as today.

**Other files**

- `compose.yaml`: the server with Postgres and MinIO, for development and the second test run (5).
- `.do/app.yaml`: the service from the published image (`http_port` matching `SYNC_PORT`, health check on
  `/api/v1/health`), a pre-deploy `migrate` job, the managed database, and the Spaces settings as secrets.
- `.github/workflows/sync-server.yml`: builds and tests on pull requests that touch the server, and publishes the
  images and release archives as above.

**To check on App Platform** before relying on it: pulling from GHCR, the request body size limit (uploads go up to
50 MiB), how long it keeps a streaming response open (the events stream), and uploads and downloads against Spaces
with the SDK's checksum settings (3.3).

## 4. Phases

Each phase is one PR. Phases 1 and 2 change no behaviour.

| # | Scope | Main files |
|---|---|---|
| 1 | `UserRepository` and `DatabaseRepository` interfaces with the current classes as the filesystem implementations. `ApiTest` goes through the interfaces instead of reading `users.json` and `.history` directly. | `UserStore.*`, `DatabaseStore.*`, `SyncApi.cpp`, `main.cpp`, `test/ApiTest.cpp` |
| 2 | vcpkg manifest, builder and server Dockerfiles, `compose.yaml` (filesystem mode with a volume), `SYNC_BEHIND_PROXY`, the GitHub workflow publishing images and release archives, README section | `vcpkg.json`, `docker/Dockerfile.builder`, `Dockerfile`, `compose.yaml`, `.github/workflows/sync-server.yml`, `Config.*`, `main.cpp`, `README.md` |
| 3 | Postgres: connection, migrations, `migrate`, `PgUserRepository`. Databases stay on the filesystem in this phase. | new `Postgres.*`, `PgUserRepository.*`, `migrations/` |
| 4 | `BlobStore` on the AWS SDK, `PgDatabaseRepository` with the commit protocol (3.4), the sweep and `sweep` | new `BlobStore.*`, `PgDatabaseRepository.*` |
| 5 | Events through `LISTEN/NOTIFY` | `SyncApi.cpp`, `PgDatabaseRepository.*` |
| 6 | `import`, the App Platform spec, deployment docs; first `sync-server-v*` release | `main.cpp`, new `Import.*`, `.do/app.yaml`, `README.md` |
| 7 | Optional: C++20 coroutine handlers (`execSqlCoro`, SDK calls off the event loop), so slow storage doesn't hold I/O threads | `SyncApi.cpp`, the repositories, `CMakeLists.txt` |

## 5. Testing

- `ApiTest` runs against both storage modes. The mode comes from the environment (`SYNC_DATABASE_URL` and
  `SYNC_S3_*`), and `compose.yaml` provides Postgres and MinIO for the second run. Each run uses its own schema and
  key prefix so runs don't interfere. The workflow runs both on pull requests.
- New tests for the Postgres mode:
  - two server instances against the same database and bucket: `ConcurrentConditionalWritesExactlyOneWins` with the
    requests split across them;
  - an event uploaded through one instance reaches a stream open on the other;
  - a pairing code created by the CLI is accepted by a running instance;
  - history pruning deletes the right blobs and never one that a row still references;
  - the sweep deletes an unreferenced old blob and keeps a new one;
  - `import` of a data directory with history and a legacy token, after which existing clients' ETags still match.
- OpenVireo's `tests/TestRemoteSyncServer.cpp` and `tests/gui/TestGuiAutoSync.cpp` run unchanged against a server started from
  the published image, in both modes.

## 6. Follow-ups, not in scope

- A shared, table-backed rate limit for pairing claims across instances.
- Streaming uploads and downloads to and from the bucket instead of holding each blob in memory (already true today,
  bounded by `SYNC_MAX_DB_BYTES`).
- Answering `GET` with a short-lived presigned URL instead of proxying the blob. OpenVireo would have to follow the
  redirect without sending its token to the bucket.
- macOS and Windows release archives.
- **Scaling events beyond `LISTEN/NOTIFY`.** Postgres only sees one listener per instance, and there is one
  notification per upload, so the expected load (a few uploads a minute, about one a second even for 1,000 active
  users) is far below its limits.

  The math, assuming each active user uploads 3 times an hour on average (auto-sync after saves) with 3 devices each,
  and peaks at 10 times the average:

  | | Formula | 1 user | 1,000 users | 100,000 users |
  |---|---|---|---|---|
  | Uploads (= notifications) / s, average | users × 3 / 3,600 | 0.0008 | 0.83 | 83 |
  | Uploads / s, peak | × 10 | 0.008 | 8.3 | 830 |
  | Deliveries from Postgres / s, average, 3 instances | uploads/s × instances | 0.0025 | 2.5 | 250 |
  | Messages to devices / s, average | uploads/s × devices per user | 0.0025 | 2.5 | 250 |
  | Postgres connections used for listening | instances | 3 | 3 | 3 |

  - **Commit lock.** A notifying commit holds the lock through its disk flush, roughly 1–5 ms on managed SSD storage
    (an estimate; measure it). That caps notifying commits at about 200–1,000 a second. At 3 uploads an hour that is
    about 240,000–1,200,000 active users on average, or 24,000–120,000 at the 10× peak. The 100,000-user peak above
    (830 a second) is where this starts to matter.
  - **Queue.** A notification takes about 200 bytes in the queue (a 150-byte payload plus overhead), so the 8 GB
    queue holds about 40 million. A stuck listener would fill it in about 1.5 years at 0.83 uploads a second, or about
    5.5 days at 83.
  - **Messages to devices** are sent by each instance from memory (`EventHub`), not by Postgres, so they don't add to
    its load.

  The limits to watch for:
  - A transaction that ran `NOTIFY` takes a database-wide lock while it commits, so notifying commits are serialized.
    With hundreds of concurrent uploads a second this lock becomes the bottleneck.
  - Notifications wait in a shared queue (8 GB by default) until every listener has read them. A stuck listener
    connection lets it fill, and then `NOTIFY`, and with it the upload, fails. `pg_notification_queue_usage()` shows
    how full it is.

  Options if it is ever needed:
  - Send `NOTIFY` after the commit, as its own statement, so uploads don't wait on the lock. A crash in between loses
    that event, which is acceptable because clients still poll.
  - Move events to Redis pub/sub (DigitalOcean has a managed Redis-compatible service), which handles far higher rates
    and takes the load off Postgres.
  - Either is easier if publishing sits behind a small `EventBus` interface (in-process, Postgres, later Redis), so
    changing the backend doesn't touch the upload path.
- **Sizing on DigitalOcean Droplets.** The same image runs on Droplets with Docker instead of App Platform, with a
  DigitalOcean Load Balancer in front once there is more than one. Same assumptions as the events math above (3
  uploads per active user per hour, 3 devices, 10× peaks), plus:
  - a 1 MB database; each upload is downloaded by the user's 2 other devices;
  - a third of all devices are online at any time, each with an events stream open and a `HEAD` poll every 5 minutes;
  - an upload holds an I/O thread for about 150 ms (Spaces PUT plus Postgres), a download for about 100 ms (Spaces
    GET), a poll for about 5 ms (two Postgres queries), because the handlers block until phase 7;
  - an idle events stream costs about 10–30 KB of memory, mostly kernel socket buffers.

  These assumptions are pessimistic (users don't sync around the clock, and most databases are smaller than 1 MB), and
  the per-request times are estimates to measure.

  | | Formula | 1,000 users | 10,000 users | 100,000 users |
  |---|---|---|---|---|
  | Open events streams | users × 3 / 3 | 1,000 | 10,000 | 100,000 |
  | Memory for streams | × 10–30 KB | 10–30 MB | 100–300 MB | 1–3 GB |
  | Polls / s | streams / 300 | 3.3 | 33 | 333 |
  | Uploads / s, peak | users × 3 / 3,600 × 10 | 8.3 | 83 | 830 |
  | Downloads / s, peak | uploads × 2 | 17 | 167 | 1,670 |
  | Busy I/O threads, peak | uploads × 0.15 + downloads × 0.1 + polls × 0.005 | 3 | 30 | 300 |
  | Spaces requests / s, peak | uploads + downloads | 25 | 250 | 2,500 |
  | Postgres queries / s, average | polls × 2 + uploads / 10 × 6 | 12 | 116 | 1,166 |
  | Transfer to devices / month, average | downloads / 10 × 1 MB × 2.6 M s | 4.3 TB | 43 TB | 430 TB |

  Suggested setups (Droplet and database sizes and prices as of writing; check current ones):

  | Users | Server | Postgres | Notes |
  |---|---|---|---|
  | Up to ~1,000 | 1 Droplet, 1 vCPU / 2 GB, `SYNC_THREADS=8`. A second one plus a Load Balancer only for redundancy. | Smallest managed plan (1 GB) | Comfortable. Transfer exceeds a small Droplet's included allowance at these pessimistic numbers; realistic use is far lower. |
  | ~10,000 | 2 Droplets, 2 vCPU / 4 GB, `SYNC_THREADS=32` each, behind a Load Balancer | 2 GB plan; use a connection pool for queries (the listener needs a direct connection) | Peak Spaces requests (about 250 a second) approach Spaces' per-bucket rate limits; check the current limits. Raise the open-file limit for the streams. |
  | ~100,000 | Not with this design | | Blocking handlers would need about 300 busy threads at peak (needs phase 7), Spaces requests (about 2,500 a second) are likely far beyond one bucket's rate limits (needs presigned downloads or a cache), notifying commits reach the `LISTEN/NOTIFY` lock (needs Redis events), and transfer becomes the main cost. |

  Things to check before relying on these numbers: how many concurrent connections and new connections a second one
  Load Balancer node handles (each events stream is a long-lived connection), its idle timeout (must stay above the
  25-second ping), Spaces' request rate limits and outbound transfer pricing, the managed database's connection limit
  per plan, and the Droplets' included transfer.

## 7. As implemented

Phases 1 to 6 are in, with these differences from the plan above:

- **No AWS SDK, no vcpkg.** `BlobStore` signs plain S3 requests (SigV4) itself and sends them with Drogon's HTTP
  client on an event loop of its own, so the only new dependencies are libpq (through Drogon) and OpenSSL, which
  Drogon already uses. That also avoids the SDK's default checksums (3.3). The Dockerfile builds Drogon with
  PostgreSQL from source in a cached stage, so there is no separate builder image yet.
- **Blob keys have a random part**: `<prefix><user>/<id>/<sha256>-<random>.kdbx`. With keys derived from the
  content only, two versions with the same content would share a file, and pruning one could delete the other's
  file while an upload with the same content was committing. With a key per upload, every key belongs to exactly
  one row, and pruning deletes it without checking for other references.
- **Pairing codes are stored hashed** (`code_hash`), as `users.json` does, not normalised.
- **Device ids are unique per user** (`PRIMARY KEY (user_name, id)`): every legacy token becomes a device called
  `legacy`, so ids repeat across users after `import`.
- **No pre-deploy `migrate` job** in `.do/app.yaml`: instances migrate on startup under the advisory lock;
  `migrate` stays as a command.
- **`SYNC_DATABASE_SCHEMA`** puts the tables in their own schema. The tests use it with a key prefix per run.
- **Local S3** in `compose.cloud.yaml` is Versity Gateway, since MinIO no longer publishes images.
- **Not done yet:** the GitHub workflow publishing images and release archives, and the two-process tests from 5.
  Instead the tests run a second repository on the same Postgres and bucket inside the test process (concurrent
  conditional writes, events across instances), and pruning, the sweep and `import` have tests of their own.

