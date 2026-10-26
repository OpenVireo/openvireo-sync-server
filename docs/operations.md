# Operations

## Backups

Everything the server stores is either encrypted databases or hashes of tokens, so a backup is as safe to keep
as the databases themselves. Still, keep it private: it holds every user's encrypted files and kept versions.

**Data directory.** Back up the whole `SYNC_DATA_DIR`. `users.sqlite` can change while the server runs, so copy it
with SQLite's online backup rather than `cp`:

```sh
sqlite3 /data/users.sqlite ".backup '/backups/users.sqlite'"
rsync -a --exclude users.sqlite /data/ /backups/data/
```

Databases are written atomically (a new file, then a rename), so copying them while the server runs is fine.

**Postgres and a bucket.** Back up both: Postgres says which file in the bucket is the current version of each
database, and the bucket holds the files. Use your provider's Postgres backups (or `pg_dump`) and turn on
versioning or replication for the bucket. To restore, restore Postgres first, then make sure every `blob_key` it
names exists in the bucket. Files the restored database doesn't name are harmless; the next
[sweep](configuration.md#storage) deletes them.

Restoring an old backup brings old versions back. Devices that synced since then notice that the server's copy
is older than one they already have (rollback protection, which also guards against a tampered server) and stop
to ask: OpenVireo shows **Check the sync server**, and answering **Sync anyway** merges the server's copy into the
local database and uploads the result, so nothing is lost. Tell users to expect that question after a restore.

## Upgrades

Upgrade by replacing the binary or image and restarting. With Postgres the server upgrades its tables when it
starts (instances starting together take turns), so a rolling deploy works: start new instances, then stop the
old ones. Migrations only add to the schema, so old and new instances can run side by side during the roll.
`openvireo-sync-server migrate` applies them ahead of time if you'd rather run a pre-deploy job.

A data directory from an older server is upgraded on the first start (for example, `users.json` is copied into
`users.sqlite`).

The GHCR tags follow semantic versioning: `:1` gets every compatible release, `:1.2` only fixes.

## Moving to Postgres

To move from a data directory to Postgres and a bucket, stop the server and run, with the Postgres and S3
settings:

```sh
openvireo-sync-server import /path/to/data
```

It copies users, devices (old per-user tokens included), databases, vaults and their kept versions, keeping
revisions and ETags, so devices carry on syncing without noticing. It leaves users and databases that already
exist alone and says so (and exits with `1`), so it can be run again after fixing a problem.

## Scaling

One instance with a data directory serves a household or a small team easily. With Postgres and a bucket the
server keeps no state of its own, so you can run several instances behind a load balancer:

- Postgres decides every upload, so conditional uploads stay safe across instances.
- Change notifications reach streams on every instance through `LISTEN`/`NOTIFY`.
- The pairing rate limit is counted per instance.
- Each events stream is a long-lived connection: the load balancer's idle timeout must be above the
  25-second ping, and the open-file limit high enough for one socket per connected device.

`SYNC_THREADS` sets the I/O threads per instance. Handlers block while they talk to Postgres and the bucket, so
more threads than cores helps (for example 8 on one vCPU). `SYNC_DATABASE_CONNECTIONS` defaults to one per
thread.

Rough sizing, from the [design notes](stateless-design.md#6-follow-ups-not-in-scope) (pessimistic: every
active user uploads 3 times an hour from 3 devices, 1 MB databases, 10× peaks):

| Users | Server | Postgres |
|---|---|---|
| Up to ~1,000 | 1 instance, 1 vCPU / 2 GB, `SYNC_THREADS=8` (a second only for redundancy) | Smallest managed plan |
| ~10,000 | 2 instances, 2 vCPU / 4 GB, `SYNC_THREADS=32` each | 2 GB plan, with a connection pool for queries |
| ~100,000 | Needs changes to the design (presigned downloads, another event bus) | |

## Monitoring

- `GET /api/v1/health` answers `{"status": "ok"}` without a token. It shows the process is serving requests;
  it doesn't check Postgres or the bucket.
- The server logs warnings and errors to standard error, and the address it listens on and the storage it uses
  to standard output when it starts. It doesn't log requests; let the reverse proxy do that.
- With Postgres, `SELECT pg_notification_queue_usage()` shows how full the notification queue is. It should stay
  near zero.
