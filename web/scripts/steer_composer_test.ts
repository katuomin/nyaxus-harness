// Regression test for the steer composer state machine:
// the composer placeholder is a pure function of the run status (it tells
// the operator what Enter does in the current state), and the steer-flash
// strip copy is decided per action.
import { composerPlaceholder, steerFlashText, type RunStatus } from "../src/harness";

function fail(message: string): never {
  throw new Error(`steer_composer_test failed: ${message}`);
}

// Idle: the default prompt (no run, terminal statuses).
{
  for (const status of [undefined, null, "completed", "cancelled", "failed"] as const) {
    const text = composerPlaceholder(status);
    if (!text.startsWith("send a task to the harness")) {
      fail(`idle placeholder wrong for ${String(status)}: ${text}`);
    }
    if (!text.includes("@ for a file")) fail("idle placeholder lost the file hint");
  }
}

// Attention states: the placeholder names what the run is waiting on, and
// Enter queues.
{
  const expectations: Array<[RunStatus, string]> = [
    ["queued", "waiting for a generation slot"],
    ["waiting_approval", "waiting for approval"],
    ["waiting_input", "waiting for your answer"],
  ];
  for (const [status, fragment] of expectations) {
    const text = composerPlaceholder(status);
    if (!text.includes(fragment)) fail(`${status} placeholder missing "${fragment}": ${text}`);
    if (!text.includes("queues your message")) {
      fail(`${status} placeholder must say Enter queues: ${text}`);
    }
  }
}

// Running: Enter queues, and the queue's steer / send-now options are named.
{
  const text = composerPlaceholder("running");
  for (const fragment of [
    "type + ⏎ to queue",
    "steer it from the queue",
    "end turn at next pause",
    "send now",
  ]) {
    if (!text.includes(fragment)) fail(`running placeholder missing "${fragment}": ${text}`);
  }

  // The steer flash strips the em dashes with the rest of the UI copy.
  const queued = composerPlaceholder("queued");
  if (!queued.startsWith("waiting for a generation slot")) fail(`queued placeholder wrong: ${queued}`);
}

// Flash copy: one line per action, each naming the phase it confirms.
{
  const steer = steerFlashText("steer");
  if (!steer.startsWith("steered")) fail(`steer flash copy wrong: ${steer}`);
  if (!steer.includes("next pause")) fail("steer flash must name the boundary: " + steer);
  const now = steerFlashText("send_now");
  if (!now.startsWith("sent now")) fail(`send-now flash copy wrong: ${now}`);
  if (!now.includes("stopped")) fail("send-now flash must name the stop: " + now);
  if (steer === now) fail("the two flash lines must be distinct");
}

console.log("steer_composer_test passed");
