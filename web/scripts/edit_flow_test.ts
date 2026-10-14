// Regression test for the edit-flow cascade.
//
// After an edit, the temp user message carries the run id and is added before
// the temp assistant. Streamed deltas must land on the assistant message only,
// and a parent cycle must not explode the active path into duplicates.
import { chipifyMentions, getActivePath, mergeOptimisticSnapshot, mergeSnapshotMessages, upsertAssistant } from "../src/messageTree";
import type { HarnessEvent, HarnessMessage } from "../src/harness";

function fail(message: string): never {
  throw new Error(`edit_flow_test failed: ${message}`);
}

const runId = "run-test-1";
const oldUser: HarnessMessage = { id: 1, parentId: 0, role: "user", content: "hello", createdAt: "t", runId: "run-old", isActive: false };
const oldAssistant: HarnessMessage = { id: 2, parentId: 1, role: "assistant", content: "Hello!", createdAt: "t", runId: "run-old", isActive: false };
const userTemp: HarnessMessage = { id: -100, parentId: 0, role: "user", content: "hi", createdAt: "t", runId, isActive: true };
const assistantTemp: HarnessMessage = { id: -200, parentId: -100, role: "assistant", content: "", createdAt: "t", runId, isActive: true };

const previous = [oldUser, oldAssistant, userTemp, assistantTemp];
const event: HarnessEvent = {
  eventId: "evt-1",
  sessionId: "session-1",
  runId,
  sequence: 1,
  type: "text.delta",
  emittedAt: "t",
  payload: { text: "Hi there!" },
};

const next = upsertAssistant(previous, event, { [runId]: -100 }, "reasoning text", "Hi there!");
const byId = new Map(next.map((message) => [message.id, message]));

if (byId.get(-100)?.content !== "hi") fail(`user temp mutated: ${JSON.stringify(byId.get(-100)?.content)}`);
if (byId.get(-200)?.content !== "Hi there!") fail(`assistant temp did not receive the delta: ${JSON.stringify(byId.get(-200)?.content)}`);
if (byId.get(-200)?.reasoning !== "reasoning text") fail("assistant temp did not receive the reasoning");
if (byId.get(-200)?.parentId !== -100) fail(`assistant temp re-parented to ${byId.get(-200)?.parentId}`);
if (next.length !== previous.length) fail(`message count changed: ${previous.length} -> ${next.length}`);

const path = getActivePath(next);
if (path.length !== 2) fail(`active path has ${path.length} messages, expected 2`);
if (path[0].id !== -100 || path[1].id !== -200) fail(`active path order wrong: ${path.map((m) => m.id).join(", ")}`);

// A second delta appends to the same assistant message instead of forking.
const second = upsertAssistant(next, { ...event, eventId: "evt-2", sequence: 2, payload: { text: " How's it going?" } }, { [runId]: -100 }, undefined, " How's it going?");
if (second.length !== previous.length) fail(`second delta added a message: ${second.length}`);
if (second.find((m) => m.id === -200)?.content !== "Hi there! How's it going?") fail("second delta did not append");

// Reasoning deltas append without requiring the caller to retain the full
// accumulated trace for every queued state update.
const reasoningSecond = upsertAssistant(
  next,
  { ...event, eventId: "evt-reasoning-2", sequence: 3, type: "reasoning.delta", payload: { text: " more" } },
  { [runId]: -100 },
  " more",
  "",
);
if (reasoningSecond.find((m) => m.id === -200)?.reasoning !== "reasoning text more") fail("reasoning delta did not append");

// Cycle safety: a self-parent must not produce thousands of duplicates.
const cyclic: HarnessMessage[] = [{ id: 5, parentId: 5, role: "user", content: "loop", createdAt: "t", isActive: true }];
if (getActivePath(cyclic).length !== 1) fail("parent cycle exploded the active path");

// ── mergeSnapshotMessages: a mid-stream reload must not drop the live turn ──
const liveRun = "run-live";
const localLive: HarnessMessage[] = [
  { id: 1, parentId: 0, role: "user", content: "old", createdAt: "t", runId: "run-old", isActive: false },
  { id: -100, parentId: 0, role: "user", content: "hi", createdAt: "t", runId: liveRun, isActive: true },
  { id: -200, parentId: -100, role: "assistant", content: "partial answer", createdAt: "t", runId: liveRun, isActive: true },
];
// The daemon persists the user message at send time but the assistant only
// when the run ends, so a mid-stream snapshot lacks the in-flight answer.
const snapshotMidStream: HarnessMessage[] = [
  { id: 1, parentId: 0, role: "user", content: "old", createdAt: "t", runId: "run-old", isActive: false },
  { id: 10, parentId: 1, role: "user", content: "hi", createdAt: "t", runId: liveRun, isActive: true },
];

// While the run is live: the temp assistant survives; the temp user is
// replaced by its persisted counterpart.
let merged = mergeSnapshotMessages(localLive, snapshotMidStream, new Set([liveRun]), new Set());
if (merged.length !== 3) fail(`mid-stream merge count: ${merged.length}`);
if (merged.find((m) => m.id === 10)?.content !== "hi") fail("persisted user message missing after merge");
const liveAssistant = merged.find((m) => m.role === "assistant" && m.runId === liveRun);
if (!liveAssistant || liveAssistant.content !== "partial answer") fail(`live assistant lost in mid-stream reload: ${JSON.stringify(liveAssistant)}`);
if (merged.some((m) => m.id === -100)) fail("temp user message should be replaced by the persisted one");
// The live assistant pointed at the dropped temp user: it must be re-pointed
// at the persisted row, or the active-path walk dangles and the history
// above it vanishes from the rendered transcript.
if (liveAssistant.parentId !== 10) fail(`live assistant parent not re-pointed at the persisted user (${liveAssistant.parentId})`);
if (getActivePath(merged).map((m) => m.id).join(",") !== "1,10,-200") {
  fail(`mid-stream merge dropped the history: ${getActivePath(merged).map((m) => m.id).join(",")}`);
}

// Once the run is no longer live, the snapshot is authoritative and the
// temp messages drop (e.g. a cancelled queued run's withdrawn user message).
merged = mergeSnapshotMessages(localLive, snapshotMidStream, new Set(), new Set());
if (merged.length !== 2) fail(`post-end merge count: ${merged.length}`);
if (merged.some((m) => m.id < 0)) fail("temp messages must drop once the run is no longer live");

// A stale snapshot (behind the deltas this client applied) keeps the longer
// content on the persisted message.
const staleSnapshot: HarnessMessage[] = [
  ...snapshotMidStream,
  { id: 11, parentId: 10, role: "assistant", content: "short", createdAt: "t", runId: liveRun, isActive: true },
];
merged = mergeSnapshotMessages(localLive, staleSnapshot, new Set([liveRun]), new Set());
const staleAssistant = merged.find((m) => m.id === 11);
if (!staleAssistant || staleAssistant.content !== "partial answer") fail(`stale snapshot content not upgraded: ${JSON.stringify(staleAssistant)}`);

// A mid-stream steer re-parents the in-flight answer onto the steered
// message AFTER the persisted row was written: the local parent is newer
// knowledge and must win the merge, so the active path passes through the
// steer (prompt -> steer -> answer, not prompt -> answer with the steer
// stranded as a sibling).
const steerScenario: HarnessMessage[] = [
  { id: 1, parentId: 0, role: "user", content: "old", createdAt: "t", runId: "run-old", isActive: false },
  { id: 10, parentId: 1, role: "user", content: "hi", createdAt: "t", runId: liveRun, isActive: true },
  { id: -200, parentId: 12, role: "assistant", content: "partial answer", createdAt: "t", runId: liveRun, isActive: true },
];
const steerSnapshot: HarnessMessage[] = [
  { id: 1, parentId: 0, role: "user", content: "old", createdAt: "t", runId: "run-old", isActive: false },
  { id: 10, parentId: 1, role: "user", content: "hi", createdAt: "t", runId: liveRun, isActive: true },
  { id: 12, parentId: 10, role: "user", content: "steered in", createdAt: "t", runId: "run-queued-live", isActive: true },
  { id: 11, parentId: 10, role: "assistant", content: "partial", createdAt: "t", runId: liveRun, isActive: true },
];
merged = mergeSnapshotMessages(steerScenario, steerSnapshot, new Set([liveRun]), new Set());
const steeredLiveAssistant = merged.find((m) => m.id === 11);
if (!steeredLiveAssistant) fail("steer: live assistant row missing after merge");
if (steeredLiveAssistant.parentId !== 12) fail(`steer: live assistant not re-parented onto the steer (parent ${steeredLiveAssistant.parentId})`);
if (steeredLiveAssistant.content !== "partial answer") fail("steer: live content lost in merge");
const steerPath = getActivePath(merged);
if (steerPath.map((m) => m.id).join(",") !== "1,10,12,11") fail(`steer: active path must pass through the steer: ${steerPath.map((m) => m.id).join(",")}`);

// Group chains: every turn's temp row parents onto the PREVIOUS turn's temp
// row. A mid-run reload (which fires on the completed turn's run.completed)
// replaces the completed temp with its persisted row - the in-flight turn's
// temp must re-point AT THAT PERSISTED ID (the swap map used to cover user
// rows only, and the live group transcript collapsed to the last turn once
// per reload). The completed temp's own drop is expected: the persisted
// row takes its place.
const groupRun = "run-179-g";
const turnA = "run-179-g-t0-0";
const turnBLive = "run-179-g-t0-1";
const groupLocal: HarnessMessage[] = [
  { id: -101, parentId: 0, role: "user", content: "discuss shipping", createdAt: "t", runId: groupRun, isActive: true },
  { id: -102, parentId: -101, role: "assistant", content: "alice turn", createdAt: "t", runId: turnA, isActive: true },
  { id: -103, parentId: -102, role: "assistant", content: "bob streaming turn", createdAt: "t", runId: turnBLive, isActive: true },
];
const groupSnapshot: HarnessMessage[] = [
  { id: 201, parentId: 0, role: "user", content: "discuss shipping", createdAt: "t", runId: groupRun, isActive: true },
  { id: 202, parentId: 201, role: "assistant", content: "alice turn", createdAt: "t", runId: turnA, isActive: true },
];
merged = mergeSnapshotMessages(groupLocal, groupSnapshot, new Set([turnBLive, groupRun]), new Set());
const bobTemp = merged.find((m) => m.runId === turnBLive);
if (!bobTemp) fail("group: in-flight turn temp must survive the reload merge");
if (bobTemp.parentId !== 202) fail(`group: live turn must re-parent onto the persisted previous turn (parent ${bobTemp.parentId})`);
if (merged.some((m) => m.id === -102)) fail("group: the replaced turn temp must not duplicate its persisted row");
const groupPath = getActivePath(merged);
if (groupPath.map((m) => m.id).join(",") !== "201,202,-103") fail(`group: active path must carry the full chain: ${groupPath.map((m) => m.id).join(",")}`);

// The failure mode the run.steered_live handler must avoid: the steered row
// carries the promoted run's id, so a STALE queue state (the ref still lists
// from_run when the reload merges) excludes the row, and the re-parented
// assistant dangles - the path collapses to the tail and the history above
// it vanishes until a full page refresh. The handler clears the ref
// synchronously so this merge never sees from_run in the excluded set.
// Mid-run snapshot: the in-flight assistant is not persisted yet (only the
// steer row was just anchored).
const steerStaleQueue = mergeSnapshotMessages(
  steerScenario,
  steerSnapshot.filter((m) => m.id !== 11),
  new Set([liveRun]),
  new Set(["run-queued-live"]),
);
const steerStalePath = getActivePath(steerStaleQueue);
if (steerStalePath.map((m) => m.id).join(",") !== "-200") {
  fail(`stale-queue steer should dangle to the assistant, got: ${steerStalePath.map((m) => m.id).join(",")}`);
}

// Queued-run messages stay excluded from the merge, as before.
const queuedMerged = mergeSnapshotMessages(
  localLive,
  [{ id: 12, parentId: 0, role: "user", content: "queued", createdAt: "t", runId: "run-queued", isActive: true }],
  new Set([liveRun]),
  new Set(["run-queued"]),
);
if (queuedMerged.some((m) => m.id === 12)) fail("queued-run message must stay excluded");

// ── chipifyMentions: @-paths in user bubbles render as file chips ──
// Windows drive path with spaces, hand-typed (the composer sends these as
// explicit mentions, but the scan must chip them identically).
if (chipifyMentions("check out @C:\\Users\\<user>\\Documents\\vault\\notes.md ok?") !== "check out `C:\\Users\\<user>\\Documents\\vault\\notes.md` ok?") {
  fail(`windows path not chipped: ${JSON.stringify(chipifyMentions("check out @C:\\Users\\<user>\\Documents\\vault\\notes.md ok?"))}`);
}
// Sentence punctuation stops the token; the colon inside C: does not.
if (chipifyMentions("see @C:/dir/subdir, then @D:/x") !== "see `C:/dir/subdir`, then `D:/x`") {
  fail(`punctuation stop set wrong: ${JSON.stringify(chipifyMentions("see @C:/dir/subdir, then @D:/x"))}`);
}
// Non-absolute @tokens (handles, mail) are left alone.
if (chipifyMentions("ping @nyaxus or @a@b") !== "ping @nyaxus or @a@b") fail("relative @token was chipped");
// No @ at all: unchanged, no allocation of a new string is required.
if (chipifyMentions("plain text") !== "plain text") fail("content without @ changed");

// ── parent swaps flow out of the merge (the retry-after-restart bug) ──
// A failed run's streamed partial is a temp (id -7). The continuation run
// parents onto that partial. When the merge swaps the partial for its
// persisted row, the continuation's parent pointer must follow the swap -
// a stale pointer dangles and the whole lineage above it vanishes from the
// active path (transcript restores only after a later full merge).
const failedRunId = "run-failed-1";
const partialTemp: HarnessMessage = { id: -7, parentId: 1, role: "assistant", content: "partial", createdAt: "t", runId: failedRunId, isActive: true };
const continuationTemp: HarnessMessage = { id: -9, parentId: -7, role: "assistant", content: "", createdAt: "t", runId: "run-continued", isActive: true };
const swaps = new Map<number, number>();
const swappedMerged = mergeSnapshotMessages(
  [oldUser, partialTemp, continuationTemp],
  [oldUser, { id: 5, parentId: 1, role: "assistant", content: "partial", createdAt: "t", runId: failedRunId, isActive: true }],
  new Set(["run-continued"]),
  new Set([]),
  swaps,
);
if (swaps.get(-7) !== 5) fail(`parent swap not recorded: ${JSON.stringify([...swaps])}`);
const swappedContinuation = swappedMerged.find((m) => m.id === -9);
if (!swappedContinuation || swappedContinuation.parentId !== 5) {
  fail(`continuation not re-pointed onto the persisted partial: ${JSON.stringify(swappedContinuation)}`);
}
if (getActivePath(swappedMerged).map((m) => m.id).join(",") !== "1,5,-9") {
  fail(`swapped path should read 1,5,-9: ${getActivePath(swappedMerged).map((m) => m.id).join(",")}`);
}

// ── the optimistic user row survives the swap-era merge ──
// The merge swaps the optimistic user row for the persisted row, then the
// optimistic-merge filter removed that persisted row "to keep the temp as
// parent" - leaving NO user row and orphaning the streamed turn (transcript
// showed only the working pill until the run completed). The filter must
// only fire when the optimistic temp actually survived the merge.
{
  const optimisticUser: HarnessMessage = { id: -4, parentId: 0, role: "user", content: "hi", createdAt: "t", runId: failedRunId, isActive: true };
  const streamedTurn: HarnessMessage = { id: -6, parentId: -4, role: "assistant", content: "streaming…", createdAt: "t", runId: failedRunId, isActive: true };
  const persistedUser = { id: 1, parentId: 0, role: "user", content: "hi", createdAt: "t", runId: failedRunId, isActive: true };
  const keptPersisted = mergeOptimisticSnapshot(
    [optimisticUser, streamedTurn],
    [persistedUser],
    new Set([failedRunId]),
    new Set([]),
    { user: optimisticUser },
  );
  if (!keptPersisted.some((m) => m.role === "user")) {
    fail(`no user row survived the optimistic merge: ${JSON.stringify(keptPersisted.map((m) => m.id))}`);
  }
  const turn = keptPersisted.find((m) => m.id === -6);
  if (!turn || turn.parentId !== 1) fail(`streamed turn's parent must follow the persisted user row: ${JSON.stringify(turn)}`);
}

// ── upsertAssistant cannot parent onto an id that does not exist ──
// The pending parent id can be stale (a temp swapped out by a merge that
// ran between the capture and the first delta). The creator must fall back
// through the temp user row and the active leaf instead of dangling.
{
  const monday: HarnessMessage = { id: 1, parentId: 0, role: "user", content: "hi", createdAt: "t", isActive: true };
  const created = upsertAssistant(
    [monday],
    { eventId: "evt-create", sessionId: "session-1", runId: "run-created", sequence: 2, type: "text.delta", emittedAt: "t", payload: { text: "a" } },
    { "run-created": -999 },
    undefined,
    "a",
  );
  const createdRow = created.find((m) => m.runId === "run-created");
  if (!createdRow || createdRow.parentId !== 1) fail(`dead pending parent should fall back to the leaf, got ${JSON.stringify(createdRow)}`);
}

console.log("edit_flow_test passed");
