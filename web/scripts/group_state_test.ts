// Regression test for the group-chat ledger projection (docs/group-chat.md):
// groupStateFromEvents replays group.created / group.started /
// group.turn.started / group.completed into the banner state (roster, live
// round, current speaker, termination); groupTranscriptItems interleaves the
// active-path messages with the recorded abstentions in ledger order (an
// abstention renders at its turn's position, a live in-flight message sorts
// last).
import { groupStateFromEvents, groupTranscriptItems, type HarnessEvent, type HarnessMessage } from "../src/harness";

function fail(message: string): never {
  throw new Error(`group_state_test failed: ${message}`);
}

let seq = 0;
function make(type: HarnessEvent["type"], payload: Record<string, unknown>, runId = "run-group"): HarnessEvent {
  seq += 1;
  return {
    eventId: `evt-${seq}`,
    sessionId: "session-g",
    runId,
    sequence: seq,
    type,
    emittedAt: `2026-10-07T00:00:${String(seq).padStart(2, "0")}.000Z`,
    payload,
  };
}

let msgId = 0;
function msg(role: HarnessMessage["role"], runId: string, speaker?: string): HarnessMessage {
  msgId += 1;
  return {
    id: msgId,
    parentId: msgId - 1,
    role,
    content: `content-${msgId}`,
    createdAt: "2026-10-07T00:00:00.000Z",
    runId,
    isActive: true,
    ...(speaker ? { speaker } : {}),
  };
}

// ── groupStateFromEvents ─────────────────────────────────────────────────

// Empty ledger -> no group.
{
  const state = groupStateFromEvents([]);
  if (state.active || state.speakers.length !== 0 || state.termination !== null) {
    fail(`empty ledger should have no group: ${JSON.stringify(state)}`);
  }
}

// group.created seeds the roster.
{
  const state = groupStateFromEvents([
    make("group.created", { speakers: ["alice", "bob"] }),
  ]);
  if (state.speakers.join(",") !== "alice,bob") fail("roster from group.created wrong");
  if (state.active) fail("group.created alone must not be active");
}

// group.started activates with the round budget.
{
  const state = groupStateFromEvents([
    make("group.started", { speakers: ["alice", "bob"], max_rounds: 3 }),
  ]);
  if (!state.active) fail("group.started must be active");
  if (state.maxRounds !== 3) fail("maxRounds must come from group.started");
  if (state.round !== 0 || state.currentSpeaker !== null) fail("fresh group has no round yet");
}

// group.turn.started tracks the round and the current speaker.
{
  const state = groupStateFromEvents([
    make("group.started", { speakers: ["alice", "bob"], max_rounds: 3 }),
    make("group.turn.started", { speaker: "alice", round: "1" }, "run-group-t0-0"),
    make("group.turn.started", { speaker: "bob", round: "1" }, "run-group-t0-1"),
    make("group.turn.started", { speaker: "alice", round: "2" }, "run-group-t1-0"),
  ]);
  if (state.round !== 2) fail(`round must be the latest turn's round, got ${state.round}`);
  if (state.currentSpeaker !== "alice") fail(`current speaker must be the latest turn's, got ${state.currentSpeaker}`);
}

// group.completed deactivates and records the termination.
{
  const state = groupStateFromEvents([
    make("group.started", { speakers: ["alice", "bob"], max_rounds: 3 }),
    make("group.turn.started", { speaker: "alice", round: "1" }, "run-group-t0-0"),
    make("group.completed", { rounds: "1", termination: "unanimous abstention" }),
  ]);
  if (state.active) fail("group.completed must deactivate");
  if (state.termination !== "unanimous abstention") fail("termination must survive");
  if (state.currentSpeaker !== null) fail("current speaker must clear on completion");
}

// A second group.started after a completion is the live group.
{
  const state = groupStateFromEvents([
    make("group.started", { speakers: ["alice", "bob"], max_rounds: 2 }),
    make("group.completed", { rounds: "1", termination: "stopped" }),
    make("group.started", { speakers: ["alice", "bob", "carol"], max_rounds: 5 }),
  ]);
  if (!state.active) fail("the second group must be active");
  if (state.maxRounds !== 5) fail("the live group's budget must win");
  if (state.speakers.length !== 3) fail("the live group's roster must win");
  if (state.prepared !== null) fail("a fresh group has nobody preparing");
}

// group.turn.prepared: the chosen-next speaker shows as preparing until
// their turn starts (then the current speaker wins the banner).
{
  const state = groupStateFromEvents([
    make("group.started", { speakers: ["alice", "bob"], max_rounds: 3 }),
    make("group.turn.started", { speaker: "alice", round: "1" }, "run-group-t0-0"),
    make("group.turn.prepared", { speaker: "bob" }, "run-group-t0-1"),
  ]);
  if (state.prepared !== "bob") fail(`prepared must survive until the turn starts: ${JSON.stringify(state)}`);
  const started = groupStateFromEvents([
    make("group.started", { speakers: ["alice", "bob"], max_rounds: 3 }),
    make("group.turn.started", { speaker: "alice", round: "1" }, "run-group-t0-0"),
    make("group.turn.prepared", { speaker: "bob" }, "run-group-t0-1"),
    make("group.turn.started", { speaker: "bob", round: "1" }, "run-group-t0-1"),
  ]);
  if (started.prepared !== null) fail("the turn's start must clear the preparing signal");
  if (started.currentSpeaker !== "bob") fail("the started turn owns the speaker state");
  const done = groupStateFromEvents([
    make("group.started", { speakers: ["alice", "bob"], max_rounds: 3 }),
    make("group.turn.started", { speaker: "alice", round: "1" }, "run-group-t0-0"),
    make("group.turn.prepared", { speaker: "bob" }, "run-group-t0-1"),
    make("group.completed", { rounds: "1", termination: "stopped" }),
  ]);
  if (done.prepared !== null || done.active) fail("completion clears the preparing signal");
}

// ── groupTranscriptItems ─────────────────────────────────────────────────

// Messages only (no group events) -> the same order, no markers.
{
  const a = msg("user", "run-group");
  const b = msg("assistant", "run-group-t0-0", "alice");
  const items = groupTranscriptItems([a, b], [
    make("run.accepted", {}, "run-group"),
    make("group.turn.started", { speaker: "alice", round: "1" }, "run-group-t0-0"),
    make("run.completed", {}, "run-group-t0-0"),
    make("run.completed", {}, "run-group"),
  ]);
  if (items.length !== 2) fail(`expected 2 items, got ${items.length}`);
  if (items[0].kind !== "message" || items[0].message.id !== a.id) fail("user message must come first");
  if (items[1].kind !== "message" || items[1].message.id !== b.id) fail("assistant message must follow");
}

// An abstention lands at its turn's position, between committed messages.
{
  const user = msg("user", "run-group");
  const alice = msg("assistant", "run-group-t0-0", "alice");
  const carol = msg("assistant", "run-group-t0-2", "carol");
  const events = [
    make("run.accepted", {}, "run-group"),
    make("group.turn.started", { speaker: "alice", round: "1" }, "run-group-t0-0"),
    make("run.completed", {}, "run-group-t0-0"),
    make("group.turn.started", { speaker: "bob", round: "1" }, "run-group-t0-1"),
    make("group.abstained", { speaker: "bob", round: "1" }, "run-group-t0-1"),
    make("group.turn.started", { speaker: "carol", round: "1" }, "run-group-t0-2"),
    make("run.completed", {}, "run-group-t0-2"),
  ];
  const items = groupTranscriptItems([user, alice, carol], events);
  if (items.length !== 4) fail(`expected 4 items, got ${items.length}`);
  const kinds = items.map((item) => item.kind).join(",");
  if (kinds !== "message,message,abstained,message") fail(`unexpected kinds: ${kinds}`);
  if (items[0].kind !== "message" || items[0].message.id !== user.id) fail("user message must come first");
  if (items[1].kind !== "message" || items[1].message.id !== alice.id) fail("alice's turn must precede the abstention");
  const abstained = items[2];
  if (abstained.kind !== "abstained") fail(`expected abstention marker at index 2: ${JSON.stringify(abstained)}`);
  if (abstained.speaker !== "bob" || abstained.round !== 1 || abstained.turnRun !== "run-group-t0-1") {
    fail(`abstention fields wrong: ${JSON.stringify(abstained)}`);
  }
  if (items[3].kind !== "message" || items[3].message.id !== carol.id) fail("carol's turn must follow the abstention");
}

// A live in-flight assistant message (no terminal event for its run) sorts
// last, after everything committed.
{
  const user = msg("user", "run-group");
  const alice = msg("assistant", "run-group-t0-0", "alice");
  const carol = msg("assistant", "run-group-t0-2", "carol");
  const aliceLive = msg("assistant", "run-group-t1-0", "alice");
  const events = [
    make("run.accepted", {}, "run-group"),
    make("group.turn.started", { speaker: "alice", round: "1" }, "run-group-t0-0"),
    make("run.completed", {}, "run-group-t0-0"),
    make("group.turn.started", { speaker: "bob", round: "1" }, "run-group-t0-1"),
    make("group.abstained", { speaker: "bob", round: "1" }, "run-group-t0-1"),
    make("group.turn.started", { speaker: "carol", round: "1" }, "run-group-t0-2"),
    make("run.completed", {}, "run-group-t0-2"),
    make("group.turn.started", { speaker: "alice", round: "2" }, "run-group-t1-0"),
  ];
  const items = groupTranscriptItems([user, alice, carol, aliceLive], events);
  if (items.length !== 5) fail(`expected 5 items, got ${items.length}`);
  const last = items[items.length - 1];
  if (last.kind !== "message" || last.message.id !== aliceLive.id) {
    fail(`live in-flight message must sort last: ${JSON.stringify(last)}`);
  }
}

console.log("group_state_test passed");
