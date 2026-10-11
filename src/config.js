'use strict';

const path = require('node:path');

function intFromEnv(name, fallback) {
  const raw = process.env[name];
  if (raw === undefined || raw === '') {
    return fallback;
  }
  const value = Number.parseInt(raw, 10);
  if (!Number.isFinite(value) || value < 0) {
    throw new Error(`Environment variable ${name} must be a non-negative integer, got "${raw}"`);
  }
  return value;
}

function loadConfig(overrides = {}) {
  return {
    host: process.env.SYNC_HOST || '127.0.0.1',
    port: intFromEnv('SYNC_PORT', 8787),
    dataDir: path.resolve(process.env.SYNC_DATA_DIR || path.join(__dirname, '..', 'data')),
    // Maximum accepted upload size in bytes (default 50 MiB)
    maxDatabaseBytes: intFromEnv('SYNC_MAX_DB_BYTES', 50 * 1024 * 1024),
    // How many previous versions of each database to keep on disk as a safety net
    historySize: intFromEnv('SYNC_HISTORY_SIZE', 10),
    // Set when running behind a reverse proxy that terminates TLS
    trustProxy: process.env.SYNC_TRUST_PROXY === '1',
    ...overrides,
  };
}

module.exports = { loadConfig };
