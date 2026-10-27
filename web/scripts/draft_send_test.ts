// Regression test for the draft (new conversation) first send.
//
// "New conversation" opens a sessionless draft; the first send creates the
// session row, adds an optimistic temp user row, and races three async
// events: the send ack (A-7 rename), the session-load snapshot (fired by the
// currentSessionId change), and the SSE delta stream. Each ordering must keep
// the user row on the active path for the whole run.
import { getActivePath, mergeOptimisticSnapshot, tempMessageId, upsertAssistant } from "../src/messageTree";
import type { HarnessEvent, HarnessMessage } from "../src/harness";

function fail(message: string): never {
  throw new Error(`draft_send_test failed: ${message}`);
}

function checkUserVisible(label: string, messages: HarnessMessage[]): void {
  const path = getActivePath(messages);
  const user = path.find((m) => m.role === "user");
  if (!user) {
    fail(`${label}: no user row on the active path: ${path.map((m) => m.id).join(",")}`);
  }
}

// Shared shape: the optimistic row and the persisted twin.
const tempId = tempMessageId("user-1700000000000");
const provisionalRun = "pending-1700000000000";
const daemonRun = "run-179-1";
const rowId = 701;

function optimisticUser(): HarnessMessage {
  return { id: tempId, parentId: 0, role: "user", content: "hi", createdAt: "t", runId: provisionalRun, isActive: true, clientId: tempId };
}
function persistedUser(): HarnessMessage {
  return { id: rowId, parentId: 0, role: "user", content: "hi", createdAt: "t", runId: daemonRun, isActive: true, clientId: tempId };
}
function deltaEvent(sequence: number, text: string): HarnessEvent {
  return {
    eventId: `evt-${sequence}`,
    sessionId: "session-1",
    runId: daemonRun,
    sequence,
    type: "text.delta",
    emittedAt: "t",
    payload: { text },
  };
}

// ── Scenario A: the snapshot lands after the row write but before the ack ──
// Ordering: optimistic row added -> snapshot (with the persisted row) merge ->
// ack rename -> deltas -> mid-run merge -> completed reload.
{
  let messages: HarnessMessage[] = [];
  const optimisticTurn: { user: HarnessMessage } = { user: optimisticUser() };
  const localRunIds = new Set<string>([provisionalRun]);
  const pendingParents: Record<string, number> = { [provisionalRun]: tempId };
  const queuedRunIds = new Set<string>();

  messages = [...messages, optimisticUser()];
  checkUserVisible("A: after optimistic add", messages);

  // Snapshot #1: the daemon already stored the row (start_run persists before
  // responding), the ack has not reached the client yet.
  const parentSwaps1 = new Map<number, number>();
  messages = mergeOptimisticSnapshot(messages, [persistedUser()], new Set(localRunIds), queuedRunIds, optimisticTurn, parentSwaps1);
  checkUserVisible("A: after snapshot before ack", messages);

  // The ack: rename maps id === tempId - the row was already replaced, so the
  // rename is a no-op; the optimistic turn and the pending parent follow.
  if (optimisticTurn.user.id !== tempId) fail("A: precondition");
  optimisticTurn.user = { ...optimisticTurn.user, id: rowId, runId: daemonRun };
  for (const [from, to] of parentSwaps1) {
    for (const key of Object.keys(pendingParents)) {
      if (pendingParents[key] === from) pendingParents[key] = to;
    }
  }
  localRunIds.delete(provisionalRun);
  localRunIds.add(daemonRun);
  pendingParents[daemonRun] = rowId;

  // Deltas stream (pre-ack in the real client is possible; here post-ack).
  for (let i = 1; i <= 3; i += 1) {
    messages = upsertAssistant(messages, deltaEvent(i, `d${i} `), pendingParents, undefined, `d${i} `);
    checkUserVisible(`A: delta ${i}`, messages);
  }

  // Mid-run merge (effect re-run / reconnect): the optimistic turn is alive.
  messages = mergeOptimisticSnapshot(messages, [persistedUser()], new Set(localRunIds), queuedRunIds, optimisticTurn);
  checkUserVisible("A: mid-run merge", messages);

  // Run completes: the optimistic turn clears, then the reload merges with
  // the persisted assistant row. Live-run ids no longer contain the run.
  messages = mergeOptimisticSnapshot(messages, [persistedUser(), { id: 702, parentId: rowId, role: "assistant", content: "d1 d2 d3 ", createdAt: "t", runId: daemonRun, isActive: true }], new Set(), queuedRunIds, null);
  checkUserVisible("A: completed reload", messages);
  const path = getActivePath(messages);
  if (path.some((m) => m.id < 0)) fail(`A: temp rows must drop after completion: ${path.map((m) => m.id).join(",")}`);
}

// ── Scenario B: the snapshot races ahead of the row write (empty snapshot) ──
{
  let messages: HarnessMessage[] = [];
  const optimisticTurn: { user: HarnessMessage } = { user: optimisticUser() };
  const localRunIds = new Set<string>([provisionalRun]);
  const pendingParents: Record<string, number> = { [provisionalRun]: tempId };
  const queuedRunIds = new Set<string>();

  messages = [...messages, optimisticUser()];
  checkUserVisible("B: after optimistic add", messages);

  // Snapshot #1: fetched before the daemon stored the row - empty.
  messages = mergeOptimisticSnapshot(messages, [], new Set(localRunIds), queuedRunIds, optimisticTurn);
  checkUserVisible("B: after empty snapshot", messages);

  // The ack renames the optimistic row onto the daemon's row id.
  messages = messages.map((m) => m.id === tempId ? { ...m, id: rowId, runId: daemonRun } : m.parentId === tempId ? { ...m, parentId: rowId } : m);
  optimisticTurn.user = { ...optimisticTurn.user, id: rowId, runId: daemonRun };
  localRunIds.delete(provisionalRun);
  localRunIds.add(daemonRun);
  pendingParents[daemonRun] = rowId;

  for (let i = 1; i <= 3; i += 1) {
    messages = upsertAssistant(messages, deltaEvent(i, `d${i} `), pendingParents, undefined, `d${i} `);
    checkUserVisible(`B: delta ${i}`, messages);
  }

  // Snapshot #2 mid-run: the row write has happened by now.
  messages = mergeOptimisticSnapshot(messages, [persistedUser()], new Set(localRunIds), queuedRunIds, optimisticTurn);
  checkUserVisible("B: mid-run merge", messages);

  messages = mergeOptimisticSnapshot(messages, [persistedUser(), { id: 702, parentId: rowId, role: "assistant", content: "d1 d2 d3 ", createdAt: "t", runId: daemonRun, isActive: true }], new Set(), queuedRunIds, null);
  checkUserVisible("B: completed reload", messages);
}

// ── Scenario C: deltas stream before the ack lands (slow ack, fast stream) ──
// The SSE opened after snapshot #1; deltas arrive while the send ack is still
// in flight. upsertAssistant must parent the streamed turn onto the (temp)
// optimistic user row, and the later ack rename must re-point children.
{
  let messages: HarnessMessage[] = [];
  const optimisticTurn: { user: HarnessMessage } = { user: optimisticUser() };
  const localRunIds = new Set<string>([provisionalRun]);
  const pendingParents: Record<string, number> = { [provisionalRun]: tempId };
  const queuedRunIds = new Set<string>();

  messages = [...messages, optimisticUser()];
  checkUserVisible("C: after optimistic add", messages);

  // Snapshot #1 with the persisted row; the optimistic row is still temp.
  messages = mergeOptimisticSnapshot(messages, [persistedUser()], new Set(localRunIds), queuedRunIds, optimisticTurn);
  checkUserVisible("C: after snapshot before ack", messages);

  // Deltas BEFORE the ack: pendingParents has no entry for the daemon run id
  // yet, so upsertAssistant falls back to the temp user row / active leaf.
  for (let i = 1; i <= 3; i += 1) {
    messages = upsertAssistant(messages, deltaEvent(i, `d${i} `), pendingParents, undefined, `d${i} `);
    checkUserVisible(`C: pre-ack delta ${i}`, messages);
  }

  // The ack: no rename (the temp row was already swapped for rowId), the
  // pending parent for the daemon run is set now.
  optimisticTurn.user = { ...optimisticTurn.user, id: rowId, runId: daemonRun };
  pendingParents[daemonRun] = rowId;
  localRunIds.delete(provisionalRun);
  localRunIds.add(daemonRun);

  // Mid-run merge: the streamed turn must survive and stay parented at the
  // persisted user row.
  messages = mergeOptimisticSnapshot(messages, [persistedUser()], new Set(localRunIds), queuedRunIds, optimisticTurn);
  checkUserVisible("C: mid-run merge", messages);
  const midPath = getActivePath(messages);
  const userIndex = midPath.findIndex((m) => m.role === "user");
  if (userIndex !== 0) fail(`C: user row must lead the active path: ${midPath.map((m) => m.id).join(",")}`);
  if (midPath.length < 2) fail(`C: streamed turn lost from the active path: ${midPath.map((m) => m.id).join(",")}`);
}

console.info("draft_send_test passed");
