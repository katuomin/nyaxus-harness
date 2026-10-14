// Regression test for the pending-actions center helpers:
// - pendingActionsFromEvents: dedup by id, clearing on resolved/terminal
//   events, provenance collection (approval.auto / loop.guard) newest-first.
// - runsFromEvents TTFT: first text event minus run start.
import { pendingActionsFromEvents, runsFromEvents, type HarnessEvent } from "../src/harness";

function fail(message: string): never {
  throw new Error(`actions_center_test failed: ${message}`);
}

let seq = 0;
function make(type: HarnessEvent["type"], payload: Record<string, unknown>, overrides: Partial<HarnessEvent> = {}): HarnessEvent {
  seq += 1;
  return {
    eventId: `evt-${seq}`,
    sessionId: "session-a",
    runId: "run-a",
    sequence: seq,
    type,
    emittedAt: `2026-09-18T00:00:${String(seq).padStart(2, "0")}.000Z`,
    payload,
    ...overrides,
  };
}

// ── pending approvals ──────────────────────────────────────────────────────
{
  const events = [
    make("approval.requested", { approval_id: "ap-1", name: "run", arguments: "rm x.txt" }),
    make("approval.requested", { approval_id: "ap-2", name: "fs.write", arguments: "a" }),
    make("approval.resolved", { approval_id: "ap-1", decision: "allow_once" }),
  ];
  const { pending } = pendingActionsFromEvents(events);
  if (pending.length !== 1 || pending[0].id !== "ap-2") fail(`expected only ap-2 pending, got ${JSON.stringify(pending)}`);
  if (pending[0].runId !== "run-a" || pending[0].kind !== "approval") fail("pending metadata wrong");
}

// Duplicate requests (ledger replay) must not double-list.
{
  const request = make("approval.requested", { approval_id: "ap-9", name: "run", arguments: "ls" });
  const { pending } = pendingActionsFromEvents([request, request]);
  if (pending.length !== 1) fail(`expected dedup to 1, got ${pending.length}`);
}

// A terminal run event clears anything it parked.
{
  const events = [
    make("approval.requested", { approval_id: "ap-3", name: "run", arguments: "rm y" }),
    make("run.cancelled", {}, { runId: "run-a" }),
  ];
  const { pending } = pendingActionsFromEvents(events);
  if (pending.length !== 0) fail(`expected no pending after cancel, got ${pending.length}`);
}

// ── pending inputs ─────────────────────────────────────────────────────────
{
  const events = [
    make("input.requested", { input_id: "in-1", kind: "quiz", question: "pick", options: JSON.stringify(["a", "b"]), has_dont_know: "true" }),
    make("input.resolved", { input_id: "in-1", choice: "0" }),
  ];
  const { pending } = pendingActionsFromEvents(events);
  if (pending.length !== 0) fail(`expected no pending after input resolved, got ${pending.length}`);
}
{
  const { pending } = pendingActionsFromEvents([
    make("input.requested", { input_id: "in-2", kind: "question", question: "q?", options: JSON.stringify(["yes", "no"]) }),
  ]);
  if (pending.length !== 1 || pending[0].kind !== "input") fail("expected one pending input");
  if (JSON.stringify(pending[0].options) !== '["yes","no"]') fail(`options parsed wrong: ${JSON.stringify(pending[0].options)}`);
}

// ── provenance ───────────────────────────────────────────────────────────
{
  const events = [
    make("approval.auto", { name: "run", arguments: "git status", rule_id: "readonly-argv" }),
    make("loop.guard", { stage: "suppress", tool: "run", args: "echo hi", inert: "true", text: "note" }),
    make("approval.auto", { name: "run", arguments: "ls", rule_id: "always-allow:git commit" }),
  ];
  const { provenance } = pendingActionsFromEvents(events);
  if (provenance.length !== 3) fail(`expected 3 provenance entries, got ${provenance.length}`);
  if (provenance[0].kind !== "approval.auto") fail("provenance must be newest-first");
  if (!provenance[0].label.includes("always-allow:git commit")) fail(`fingerprint rule missing from newest label: ${provenance[0].label}`);
  if (!provenance[2].label.includes("readonly-argv")) fail(`auto label must carry the rule id: ${provenance[2].label}`);
  if (!provenance[1].label.includes("suppress")) fail(`guard label wrong: ${provenance[1].label}`);
  if (!provenance[1].label.includes("inert")) fail(`guard inert flag missing: ${provenance[1].label}`);
}

// Legacy daemon (no rule_id): the label degrades to the plain form.
{
  const { provenance } = pendingActionsFromEvents([
    make("approval.auto", { name: "run", arguments: "ls" }),
  ]);
  if (provenance[0].label !== "auto-approved run") fail(`legacy label wrong: ${provenance[0].label}`);
}

// ── TTFT ───────────────────────────────────────────────────────────────────
{
  const events = [
    make("run.started", { model: "m", profile: "p" }, { runId: "run-ttft", emittedAt: "2026-09-18T00:00:00.000Z" }),
    make("reasoning.delta", { text: "hmm" }, { runId: "run-ttft", emittedAt: "2026-09-18T00:00:01.000Z" }),
    make("text.delta", { text: "hello" }, { runId: "run-ttft", emittedAt: "2026-09-18T00:00:02.500Z" }),
    make("text.delta", { text: " world" }, { runId: "run-ttft", emittedAt: "2026-09-18T00:00:03.000Z" }),
    make("run.completed", {}, { runId: "run-ttft", emittedAt: "2026-09-18T00:00:04.000Z" }),
  ];
  const runs = runsFromEvents(events, "session-a");
  const run = runs.get("run-ttft");
  if (!run) fail("run not rebuilt");
  if (run.metrics.ttftMs !== 2500) fail(`expected ttft 2500ms, got ${run.metrics.ttftMs}`);
}

// No text event (pure tool run) -> no ttft.
{
  const events = [
    make("run.started", { model: "m" }, { runId: "run-notext", emittedAt: "2026-09-18T00:00:00.000Z" }),
    make("tool.started", { tool_id: "t1", name: "memory.recall" }, { runId: "run-notext" }),
    make("tool.completed", { tool_id: "t1", name: "memory.recall", result: "ok" }, { runId: "run-notext" }),
    make("run.completed", {}, { runId: "run-notext", emittedAt: "2026-09-18T00:00:01.000Z" }),
  ];
  const runs = runsFromEvents(events, "session-a");
  if (runs.get("run-notext")?.metrics.ttftMs !== undefined) fail("ttft should be undefined without text");
}

console.log("actions_center_test passed");
