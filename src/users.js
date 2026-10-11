'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

// User names and database ids end up as path components, so keep them strict.
const NAME_PATTERN = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;

function isValidName(name) {
  return typeof name === 'string' && NAME_PATTERN.test(name);
}

function hashToken(token) {
  return crypto.createHash('sha256').update(token, 'utf8').digest('hex');
}

function generateToken() {
  return crypto.randomBytes(32).toString('base64url');
}

/**
 * Users live in <dataDir>/users.json as { "users": { "<name>": { "tokenHash": "<sha256 hex>" } } }.
 * Only token hashes are stored; the plain token is shown once when it is created.
 */
class UserStore {
  constructor(dataDir) {
    this.file = path.join(dataDir, 'users.json');
    this.cache = null;
    this.cacheMtimeMs = 0;
  }

  read() {
    let stat;
    try {
      stat = fs.statSync(this.file);
    } catch (err) {
      if (err.code === 'ENOENT') {
        return { users: {} };
      }
      throw err;
    }
    // Re-read when the file changes so users can be added without a restart
    if (!this.cache || stat.mtimeMs !== this.cacheMtimeMs) {
      this.cache = JSON.parse(fs.readFileSync(this.file, 'utf8'));
      this.cache.users ??= {};
      this.cacheMtimeMs = stat.mtimeMs;
    }
    return this.cache;
  }

  write(data) {
    fs.mkdirSync(path.dirname(this.file), { recursive: true });
    const tmp = `${this.file}.${process.pid}.tmp`;
    fs.writeFileSync(tmp, JSON.stringify(data, null, 2), { mode: 0o600 });
    fs.renameSync(tmp, this.file);
    this.cache = null;
  }

  /** Returns the user name that owns the token, or null. */
  authenticate(token) {
    if (typeof token !== 'string' || token.length === 0) {
      return null;
    }
    const candidate = Buffer.from(hashToken(token), 'hex');
    let match = null;
    // Compare against every user in constant time per comparison
    for (const [name, user] of Object.entries(this.read().users)) {
      const stored = Buffer.from(user.tokenHash || '', 'hex');
      if (stored.length === candidate.length && crypto.timingSafeEqual(stored, candidate)) {
        match = name;
      }
    }
    return match;
  }

  /** Creates the user or rotates its token. Returns the new plain token. */
  setUser(name) {
    if (!isValidName(name)) {
      throw new Error(`Invalid user name "${name}" (allowed: letters, digits, ".", "_", "-"; max 64 chars)`);
    }
    const data = this.read();
    const token = generateToken();
    data.users[name] = { tokenHash: hashToken(token), createdAt: new Date().toISOString() };
    this.write(data);
    return token;
  }

  removeUser(name) {
    const data = this.read();
    if (!data.users[name]) {
      return false;
    }
    delete data.users[name];
    this.write(data);
    return true;
  }

  listUsers() {
    return Object.keys(this.read().users).sort();
  }
}

module.exports = { UserStore, isValidName, hashToken };
