'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { after, before, describe, test } = require('node:test');

const { createApp } = require('../src/app');
const { loadConfig } = require('../src/config');
const { UserStore } = require('../src/users');

const KDBX_HEADER = Buffer.from([0x03, 0xd9, 0xa2, 0x9a, 0x67, 0xfb, 0x4b, 0xb5]);

function fakeDatabase(contents) {
  return Buffer.concat([KDBX_HEADER, Buffer.from(contents)]);
}

describe('sync server API', () => {
  let server;
  let baseUrl;
  let dataDir;
  let aliceToken;
  let bobToken;

  before(async () => {
    dataDir = fs.mkdtempSync(path.join(os.tmpdir(), 'kpxc-sync-test-'));
    const users = new UserStore(dataDir);
    aliceToken = users.setUser('alice');
    bobToken = users.setUser('bob');

    const app = createApp(loadConfig({ dataDir, maxDatabaseBytes: 1024, historySize: 2 }));
    await new Promise((resolve) => {
      server = app.listen(0, '127.0.0.1', resolve);
    });
    baseUrl = `http://127.0.0.1:${server.address().port}/api/v1`;
  });

  after(() => {
    server.close();
    fs.rmSync(dataDir, { recursive: true, force: true });
  });

  const request = (method, urlPath, { token = aliceToken, headers = {}, body } = {}) =>
    fetch(`${baseUrl}${urlPath}`, {
      method,
      body,
      headers: { ...(token ? { Authorization: `Bearer ${token}` } : {}), ...headers },
    });

  test('health check needs no auth', async () => {
    const res = await request('GET', '/health', { token: null });
    assert.equal(res.status, 200);
    assert.deepEqual(await res.json(), { status: 'ok' });
  });

  test('rejects missing and invalid tokens', async () => {
    assert.equal((await request('GET', '/databases', { token: null })).status, 401);
    assert.equal((await request('GET', '/databases', { token: 'nope' })).status, 401);
  });

  test('rejects path traversal in database ids', async () => {
    const res = await request('GET', '/databases/..%2F..%2Fusers.json');
    assert.equal(res.status, 400);
  });

  test('full create / read / update cycle with optimistic concurrency', async () => {
    assert.equal((await request('GET', '/databases/main')).status, 404);

    const v1 = fakeDatabase('version 1');
    const created = await request('PUT', '/databases/main', { body: v1, headers: { 'If-None-Match': '*' } });
    assert.equal(created.status, 201);
    const etag1 = created.headers.get('etag');
    assert.match(etag1, /^"[0-9a-f]{64}"$/);

    // Creating again must fail
    const again = await request('PUT', '/databases/main', { body: v1, headers: { 'If-None-Match': '*' } });
    assert.equal(again.status, 412);

    const read = await request('GET', '/databases/main');
    assert.equal(read.status, 200);
    assert.equal(read.headers.get('etag'), etag1);
    assert.deepEqual(Buffer.from(await read.arrayBuffer()), v1);

    const notModified = await request('GET', '/databases/main', { headers: { 'If-None-Match': etag1 } });
    assert.equal(notModified.status, 304);

    // Blind overwrite is refused
    const blind = await request('PUT', '/databases/main', { body: fakeDatabase('x') });
    assert.equal(blind.status, 428);

    const v2 = fakeDatabase('version 2');
    const updated = await request('PUT', '/databases/main', { body: v2, headers: { 'If-Match': etag1 } });
    assert.equal(updated.status, 200);
    const etag2 = updated.headers.get('etag');
    assert.notEqual(etag2, etag1);
    assert.equal((await updated.json()).revision, 2);

    // A second device still holding etag1 loses the race
    const stale = await request('PUT', '/databases/main', { body: fakeDatabase('v2b'), headers: { 'If-Match': etag1 } });
    assert.equal(stale.status, 412);

    const list = await (await request('GET', '/databases')).json();
    assert.deepEqual(
      list.databases.map((d) => [d.id, d.revision]),
      [['main', 2]],
    );
  });

  test('keeps a bounded history of previous versions', async () => {
    let etag = (await request('HEAD', '/databases/main')).headers.get('etag');
    for (let i = 3; i <= 6; i++) {
      const res = await request('PUT', '/databases/main', {
        body: fakeDatabase(`version ${i}`),
        headers: { 'If-Match': etag },
      });
      assert.equal(res.status, 200);
      etag = res.headers.get('etag');
    }
    const history = fs.readdirSync(path.join(dataDir, 'databases', 'alice', '.history', 'main')).sort();
    assert.deepEqual(history, ['4.kdbx', '5.kdbx']);
  });

  test('users cannot see each other’s databases', async () => {
    assert.equal((await request('GET', '/databases/main', { token: bobToken })).status, 404);
    const list = await (await request('GET', '/databases', { token: bobToken })).json();
    assert.deepEqual(list.databases, []);
  });

  test('rejects bodies that are not KDBX files or are too large', async () => {
    const notKdbx = await request('PUT', '/databases/other', { body: Buffer.from('hello world, not a db') });
    assert.equal(notKdbx.status, 415);

    const tooBig = await request('PUT', '/databases/other', { body: fakeDatabase('x'.repeat(2048)) });
    assert.equal(tooBig.status, 413);
  });

  test('concurrent conditional writes: exactly one wins', async () => {
    const created = await request('PUT', '/databases/race', {
      body: fakeDatabase('base'),
      headers: { 'If-None-Match': '*' },
    });
    const etag = created.headers.get('etag');
    const results = await Promise.all(
      [1, 2, 3, 4, 5].map((i) =>
        request('PUT', '/databases/race', { body: fakeDatabase(`writer ${i}`), headers: { 'If-Match': etag } }),
      ),
    );
    const statuses = results.map((r) => r.status).sort();
    assert.deepEqual(statuses, [200, 412, 412, 412, 412]);
  });

  test('delete honours If-Match', async () => {
    const head = await request('HEAD', '/databases/race');
    assert.equal(
      (await request('DELETE', '/databases/race', { headers: { 'If-Match': '"0000"' } })).status,
      412,
    );
    assert.equal(
      (await request('DELETE', '/databases/race', { headers: { 'If-Match': head.headers.get('etag') } })).status,
      204,
    );
    assert.equal((await request('GET', '/databases/race')).status, 404);
  });
});
