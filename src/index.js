'use strict';

const { createApp } = require('./app');
const { loadConfig } = require('./config');

const config = loadConfig();
const app = createApp(config);

const server = app.listen(config.port, config.host, () => {
  console.log(`KeePassXC sync server listening on http://${config.host}:${config.port}`);
  console.log(`Data directory: ${config.dataDir}`);
  if (config.host !== '127.0.0.1' && config.host !== 'localhost' && config.host !== '::1') {
    console.warn('Warning: listening on a non-loopback address. Put the server behind HTTPS (e.g. a reverse proxy).');
  }
});

function shutdown() {
  server.close(() => process.exit(0));
}
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
