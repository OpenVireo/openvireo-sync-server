#!/usr/bin/env node
'use strict';

const { loadConfig } = require('../src/config');
const { UserStore } = require('../src/users');

const usage = `Usage: manage-users <command> [name]

Commands:
  add <name>      Create a user (or rotate its token) and print the new access token
  remove <name>   Delete a user (their stored databases are kept on disk)
  list            List users

Uses SYNC_DATA_DIR (default: ./data) to locate users.json.`;

function main(argv) {
  const [command, name] = argv;
  const users = new UserStore(loadConfig().dataDir);

  switch (command) {
    case 'add': {
      if (!name) {
        break;
      }
      const token = users.setUser(name);
      console.log(`Access token for "${name}" (shown only once, enter it in KeePassXC):\n\n  ${token}\n`);
      return 0;
    }
    case 'remove':
      if (!name) {
        break;
      }
      if (!users.removeUser(name)) {
        console.error(`No such user: ${name}`);
        return 1;
      }
      console.log(`Removed user "${name}".`);
      return 0;
    case 'list':
      for (const user of users.listUsers()) {
        console.log(user);
      }
      return 0;
    default:
      break;
  }
  console.error(usage);
  return 2;
}

try {
  process.exitCode = main(process.argv.slice(2));
} catch (err) {
  console.error(err.message);
  process.exitCode = 1;
}
