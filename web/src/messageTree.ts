// Pure message-tree helpers shared by the chat view and the client tests.
// Kept out of App.tsx so the component file only exports components.
import type { HarnessEvent, HarnessMessage } from "./harness";

// Negative ids so live-streamed messages never collide with persisted message
// ids and always sort after them as the active leaf.
export function tempMessageId(seed: string): number {
  let hash = 0;
  for (const character of seed) hash = (hash * 31 + character.charCodeAt(0)) | 0;
  return -(Math.abs(hash) || 1);
}

export function getActivePath(messages: HarnessMessage[]): HarnessMessage[] {
  const byId = new Map<number, HarnessMessage>();
  const activeParents = new Set<number>();
  for (const message of messages) {
    byId.set(message.id, message);
    if (message.isActive && message.parentId) activeParents.add(message.parentId);
  }
  // The active leaf is the active message that is not the parent of another
  // active message. This is robust to negative temp ids, which Math.max would
  // skip. Fall back to the highest active id (and then the last message) for
  // legacy data without is_active flags.
  let leafId = 0;
  let highestActive = 0;
  for (const message of messages) {
    if (!message.isActive) continue;
    if (message.id > highestActive) highestActive = message.id;
    if (!activeParents.has(message.id)) leafId = message.id;
  }
  if (!leafId) leafId = highestActive;
  if (!leafId) leafId = messages[messages.length - 1]?.id ?? 0;
  // Walk parents to the root. A seen-set breaks parent cycles, which a
  // numeric cap would silently render as thousands of duplicate bubbles.
  // A dangling parent (an id in state that no longer exists) truncates the
  // walk above the orphan: log the exact id so the client truth-model bugs
  // surface with concrete evidence in the console instead of only as a
  // missing transcript.
  const path: HarnessMessage[] = [];
  const seen = new Set<number>();
  let currentId = leafId;
  while (currentId && !seen.has(currentId)) {
    const node = byId.get(currentId);
    if (!node) {
      console.warn(
        `[harness] transcript lineage broken: id=${currentId} is referenced as a parent but not present in state (leaf=${leafId}, chain=${path.map((m) => m.id).join(" -> ")})`,
      );
      break;
    }
    seen.add(currentId);
    path.unshift(node);
    currentId = node.parentId;
  }
  return path;
}

export function getActiveLeaf(messages: HarnessMessage[]): number {
  const path = getActivePath(messages);
  return path.length ? path[path.length - 1].id : 0;
}

// Rebuild the assistant message of an in-flight run after a page reload. The
// daemon persists the message row only at run end, so a refresh mid-stream
// would otherwise drop everything streamed before the reload (its deltas live
// in the event ledger, not the message list). Later live deltas find this
// message by run id and append to it, exactly like a fresh upsert.
export function seedAssistantFromRun(
  previous: HarnessMessage[],
  runId: string,
  reasoning: string,
  content: string,
): HarnessMessage[] {
  if (!runId) return previous;
  const index = previous.findIndex(
    (message) => message.role === "assistant" && message.runId === runId,
  );
  if (index >= 0) {
    const next = [...previous];
    next[index] = {
      ...next[index],
      content: content || next[index].content,
      reasoning: reasoning || next[index].reasoning,
    };
    return next;
  }
  return [
    ...previous,
    {
      id: tempMessageId(`seed-${runId}`),
      parentId: getActiveLeaf(previous) ?? 0,
      role: "assistant",
      content,
      createdAt: new Date().toISOString(),
      runId,
      reasoning: reasoning || undefined,
      isActive: true,
    },
  ];
}

// Link a live assistant message into the active branch. Existing messages for
// the same run are updated in place so their parent link is preserved; a new
// message uses the tracked pending parent, falling back to the active leaf.
export function upsertAssistant(
  previous: HarnessMessage[],
  event: HarnessEvent,
  pendingParents: Record<string, number>,
  reasoningDelta: string | undefined,
  contentDelta: string,
): HarnessMessage[] {
  // Match the assistant message only: temp user messages also carry the run
  // id (edit and queued flows), and streamed deltas must never land on them.
  const index = previous.findIndex(
    (message) => message.role === "assistant" && message.runId === event.runId,
  );
  if (index >= 0) {
    const next = [...previous];
    const existing = next[index];
    const pendingParent = pendingParents[event.runId];
    next[index] = {
      ...existing,
      ...(existing.id < 0 && pendingParent !== undefined
        ? { parentId: pendingParent }
        : {}),
      content: contentDelta ? existing.content + contentDelta : existing.content,
      reasoning: reasoningDelta
        ? `${existing.reasoning || ""}${reasoningDelta}`
        : existing.reasoning,
    };
    return next;
  }
  // Parent candidates must EXIST in the latest state: a stale pending or
  // optimistic-parent id (swapped out by an earlier merge) would otherwise
  // create a row parented on a temp that no longer exists, and the
  // active-path walk would truncate the whole lineage above it. Fall back
  // through the last temp user row, then to the active leaf.
  const parentExists = (id: number) => previous.some((message) => message.id === id);
  const pendingUser = [...previous]
    .reverse()
    .find((message) => message.role === "user" && message.id < 0);
  const pendingParent = pendingParents[event.runId];
  const pendingUserId = pendingUser ? pendingUser.id : 0;
  const activeLeaf = getActiveLeaf(previous) ?? 0;
  const parentId =
    [pendingParent, pendingUserId, activeLeaf].find(
      (candidate) => candidate !== undefined && candidate !== 0 && parentExists(candidate),
    ) ?? 0;
  const base: HarnessMessage = {
    id: tempMessageId(event.eventId),
    parentId,
    role: "assistant",
    content: contentDelta,
    createdAt: event.emittedAt,
    runId: event.runId,
    reasoning: reasoningDelta,
    isActive: true,
  };
  return [...previous, base];
}

// One row of the session graph: a message with its lane (horizontal branch
// position) and depth (vertical position), git-graph style.
export interface GraphRow {
  message: HarnessMessage;
  lane: number;
  depth: number;
  /** True when the message sits on the active path. */
  active: boolean;
  /** Lanes occupied at this depth; >1 marks a branch point. */
  lanes: number;
  /** Lane of the parent's row; null for roots (and missing parents). */
  parentLane: number | null;
}

// Lay the message tree out as a git-graph: rows in depth order, one lane per
// branch. A child keeps its parent's lane when that lane is free at the child's
// depth and takes the leftmost free lane otherwise, so a branch stays visually
// attached to its fork point and lanes freed by a finished branch are reused
// by later siblings. Defensive second pass: a cycle (corrupt parent links) de-
// grades to a new lane at depth 0 instead of dropping rows from the graph.
export function computeGraphRows(messages: HarnessMessage[]): GraphRow[] {
  const byId = new Map<number, HarnessMessage>();
  for (const message of messages) byId.set(message.id, message);
  const children = new Map<number, HarnessMessage[]>();
  const roots: HarnessMessage[] = [];
  for (const message of messages) {
    const parent = message.parentId ? byId.get(message.parentId) : undefined;
    if (!parent) {
      roots.push(message);
      continue;
    }
    const list = children.get(parent.id);
    if (list) list.push(message);
    else children.set(parent.id, [message]);
  }
  const depth = new Map<number, number>();
  const ordered: HarnessMessage[] = [];
  const visit = (message: HarnessMessage, level: number) => {
    if (depth.has(message.id)) return;
    depth.set(message.id, level);
    ordered.push(message);
    for (const child of children.get(message.id) ?? []) visit(child, level + 1);
  };
  for (const root of roots) visit(root, 0);
  for (const message of messages) if (!depth.has(message.id)) visit(message, 0);

  // Order siblings by parent lane (then id) so continuations are assigned
  // before new branches and the layout stays stable across re-renders.
  const activeIds = new Set(getActivePath(messages).map((message) => message.id));
  const laneOf = new Map<number, number>();
  const maxDepth = ordered.reduce((max, message) => Math.max(max, depth.get(message.id) ?? 0), 0);
  const rows: GraphRow[] = [];
  for (let d = 0; d <= maxDepth; d++) {
    const atDepth = ordered.filter((message) => (depth.get(message.id) ?? 0) === d);
    atDepth.sort((a, b) => {
      const la = laneOf.get(a.parentId) ?? -1;
      const lb = laneOf.get(b.parentId) ?? -1;
      return la !== lb ? la - lb : a.id - b.id;
    });
    const occupied = new Set<number>();
    const depthRows = atDepth.map((message) => {
      const parentLane = laneOf.get(message.parentId);
      let lane = parentLane !== undefined && !occupied.has(parentLane) ? parentLane : 0;
      while (occupied.has(lane)) lane++;
      laneOf.set(message.id, lane);
      occupied.add(lane);
      return { message, lane, depth: d, active: activeIds.has(message.id), lanes: 0, parentLane: parentLane ?? null };
    });
    for (const row of depthRows) row.lanes = occupied.size;
    rows.push(...depthRows);
  }
  return rows;
}

// Per-row edge view for the graph renderer: which lanes have a real parent→
// child chain passing through the row (full-height rails), what comes in from
// the parent (top half), and what leaves toward children (bottom half). Rails
// are derived from actual edges, so a lane reused by an unrelated later
// branch draws no phantom line across the gap. Pure so the client test pins
// the geometry the SVG render consumes.
export interface GraphEdgeView {
  /** Lane -> active, for each lane whose chain passes strictly through this row. */
  through: Map<number, boolean>;
  /** Incoming edge: parent lane -> this row's lane, drawn in the top half. */
  inEdge: { fromLane: number; active: boolean } | null;
  /** Outgoing edges toward children, drawn in the bottom half. */
  outEdges: { fromLane: number; toLane: number; active: boolean }[];
}

export function computeGraphEdges(rows: GraphRow[]): GraphEdgeView[] {
  const indexById = new Map<number, number>();
  rows.forEach((row, index) => indexById.set(row.message.id, index));
  const views: GraphEdgeView[] = rows.map(() => ({ through: new Map<number, boolean>(), inEdge: null, outEdges: [] }));
  rows.forEach((row, index) => {
    if (row.parentLane === null) return;
    views[index].inEdge = { fromLane: row.parentLane, active: row.active };
    const parentIndex = indexById.get(row.message.parentId);
    // Defensive: a corrupt parent link (cycle) may point past this row; skip
    // the geometry rather than drawing backwards.
    if (parentIndex === undefined || parentIndex >= index) return;
    const parentLane = rows[parentIndex].lane;
    views[parentIndex].outEdges.push({ fromLane: parentLane, toLane: row.lane, active: row.active });
    // The gap between parent and child rails at the FROM lane: the chain
    // leaves the parent's node in its lane and only curves into the child's
    // lane in the child's row (a cross-lane edge's line stays at the parent
    // lane across the gap). Paint order in the renderer resolves overlap
    // with a different chain sharing the lane (active paints last).
    for (let k = parentIndex + 1; k < index; k++) {
      const through = views[k].through;
      through.set(parentLane, through.get(parentLane) || row.active);
    }
  });
  return views;
}

// @-mentioned paths in a user message render as file chips (inline code),
// matching how the assistant shows file references. Mirrors the daemon's
// hand-typed mention scan exactly: an @token up to whitespace/sentence
// punctuation, absolute paths only (no colon in the stop set - Windows drive
// paths need C:). Composer-inserted spans ship as explicit mentions, so this
// only styles what the model also picks up from the text.
const MENTION_STOPS = " \t\n\r,;!?)>]'";
export function chipifyMentions(content: string): string {
  if (!content.includes("@")) return content;
  let out = "";
  let i = 0;
  for (;;) {
    const at = content.indexOf("@", i);
    if (at < 0) {
      out += content.slice(i);
      break;
    }
    let j = at + 1;
    while (j < content.length && !MENTION_STOPS.includes(content[j])) j++;
    const token = content.slice(at + 1, j);
    const absolute =
      token.length >= 3 &&
      (token.startsWith("/") || /^[A-Za-z]:[\\//]/.test(token));
    if (absolute && !token.includes("`")) {
      out += content.slice(i, at) + "`" + token + "`";
      i = j;
    } else {
      out += content.slice(i, at + 1);
      i = at + 1;
    }
  }
  return out;
}

// A mid-stream reload must not drop the live turn: the daemon persists the
// assistant message only when the run ends, so a snapshot fetched while the
// run is streaming carries the user message but not the in-flight answer.
// Local temp messages (negative ids) survive only while their run is still
// live; where the snapshot already carries the same run + role, the longer
// content wins (the snapshot can lag the deltas this client already applied).
export function mergeSnapshotMessages(
  local: HarnessMessage[],
  snapshot: HarnessMessage[],
  liveRunIds: ReadonlySet<string>,
  excludeRunIds: ReadonlySet<string>,
  parentSwaps?: Map<number, number>,
): HarnessMessage[] {
  const merged = snapshot.filter(
    (message) => !message.runId || !excludeRunIds.has(message.runId),
  );
  const indexByKey = new Map<string, number>();
  const indexById = new Map<number, number>();
  const indexByClientId = new Map<number, number>();
  merged.forEach((message, index) => {
    indexById.set(message.id, index);
    if (message.clientId) indexByClientId.set(message.clientId, index);
    if (message.runId) indexByKey.set(`${message.runId}:${message.role}`, index);
  });
  // A temp row that the snapshot replaced (its persisted counterpart lives
  // in the snapshot now): live siblings pointing at the dropped temp id must
  // be re-pointed at the persisted row, or the active-path walk dangles at
  // the missing temp and the history above it vanishes from the rendered
  // transcript. ANY role qualifies: a group session chains every turn's temp
  // row onto the previous turn's temp row, so an assistant temp can be the
  // parent of other live temps too (the swap map was user-only, and the
  // group transcript vanished from the top once per reload mid-run).
  const replacedIds = new Map<number, number>();
  for (const message of local) {
    if (message.id >= 0 || !message.runId) continue;
    const key = `${message.runId}:${message.role}`;
    // A-7: match by the client anchor first. The optimistic row's clientId
    // is its temp id; the daemon stores it on the persisted row, so the
    // pair dedupes even when the run id mapping is lost (reconnect
    // mid-flight, provisional id race). Exact id (post-rename) and
    // run id + role (legacy daemon) are the fallbacks.
    let index = message.clientId !== undefined
      ? indexByClientId.get(message.clientId)
      : undefined;
    if (index === undefined) index = indexById.get(message.id);
    if (index === undefined) index = indexByKey.get(key);
    const resolveParent = (
      parentId: number,
    ): number => {
      const swapped = replacedIds.get(parentId);
      return swapped === undefined ? parentId : swapped;
    };
    if (index === undefined) {
      // Not in the snapshot and not a live run: the row was trimmed or
      // otherwise gone - do not resurrect it.
      if (!liveRunIds.has(message.runId)) continue;
      const parentId = resolveParent(message.parentId);
      merged.push(parentId === message.parentId ? message : { ...message, parentId });
      indexByKey.set(key, merged.length - 1);
      continue;
    }
    const existing = merged[index];
    // The persisted row replaced this temp: register the swap REGARDLESS of
    // liveness - a completed group turn's temp must swap too, because the
    // live turn temps chain onto it (the swap map was live-only and the
    // group transcript collapsed to the last turn once per reload). The
    // persisted row wins the merge; the longer-content winner keeps the
    // streamed tail only for live rows, where the snapshot may lag a delta.
    replacedIds.set(message.id, existing.id);
    // Callers that hold parent ids across merges (the pending-parent map in
    // App) record the same swaps so their pointers follow the swap instead
    // of staying attached to a temp id that no longer exists.
    parentSwaps?.set(message.id, existing.id);
    if (!liveRunIds.has(message.runId)) continue;
    if (
      (message.role === "user" && message.content.length > existing.content.length) ||
      (message.role !== "user" && (
        message.content.length > existing.content.length ||
        (message.reasoning !== undefined &&
          message.reasoning.length > (existing.reasoning || "").length)
      ))
    ) {
      const localParent = resolveParent(message.parentId);
      merged[index] = {
        ...existing,
        content: message.content,
        reasoning: message.reasoning || existing.reasoning,
        // A local parent that differs from the persisted row's is newer
        // knowledge (a mid-stream steer re-parented the in-flight answer
        // after the row was written) - the active path must follow it.
        ...(localParent !== existing.parentId ? { parentId: localParent } : {}),
      };
    }
  }
  return merged;
}

// The optimistic-snapshot variant used while a send is in flight (the
// optimistic user row is still the parent of the streamed turn).
// parentSwaps, when provided, receives every temp-to-persisted swap
// performed during the merge so callers can re-point cross-merge state
// (the pending-parent map).
export function mergeOptimisticSnapshot(
  local: HarnessMessage[],
  snapshot: HarnessMessage[],
  liveRunIds: ReadonlySet<string>,
  queuedRunIds: ReadonlySet<string>,
  optimisticTurn: { user: HarnessMessage } | null,
  parentSwaps?: Map<number, number>,
): HarnessMessage[] {
  const snapshotMessages = snapshot.filter(
    (message) => !message.runId || !queuedRunIds.has(message.runId),
  );
  if (!optimisticTurn) return mergeSnapshotMessages(local, snapshot, liveRunIds, queuedRunIds, parentSwaps);
  const preservedRunIds = new Set(liveRunIds);
  if (optimisticTurn.user.runId) preservedRunIds.add(optimisticTurn.user.runId);
  const localMessages = local.some((message) => message.id === optimisticTurn.user.id)
    ? local
    : [...local, optimisticTurn.user];
  const merged = mergeSnapshotMessages(localMessages, snapshot, preservedRunIds, queuedRunIds, parentSwaps);
  const persistedUser = snapshotMessages.find((message) => message.role === "user" && message.id > 0);
  // Keep the temporary user row as the live assistant's parent until the run
  // is terminal. The surviving-temp check is load-bearing: when the swap
  // already replaced the temp with the persisted row, the optimistic temp is
  // GONE from merged - dropping the persisted row too would orphan the
  // streamed turn entirely (transcript shows only the working pill until the
  // run completes and a later reload restores the row). In that case the
  // persisted row stays: the merge re-pointed live children onto it.
  const tempUserSurvived = merged.some((message) => message.id === optimisticTurn.user.id);
  return persistedUser && tempUserSurvived
    ? merged.filter((message) => message.id !== persistedUser.id)
    : merged;
}

