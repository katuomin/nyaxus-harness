// Regression test for the task-checklist ledger projection:
// tasksFromEvents replays tasks.created (replace) and tasks.updated
// (toggle by 1-based index), ignoring stale indices and malformed payloads;
// runBlocksFromEvents emits compact task markers (task_created /
// task_updated), never full-card blocks - the checklist is one pinned card.
import { runBlocksFromEvents, taskChecklistRecords, tasksFromEvents, type HarnessEvent } from "../src/harness";

function fail(message: string): never {
  throw new Error(`tasks_test failed: ${message}`);
}

let seq = 0;
function make(type: HarnessEvent["type"], payload: Record<string, unknown>, runId = "run-a"): HarnessEvent {
  seq += 1;
  return {
    eventId: `evt-${seq}`,
    sessionId: "session-a",
    runId,
    sequence: seq,
    type,
    emittedAt: `2026-09-28T00:00:${String(seq).padStart(2, "0")}.000Z`,
    payload,
  };
}

// Empty ledger -> no checklist.
{
  if (tasksFromEvents([]).length !== 0) fail("expected an empty checklist");
}

// create -> list of pending tasks.
{
  const tasks = tasksFromEvents([
    make("tasks.created", { tasks: JSON.stringify(["fix the parser", "add tests"]) }),
  ]);
  if (tasks.length !== 2 || tasks[0].text !== "fix the parser" || tasks[1].done) {
    fail(`created list wrong: ${JSON.stringify(tasks)}`);
  }
}

// update toggles by 1-based index; both directions.
{
  const tasks = tasksFromEvents([
    make("tasks.created", { tasks: JSON.stringify(["a", "b", "c"]) }),
    make("tasks.updated", { task: "1", status: "done" }),
    make("tasks.updated", { task: "3", status: "done" }),
    make("tasks.updated", { task: "1", status: "pending" }),
  ]);
  if (tasks[0].done) fail("task 1 should be pending again");
  if (tasks[1].done) fail("task 2 untouched, should stay pending");
  if (!tasks[2].done) fail("task 3 should be done");
}

// A later create replaces the whole list (fresh checklist).
{
  const tasks = tasksFromEvents([
    make("tasks.created", { tasks: JSON.stringify(["old task"]) }),
    make("tasks.updated", { task: "1", status: "done" }),
    make("tasks.created", { tasks: JSON.stringify(["new task"]) }),
  ]);
  if (tasks.length !== 1 || tasks[0].text !== "new task" || tasks[0].done) {
    fail(`replace semantics wrong: ${JSON.stringify(tasks)}`);
  }
}

// Stale indices (out of range) are ignored, not a crash.
{
  const tasks = tasksFromEvents([
    make("tasks.created", { tasks: JSON.stringify(["only one"]) }),
    make("tasks.updated", { task: "5", status: "done" }),
    make("tasks.updated", { task: "0", status: "done" }),
  ]);
  if (tasks.length !== 1 || tasks[0].done) fail("stale update should be ignored");
}

// Malformed payload degrades to an empty list.
{
  if (tasksFromEvents([make("tasks.created", { tasks: "not-json" })]).length !== 0) {
    fail("malformed create should yield an empty list");
  }
}

// Tasks events render compact transcript markers at the change points:
// task_created carries the count (0 for a discard), task_updated carries
// the 1-based index + status; no full-card blocks are emitted.
{
  const createdEvent = make("tasks.created", { tasks: JSON.stringify(["a", "b"]) });
  const updatedEvent = make("tasks.updated", { task: "1", status: "done" });
  const blocks = runBlocksFromEvents([
    make("text", { text: "hello" }),
    createdEvent,
    updatedEvent,
    make("tasks.updated", { task: "2", status: "pending" }),
  ], "run-a");
  const kinds = blocks.map((block) => block.kind).join(",");
  if (kinds !== "text,task_created,task_updated,task_updated") fail(`unexpected kinds: ${kinds}`);
  const created = blocks[1];
  if (created.kind !== "task_created" || created.count !== 2 || created.eventId !== createdEvent.eventId) fail(`created marker wrong: ${JSON.stringify(created)}`);
  const updated = blocks[2];
  if (updated.kind !== "task_updated" || updated.index !== 1 || updated.status !== "done" || updated.eventId !== updatedEvent.eventId) fail(`updated marker wrong: ${JSON.stringify(updated)}`);
}

// A discard (empty create) is a zero-count marker.
{
  const blocks = runBlocksFromEvents([make("tasks.created", { tasks: "[]" })], "run-a");
  if (blocks.length !== 1 || blocks[0].kind !== "task_created" || blocks[0].count !== 0) {
    fail(`discard marker wrong: ${JSON.stringify(blocks)}`);
  }
}

// taskChecklistRecords: a discard records the list state right before it,
// anchored at the discard event (the card lingers there, red Xs on the
// items that never ran).
{
  const createdEvent = make("tasks.created", { tasks: JSON.stringify(["a", "b"]) });
  const updatedEvent = make("tasks.updated", { task: "1", status: "done" });
  const discardEvent = make("tasks.created", { tasks: "[]" });
  const records = taskChecklistRecords([createdEvent, updatedEvent, discardEvent]);
  if (records.length !== 1 || records[0].kind !== "discarded") {
    fail(`expected one discarded record: ${JSON.stringify(records)}`);
  }
  if (records[0].anchor !== discardEvent.eventId) fail(`discard anchor wrong: ${records[0].anchor}`);
  if (records[0].tasks.length !== 2 || !records[0].tasks[0].done || records[0].tasks[1].done) {
    fail(`discard state wrong: ${JSON.stringify(records[0].tasks)}`);
  }
}

// A completed list anchors its card at the last tick (all green).
{
  const createdEvent = make("tasks.created", { tasks: JSON.stringify(["a", "b"]) });
  const u1 = make("tasks.updated", { task: "1", status: "done" });
  const u2 = make("tasks.updated", { task: "2", status: "done" });
  const records = taskChecklistRecords([createdEvent, u1, u2]);
  if (records.length !== 1 || records[0].kind !== "completed") {
    fail(`expected one completed record: ${JSON.stringify(records)}`);
  }
  if (records[0].anchor !== u2.eventId) fail(`completed anchor wrong: ${records[0].anchor}`);
  if (!records[0].tasks.every((t) => t.done)) fail(`completed state wrong: ${JSON.stringify(records[0].tasks)}`);
}

// A later create/discard never removes an earlier completed card: each closed
// checklist is an independent record.
{
  const c1 = make("tasks.created", { tasks: JSON.stringify(["1", "2", "3"]) });
  const d1 = make("tasks.updated", { task: "1", status: "done" });
  const d2 = make("tasks.updated", { task: "2", status: "done" });
  const d3 = make("tasks.updated", { task: "3", status: "done" });
  const c2 = make("tasks.created", { tasks: JSON.stringify(["x", "y"]) });
  const disc2 = make("tasks.created", { tasks: "[]" });
  const records = taskChecklistRecords([c1, d1, d2, d3, c2, disc2]);
  if (records.length !== 2) fail(`expected two records: ${JSON.stringify(records)}`);
  if (records[0].kind !== "completed" || records[0].anchor !== d3.eventId) {
    fail(`first record should be the completed list at its last tick: ${JSON.stringify(records[0])}`);
  }
  if (records[1].kind !== "discarded" || records[1].anchor !== disc2.eventId) {
    fail(`second record should be the discard: ${JSON.stringify(records[1])}`);
  }
}

// A live (incomplete) list replaced by a new create leaves no record.
{
  const first = make("tasks.created", { tasks: JSON.stringify(["a", "b"]) });
  make("tasks.updated", { task: "1", status: "done" });
  const second = make("tasks.created", { tasks: JSON.stringify(["c"]) });
  const records = taskChecklistRecords([first, make("tasks.updated", { task: "1", status: "done" }), second]);
  if (records.length !== 0) fail(`a merely-replaced live list should leave no record: ${JSON.stringify(records)}`);
}

// Discarding into an already-empty list leaves no record.
{
  const records = taskChecklistRecords([make("tasks.created", { tasks: "[]" })]);
  if (records.length !== 0) fail(`discard into an empty list should leave no record: ${JSON.stringify(records)}`);
}

// Rewind scoping: with the active path's run ids passed, a rewound run's
// task events stop replaying - the list dies with the turn instead of
// ghosting (the append-only ledger keeps its tasks.created forever).
{
  const created = make("tasks.created", { tasks: JSON.stringify(["fix the parser", "add tests"]) });
  const updated = make("tasks.updated", { task: "1", status: "done" });
  // Everything replayed, no set - unchanged legacy behavior.
  if (tasksFromEvents([created, updated]).length !== 2) fail("unscoped replay should include the run");
  // The run is still part of the active path - the list survives.
  if (tasksFromEvents([created, updated], new Set(["run-a"])).length !== 2) fail("scoped replay should keep an active run's list");
  // The run was rewound away - the list is gone.
  if (tasksFromEvents([created, updated], new Set()).length !== 0) fail("rewound run's list must not ghost");
  // A rewound first list plus a live second run: only the second list shows.
  const second = make("tasks.created", { tasks: JSON.stringify(["new plan"]) }, "run-b");
  const tasks = tasksFromEvents([created, updated, second], new Set(["run-b"]));
  if (tasks.length !== 1 || tasks[0].text !== "new plan") fail(`two-run scoping wrong: ${JSON.stringify(tasks)}`);
}

console.log("tasks_test passed");
