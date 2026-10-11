'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs/promises');
const path = require('node:path');

/**
 * Stores opaque database blobs on disk. The server never decrypts anything;
 * KeePassXC uploads the already-encrypted .kdbx file.
 *
 * Layout:
 *   <dataDir>/databases/<user>/<id>.kdbx        current version
 *   <dataDir>/databases/<user>/<id>.meta.json   { revision, etag, size, updatedAt }
 *   <dataDir>/databases/<user>/.history/<id>/<revision>.kdbx   previous versions
 */
class DatabaseStore {
  constructor({ dataDir, historySize }) {
    this.root = path.join(dataDir, 'databases');
    this.historySize = historySize;
    this.locks = new Map();
  }

  paths(user, id) {
    const dir = path.join(this.root, user);
    return {
      dir,
      blob: path.join(dir, `${id}.kdbx`),
      meta: path.join(dir, `${id}.meta.json`),
      history: path.join(dir, '.history', id),
    };
  }

  /** Serialises operations on one database so check-then-write is atomic. */
  async withLock(user, id, fn) {
    const key = `${user}/${id}`;
    const previous = this.locks.get(key) || Promise.resolve();
    let release;
    const current = new Promise((resolve) => (release = resolve));
    const chained = previous.then(() => current);
    this.locks.set(key, chained);
    await previous;
    try {
      return await fn();
    } finally {
      release();
      if (this.locks.get(key) === chained) {
        this.locks.delete(key);
      }
    }
  }

  async getMeta(user, id) {
    try {
      return JSON.parse(await fs.readFile(this.paths(user, id).meta, 'utf8'));
    } catch (err) {
      if (err.code === 'ENOENT') {
        return null;
      }
      throw err;
    }
  }

  async read(user, id) {
    const meta = await this.getMeta(user, id);
    if (!meta) {
      return null;
    }
    const data = await fs.readFile(this.paths(user, id).blob);
    return { meta, data };
  }

  async list(user) {
    let names;
    try {
      names = await fs.readdir(path.join(this.root, user));
    } catch (err) {
      if (err.code === 'ENOENT') {
        return [];
      }
      throw err;
    }
    const ids = names.filter((n) => n.endsWith('.meta.json')).map((n) => n.slice(0, -'.meta.json'.length));
    const metas = await Promise.all(ids.map((id) => this.getMeta(user, id)));
    return metas.filter(Boolean).sort((a, b) => a.id.localeCompare(b.id));
  }

  /**
   * Writes a new version. `precondition(meta)` runs under the lock with the current
   * metadata (or null) and may throw to abort the write.
   */
  async write(user, id, data, precondition) {
    return this.withLock(user, id, async () => {
      const current = await this.getMeta(user, id);
      precondition(current);

      const p = this.paths(user, id);
      await fs.mkdir(p.dir, { recursive: true, mode: 0o700 });

      if (current && this.historySize > 0) {
        await fs.mkdir(p.history, { recursive: true, mode: 0o700 });
        await fs.copyFile(p.blob, path.join(p.history, `${current.revision}.kdbx`));
        await this.pruneHistory(p.history);
      }

      const meta = {
        id,
        revision: (current?.revision || 0) + 1,
        etag: `"${crypto.createHash('sha256').update(data).digest('hex')}"`,
        size: data.length,
        updatedAt: new Date().toISOString(),
      };

      // Blob first, then metadata, each via rename so readers never see a partial file
      await atomicWrite(p.blob, data);
      await atomicWrite(p.meta, JSON.stringify(meta, null, 2));
      return { meta, created: !current };
    });
  }

  async remove(user, id, precondition) {
    return this.withLock(user, id, async () => {
      const current = await this.getMeta(user, id);
      precondition(current);
      if (!current) {
        return false;
      }
      const p = this.paths(user, id);
      await fs.rm(p.meta, { force: true });
      await fs.rm(p.blob, { force: true });
      await fs.rm(p.history, { recursive: true, force: true });
      return true;
    });
  }

  async pruneHistory(historyDir) {
    const revisions = (await fs.readdir(historyDir))
      .map((n) => Number.parseInt(n, 10))
      .filter(Number.isFinite)
      .sort((a, b) => b - a);
    for (const rev of revisions.slice(this.historySize)) {
      await fs.rm(path.join(historyDir, `${rev}.kdbx`), { force: true });
    }
  }
}

async function atomicWrite(file, contents) {
  const tmp = `${file}.${process.pid}.${crypto.randomBytes(4).toString('hex')}.tmp`;
  await fs.writeFile(tmp, contents, { mode: 0o600 });
  await fs.rename(tmp, file);
}

module.exports = { DatabaseStore };
