# Users and devices

On a self-hosted server (`SYNC_AUTH_MODE=standalone`, the default) the server has **users**, and each user
has **devices**. Every device has its own access token, so one lost device can be shut out without touching
the others. All of a user's devices see the same databases; users can't see each other's.

Only SHA-256 hashes of tokens and pairing codes are stored. Changes take effect at once, without a restart.

## Commands

```sh
openvireo-sync-server users add <name> [device-name]     # create a user with a first device, print its token
openvireo-sync-server users remove <name>                # delete a user and their devices (databases are kept)
openvireo-sync-server users list

openvireo-sync-server devices add <user> <device-name>   # a new device and its access token
openvireo-sync-server devices pair <user>                # a one-time code a new device trades for a token
openvireo-sync-server devices list <user>                # ids, names, when each was added and last seen
openvireo-sync-server devices revoke <user> <device-id>  # its token stops working at once
```

Tokens are printed once and can't be shown again. The commands work on whichever storage is configured. With
Postgres you can run them from any machine or one-off container with the same `SYNC_*` settings; with Docker,
use `docker exec` or `docker compose run --rm sync-server …`.

## Adding a device

The easiest way is a pairing code: on a device that already syncs, open **Remote Sync** in the database
settings, select the sync server and click **Pair Device…**. Or on the server:

```sh
openvireo-sync-server devices pair alice   # prints a code such as WJDA-247W
```

On the new device, enter the code where OpenVireo asks for the access token. It trades the code for a token
of its own, named after the computer. A code adds one device and works for 10 minutes. If you'd rather copy a
token by hand, `devices add alice phone` prints one.

To get the database onto the new device:

1. Use **Database → Import…** and choose *OpenVireo Sync Server (.kdbx)*.
2. Enter the server URL and a pairing code (or the new device's access token), then click **Load** to pick a
   database, or type its ID.
3. Enter the database's password and/or key file.
4. Choose where to import it:
   - **Local Copy** opens the server database unchanged (same password) and asks where to save it. It is
     already linked to the server, so **Remote Sync** works right away.
   - **Existing Database** copies its entries into a group of a database you have open.
   - **Temporary Database** opens it without saving it anywhere first.

If a device's copy uses a different password or key file, OpenVireo asks you to unlock the server copy
before merging.

The web app and the SDK pair the same way (see `SelfHostedSync.pair` in the
[SDK docs](https://github.com/openvireo/openvireo-sdk/blob/develop/docs/guides/self-hosted-sync.md)).

## Losing a device

Revoke it:

```sh
openvireo-sync-server devices list alice
openvireo-sync-server devices revoke alice 3f2a…
```

Its token can no longer reach the server. An events stream the device already had open stays open until
it disconnects, but it only carries database ids and ETags, never data. If whoever has the device may
also know the database's password, change that password too, so copies uploaded from now on can't be opened
with the old one. The server can't do that for you: it never sees the password.

## Upgrading from one token per user

Tokens from before devices existed keep working, as a device called `legacy` that all your devices share.
Older OpenVireo versions also stored that token inside the database. To move to per-device tokens:

1. `devices add <user> <device-name>` for each device, and enter each token in that device's Remote Sync
   settings. Saving the settings also removes the old token from the database.
2. `devices revoke <user> legacy`.
