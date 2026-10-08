// Loads the plain <script> files of web/js/ (no modules, no build step) into a
// function scope so their classes and helpers can be tested under `node --test`.
// Same load order as index.html; only the files that do not need a DOM.

const fs = require("node:fs");
const path = require("node:path");

const WEB_JS = path.join(__dirname, "..", "..", "web", "js");

function loadWeb(files, exportNames, con = console) {
  const source = files.map((f) => fs.readFileSync(path.join(WEB_JS, f), "utf8")).join("\n;\n");
  return new Function("console", `${source}\n;return { ${exportNames.join(", ")} };`)(con);
}

module.exports = { loadWeb };
