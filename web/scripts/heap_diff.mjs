// Diff two CDP heap snapshots captured by scripts/heap_profile.mjs.
//
// Aggregates node count + self size by constructor name for each snapshot,
// then reports the top growers (by self-size delta) plus the largest
// absolute holders for context. A growing top name = retention; flat names
// with a climbing curve = churn (GC sawtooth, not a true leak).
//
// Usage:
//   node --max-old-space-size=8192 scripts/heap_diff.mjs [snap1] [snap2]
//   (defaults: agent/profile/heap-1.heapsnapshot, agent/profile/heap-2.heapsnapshot)
import fs from "node:fs";
import path from "node:path";

const defaultDir = path.resolve(new URL("..", import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1"), "profile");
const [path1, path2] = process.argv.length >= 4
  ? process.argv.slice(2)
  : [path.join(defaultDir, "heap-1.heapsnapshot"), path.join(defaultDir, "heap-2.heapsnapshot")];

const MiB = 1048576;

// node_fields order varies by Chromium build, so map by name.
function aggregate(file) {
  const raw = JSON.parse(fs.readFileSync(file, "utf8"));
  const snap = raw.snapshot ?? raw;
  const fields = (snap.meta ?? snap.meta_data).node_fields;
  const idx = Object.fromEntries(fields.map((name, i) => [name, i]));
  // Chrome 154+ keeps nodes/edges/strings at the top level; older builds nest
  // them inside `snapshot`.
  const nodes = raw.nodes ?? snap.nodes;
  const strings = raw.strings ?? snap.strings;
  const perNode = fields.length;
  // Chrome 154+ format has no name_type field; name is a strings index when >= 0.
  const nameType = idx.name_type ?? 0;
  const hasNameType = idx.name_type !== undefined;
  const nameIdx = idx.name;
  const sizeIdx = idx.self_size;
  const typeIdx = idx.type;

  const byName = new Map();
  let nodeCount = 0;
  let totalSelf = 0;
  for (let i = 0; i < nodes.length; i += perNode) {
    const nameTypeCode = hasNameType ? nodes[i + nameType] : 1;
    const nameCode = nodes[i + nameIdx];
    const name =
      nameTypeCode !== 0 && nameCode >= 0 && nameCode < strings.length
        ? strings[nameCode]
        : `<hidden:${nodes[i + typeIdx]}>`;
    const self = nodes[i + sizeIdx] || 0;
    nodeCount += 1;
    totalSelf += self;
    let entry = byName.get(name);
    if (!entry) {
      entry = { count: 0, selfSize: 0 };
      byName.set(name, entry);
    }
    entry.count += 1;
    entry.selfSize += self;
  }
  return { nodeCount, totalSelf, byName };
}

function topEntries(byName, n, key) {
  return [...byName.entries()].sort((a, b) => key(b[1]) - key(a[1])).slice(0, n);
}

function table(rows, nameWidth) {
  const header = `name ${" ".repeat(Math.max(0, nameWidth - 8))} count1 count2   dCount  size1MiB size2MiB  dMiB`;
  const lines = [header];
  for (const [name, a, b, dSize, dCount] of rows) {
    lines.push(
      `${name.slice(0, nameWidth).padEnd(nameWidth)} ${String(a.count).padStart(7)} ${String(b.count).padStart(7)} ${String(dCount).padStart(8)} ${String(Math.round(a.selfSize / MiB)).padStart(8)} ${String(Math.round(b.selfSize / MiB)).padStart(8)} ${String(Math.round(dSize / MiB)).padStart(6)}`,
    );
  }
  return lines.join("\n");
}

const a = aggregate(path1);
console.log(`[heap_diff] snap1: ${path1} - ${a.nodeCount} nodes, ${(a.totalSelf / MiB).toFixed(1)} MiB self size`);
const b = aggregate(path2);
console.log(`[heap_diff] snap2: ${path2} - ${b.nodeCount} nodes, ${(b.totalSelf / MiB).toFixed(1)} MiB self size`);

const names = new Set([...a.byName.keys(), ...b.byName.keys()]);
const rows = [];
for (const name of names) {
  const x = a.byName.get(name) ?? { count: 0, selfSize: 0 };
  const y = b.byName.get(name) ?? { count: 0, selfSize: 0 };
  rows.push([name, x, y, y.selfSize - x.selfSize, y.count - x.count]);
}

const growers = rows.filter((row) => row[3] > 0).sort((x, y) => y[3] - x[3]).slice(0, 30);
const shrinkers = rows.filter((row) => row[3] < 0).sort((x, y) => x[3] - y[3]).slice(0, 10);
const biggest = topEntries(b.byName, 15, (entry) => entry.selfSize);

const nameWidth = Math.min(48, Math.max(24, ...growers.map((row) => row[0].length)));
console.log(`\n=== Top ${growers.length} growers (snap1 -> snap2) ===`);
console.log(table(growers, nameWidth));
if (shrinkers.length) {
  console.log(`\n=== Top ${shrinkers.length} shrinkers ===`);
  console.log(table(shrinkers, nameWidth));
}
console.log(`\n=== Largest holders in snap2 (context) ===`);
console.log(
  biggest
    .map(([name, entry]) => `${name.slice(0, nameWidth).padEnd(nameWidth)} ${String(entry.count).padStart(8)} ${String(Math.round(entry.selfSize / MiB)).padStart(8)}`)
    .join("\n"),
);

const out = {
  snap1: { file: path1, nodeCount: a.nodeCount, totalSelfBytes: a.totalSelf },
  snap2: { file: path2, nodeCount: b.nodeCount, totalSelfBytes: b.totalSelf },
  topGrowers: growers.map(([name, x, y, dSize, dCount]) => ({
    name,
    count1: x.count,
    count2: y.count,
    dCount,
    size1Bytes: x.selfSize,
    size2Bytes: y.selfSize,
    dBytes: dSize,
  })),
};
const outFile = path.join(path.dirname(path2), "heap-diff.json");
fs.writeFileSync(outFile, JSON.stringify(out, null, 2));
console.log(`\n[heap_diff] full diff -> ${outFile}`);
