'use strict';

const express = require('express');

const { DatabaseStore } = require('./store');
const { UserStore, isValidName } = require('./users');

// KDBX 2/3/4 files start with these two little-endian uint32 signatures
const KDBX_SIGNATURE = Buffer.from([0x03, 0xd9, 0xa2, 0x9a, 0x67, 0xfb, 0x4b, 0xb5]);

class HttpError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

function parseEtagList(header) {
  if (!header) {
    return null;
  }
  return header
    .split(',')
    .map((tag) => tag.trim().replace(/^W\//, ''))
    .filter(Boolean);
}

function etagMatches(list, meta) {
  return list.includes('*') ? Boolean(meta) : Boolean(meta) && list.includes(meta.etag);
}

/**
 * Optimistic concurrency for writes, following RFC 9110:
 *   If-None-Match: *     -> only create, fail if it already exists
 *   If-Match: "<etag>"   -> only overwrite the version the client last saw
 * Overwriting an existing database without either header is refused (428) so a
 * client can never blindly clobber changes made from another device.
 */
function checkWritePreconditions(req, meta) {
  const ifMatch = parseEtagList(req.get('If-Match'));
  const ifNoneMatch = parseEtagList(req.get('If-None-Match'));

  if (ifMatch && !etagMatches(ifMatch, meta)) {
    throw new HttpError(412, 'The database was changed on the server. Download, merge and try again.');
  }
  if (ifNoneMatch && (ifNoneMatch.includes('*') ? meta : etagMatches(ifNoneMatch, meta))) {
    throw new HttpError(412, 'The database already exists on the server.');
  }
  if (meta && !ifMatch && !ifNoneMatch) {
    throw new HttpError(428, 'Updating an existing database requires an If-Match header.');
  }
}

function describe(meta) {
  return {
    id: meta.id,
    revision: meta.revision,
    etag: meta.etag,
    size: meta.size,
    updatedAt: meta.updatedAt,
  };
}

function setMetaHeaders(res, meta) {
  res.set('ETag', meta.etag);
  res.set('Last-Modified', new Date(meta.updatedAt).toUTCString());
  res.set('X-Database-Revision', String(meta.revision));
}

function createApp(config) {
  const users = new UserStore(config.dataDir);
  const store = new DatabaseStore(config);
  const app = express();

  app.disable('x-powered-by');
  // We manage ETags ourselves (SHA-256 of the stored blob)
  app.set('etag', false);
  if (config.trustProxy) {
    app.set('trust proxy', 'loopback');
  }

  app.use((req, res, next) => {
    res.set('Cache-Control', 'no-store');
    next();
  });

  app.get('/api/v1/health', (req, res) => {
    res.json({ status: 'ok' });
  });

  const api = express.Router();

  api.use((req, res, next) => {
    const match = /^Bearer\s+(\S+)$/i.exec(req.get('Authorization') || '');
    const user = match ? users.authenticate(match[1]) : null;
    if (!user) {
      res.set('WWW-Authenticate', 'Bearer realm="keepassxc-sync"');
      return next(new HttpError(401, 'Missing or invalid access token.'));
    }
    req.user = user;
    next();
  });

  api.param('id', (req, res, next, id) => {
    if (!isValidName(id)) {
      return next(new HttpError(400, 'Invalid database id (allowed: letters, digits, ".", "_", "-"; max 64 chars).'));
    }
    next();
  });

  api.get('/databases', async (req, res) => {
    const metas = await store.list(req.user);
    res.json({ databases: metas.map(describe) });
  });

  api.get('/databases/:id', async (req, res) => {
    const meta = await store.getMeta(req.user, req.params.id);
    if (!meta) {
      throw new HttpError(404, 'Database not found.');
    }
    setMetaHeaders(res, meta);

    const ifNoneMatch = parseEtagList(req.get('If-None-Match'));
    if (ifNoneMatch && etagMatches(ifNoneMatch, meta)) {
      return res.status(304).end();
    }
    if (req.method === 'HEAD') {
      res.set('Content-Length', String(meta.size));
      return res.status(200).end();
    }

    const entry = await store.read(req.user, req.params.id);
    // Re-set headers in case the database changed between the two reads
    setMetaHeaders(res, entry.meta);
    res.type('application/octet-stream').send(entry.data);
  });

  api.put(
    '/databases/:id',
    express.raw({ type: () => true, limit: config.maxDatabaseBytes }),
    async (req, res) => {
      const data = req.body;
      if (!Buffer.isBuffer(data) || data.length < KDBX_SIGNATURE.length) {
        throw new HttpError(400, 'Request body must be a KeePass (.kdbx) database.');
      }
      if (!data.subarray(0, KDBX_SIGNATURE.length).equals(KDBX_SIGNATURE)) {
        throw new HttpError(415, 'Request body is not a KDBX database.');
      }

      const { meta, created } = await store.write(req.user, req.params.id, data, (current) =>
        checkWritePreconditions(req, current),
      );
      setMetaHeaders(res, meta);
      res.status(created ? 201 : 200).json(describe(meta));
    },
  );

  api.delete('/databases/:id', async (req, res) => {
    const removed = await store.remove(req.user, req.params.id, (current) => {
      const ifMatch = parseEtagList(req.get('If-Match'));
      if (current && ifMatch && !etagMatches(ifMatch, current)) {
        throw new HttpError(412, 'The database was changed on the server.');
      }
    });
    if (!removed) {
      throw new HttpError(404, 'Database not found.');
    }
    res.status(204).end();
  });

  app.use('/api/v1', api);

  app.use((req, res, next) => next(new HttpError(404, 'Not found.')));

  // eslint-disable-next-line no-unused-vars
  app.use((err, req, res, next) => {
    let status = err.status || err.statusCode || 500;
    let message = err.message;
    if (err.type === 'entity.too.large') {
      status = 413;
      message = `Database exceeds the ${config.maxDatabaseBytes} byte limit.`;
    } else if (status >= 500) {
      console.error(err);
      message = 'Internal server error.';
    }
    res.status(status).json({ error: message });
  });

  return app;
}

module.exports = { createApp };
