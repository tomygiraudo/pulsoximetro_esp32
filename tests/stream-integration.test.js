// CloudSource / CloudHistory against the local mock of Firebase's REST + streaming
// API (tools/cloud/mock_rtdb.js) using Node's real EventSource, in a child process
// because EventSource is behind --experimental-eventsource in current Node.
// Skipped when that flag does not exist (older Node).

const test = require("node:test");
const assert = require("node:assert/strict");
const { spawnSync } = require("node:child_process");
const path = require("node:path");

const hasEventSource =
  spawnSync(process.execPath, ["--experimental-eventsource", "-e", "process.exit(typeof EventSource === 'function' ? 0 : 1)"]).status === 0;

test(
  "real EventSource: 307 redirect, put/patch events, dropped stream, history",
  { skip: hasEventSource ? false : "EventSource not available in this Node", timeout: 60000 },
  () => {
    const script = path.join(__dirname, "helpers", "stream-client.js");
    const run = spawnSync(process.execPath, ["--experimental-eventsource", "--no-warnings", script], { encoding: "utf8", timeout: 50000 });
    assert.equal(run.status, 0, `${run.stdout}\n${run.stderr}`);
    assert.match(run.stdout, /STREAM-INTEGRATION-OK/);
  }
);
