import { computeGraphEdges, computeGraphRows, type GraphRow } from "../src/messageTree";
import type { HarnessMessage } from "../src/harness";

function assert(condition: boolean, message: string): void {
  if (!condition) throw new Error(`graph_test failed: ${message}`);
}

function mk(id: number, parentId: number, role: HarnessMessage["role"] = "user", active = false): HarnessMessage {
  return { id, parentId, role, content: `m${id}`, createdAt: new Date(id * 1000).toISOString(), isActive: active };
}

function byId(rows: GraphRow[]): Map<number, GraphRow> {
  return new Map(rows.map((row) => [row.message.id, row]));
}

// Linear chain: one lane, increasing depths, all rows on the active path.
{
  const rows = computeGraphRows([mk(1, 0, "user", true), mk(2, 1, "assistant", true), mk(3, 2, "user", true)]);
  assert(rows.length === 3, "linear: 3 rows");
  assert(rows.every((row) => row.lane === 0), "linear: single lane");
  assert(rows.map((row) => row.depth).join(",") === "0,1,2", "linear: depths 0,1,2");
  assert(rows.every((row) => row.active && row.lanes === 1), "linear: all active, 1 lane");
}

// Branch: the first child keeps the parent's lane, the sibling takes the next
// lane, and the sibling's child stays on it. Active flags follow the path.
{
  const rows = computeGraphRows([
    mk(1, 0, "user", true),
    mk(2, 1, "assistant", true),
    mk(3, 1, "assistant", false),
    mk(4, 3, "user", false),
  ]);
  assert(rows.length === 4, "branch: 4 rows");
  const map = byId(rows);
  assert(map.get(1)!.lane === 0, "branch: root in lane 0");
  assert(map.get(2)!.lane === 0, "branch: first child keeps parent lane");
  assert(map.get(3)!.lane === 1, "branch: sibling takes lane 1");
  assert(map.get(4)!.lane === 1, "branch: grandchild stays in lane 1");
  assert(map.get(2)!.active && !map.get(3)!.active && !map.get(4)!.active, "branch: active flags follow the path");
  assert(map.get(2)!.lanes === 2 && map.get(3)!.lanes === 2 && map.get(4)!.lanes === 1, "branch: lane count per depth");
}

// Lane reuse: a branch that ends at depth 1 frees its lane, which the next
// sibling at depth 2 reuses instead of growing a third lane.
{
  const rows = computeGraphRows([
    mk(1, 0, "user", true),
    mk(2, 1, "assistant", true),
    mk(3, 1, "assistant", false),
    mk(4, 2, "user", true),
    mk(5, 2, "user", false),
  ]);
  const map = byId(rows);
  assert(map.get(2)!.lane === 0 && map.get(3)!.lane === 1, "reuse: depth 1 in lanes 0 and 1");
  assert(map.get(4)!.lane === 0, "reuse: child of the lane-0 branch stays in lane 0");
  assert(map.get(5)!.lane === 1, "reuse: later sibling reuses the freed lane 1, not lane 2");
}

// Rows come back in depth order (top to bottom), parents before children.
{
  const rows = computeGraphRows([mk(1, 0), mk(2, 1), mk(3, 1), mk(4, 2), mk(5, 3)]);
  const depths = rows.map((row) => row.depth);
  assert(depths.every((depth, index) => index === 0 || depth >= depths[index - 1]), "order: depths non-decreasing");
  assert(byId(rows).get(4)!.depth === 2 && byId(rows).get(5)!.depth === 2, "order: correct depths");
}

// A message whose parent is not in the list degrades to a root, not a drop.
{
  const rows = computeGraphRows([mk(1, 999), mk(2, 1)]);
  assert(rows.length === 2, "orphan: 2 rows");
  assert(byId(rows).get(1)!.depth === 0 && byId(rows).get(2)!.depth === 1, "orphan: treated as root");
}

// A parent cycle (corrupt data) must not drop rows or hang the layout.
{
  const rows = computeGraphRows([mk(1, 2), mk(2, 1)]);
  assert(rows.length === 2, "cycle: both rows survive");
}

// parentLane: roots are null; a child reports its parent's row lane even when
// it takes a different lane itself (the fork point).
{
  const rows = computeGraphRows([
    mk(1, 0, "user", true),
    mk(2, 1, "assistant", true),
    mk(3, 1, "assistant", false),
    mk(4, 3, "user", false),
  ]);
  const map = byId(rows);
  assert(map.get(1)!.parentLane === null, "parentLane: root is null");
  assert(map.get(2)!.parentLane === 0 && map.get(2)!.lane === 0, "parentLane: same-lane child");
  assert(map.get(3)!.parentLane === 0 && map.get(3)!.lane === 1, "parentLane: fork carries the parent's lane");
  assert(map.get(4)!.parentLane === 1, "parentLane: grandchild follows its parent");
}

// Edge view geometry: in/out curves around fork points, and full-height rails
// only where a same-lane chain actually passes between its nodes - no phantom
// rail for a lane reused by an unrelated later branch.
{
  // Tree: 1 <- 2 <- 3 (active main line), 1 <- 4 (dead fork on lane 1),
  // and 2 <- 6 reusing lane 1 after 4 dies.
  const rows = computeGraphRows([
    mk(1, 0, "user", true),
    mk(2, 1, "assistant", true),
    mk(3, 2, "user", true),
    mk(4, 1, "assistant", false),
    mk(6, 2, "assistant", false),
  ]);
  const edges = computeGraphEdges(rows);
  const idx = (id: number) => rows.findIndex((row) => row.message.id === id);
  // Row order: 1, 2, 4, 3, 6 (depth groups: d0, d1, d2).

  // Root 1: no incoming; fans out to 2 and 4 only (6's parent is 2).
  assert(edges[idx(1)].inEdge === null, "edges: root has no incoming edge");
  const outs = edges[idx(1)].outEdges;
  assert(outs.length === 2, "edges: root fans out to its own children");
  assert(outs.some((edge) => edge.toLane === 0 && edge.active), "edges: root -> active child");
  assert(outs.some((edge) => edge.toLane === 1 && !edge.active), "edges: root -> dead lane-1 fork dim");

  // 2 carries both continuations: the active 2 -> 3 and the dim 2 -> 6 curve.
  const out2 = edges[idx(2)].outEdges;
  assert(out2.length === 2, "edges: branching node fans out to both children");

  // 3 curves in from lane 0 and is active; 4/6 curve in from lane 0, dim.
  const in3 = edges[idx(3)].inEdge;
  assert(in3 !== null && in3.fromLane === 0 && in3.active, "edges: main-line child inherits active");
  const in4 = edges[idx(4)].inEdge;
  assert(in4 !== null && in4.fromLane === 0 && !in4.active, "edges: fork curves in from the parent lane, dim");
  const in6 = edges[rows.length - 1].inEdge;
  assert(in6 !== null && in6.fromLane === 0 && !in6.active, "edges: reuse branch curves in from its parent lane, dim");

  // The 2 -> 6 chain rails lane 0 across the gap rows; the 2 -> 3 chain adds
  // its active rail over the shared lane where they bundle (row of 4, which
  // sits between 2 and 3). No lane-1 rail may appear anywhere.
  assert(edges[idx(4)].through.get(0) === true, "edges: lane-0 rail crosses the dead fork's row (bundled, active)");
  assert(edges[idx(3)].through.get(0) === false, "edges: below its node only the dim continue-to-reuse chain passes");
  assert(edges.every((view) => ![...view.through.keys()].includes(1)), "edges: reused lane draws no phantom rail");
}

// A same-lane chain spanning an intermediate row lights exactly that row.
{
  // Tree: 1 <- 2 <- 3 on lane 0; 1 <- 4 on lane 1. Row order 1, 2, 4, 3;
  // the 2 -> 3 rail must light message 4's row.
  const rows = computeGraphRows([
    mk(1, 0, "user", true),
    mk(2, 1, "assistant", true),
    mk(4, 1, "assistant", false),
    mk(3, 2, "user", true),
  ]);
  const edges = computeGraphEdges(rows);
  const idx = (id: number) => rows.findIndex((row) => row.message.id === id);
  assert(edges[idx(4)].through.get(0) === true, "edges: intermediate row carries the through rail, active");

  // Same layout with the deep child inactive: the rail stays dim.
  const rowsDim = computeGraphRows([
    mk(1, 0, "user", true),
    mk(2, 1, "assistant", true),
    mk(4, 1, "assistant", false),
    mk(3, 2, "user", false),
  ]);
  const dimIdx = rowsDim.findIndex((row) => row.message.id === 4);
  assert(computeGraphEdges(rowsDim)[dimIdx].through.get(0) === false,
    "edges: intermediate row rail follows the child's active flag");
}

// Orphans and cycles must not draw geometry pointing past the row.
{
  const orphanEdges = computeGraphEdges(computeGraphRows([mk(7, 999)]));
  assert(orphanEdges[0].inEdge === null && orphanEdges[0].outEdges.length === 0, "edges: orphan draws nothing");
  const cycleEdges = computeGraphEdges(computeGraphRows([mk(1, 2), mk(2, 1)]));
  assert(cycleEdges.every((view) => view.inEdge === null || view.inEdge.fromLane >= 0), "edges: cycle edge view is total");
}

// Empty input.
assert(computeGraphRows([]).length === 0, "empty: no rows");
assert(computeGraphEdges([]).length === 0, "empty: no edge views");

console.log("graph_test passed");
