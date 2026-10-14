// Regression test for the agent.spawn result parse: the inline subagent
// block renders the harvested narrative from the tool result string.
import { parseSpawnResult } from "../src/harness";

function fail(message: string): never {
  throw new Error(`spawn_test failed: ${message}`);
}

// Completed with a plain narrative.
{
  const parsed = parseSpawnResult("Worker completed (profile purri, session session-123, run run-456). Full transcript: session session-123.\n\npong");
  if (!parsed || parsed.status !== "completed" || parsed.childSessionId !== "session-123" || parsed.narrative !== "pong") {
    fail(`completed parse wrong: ${JSON.stringify(parsed)}`);
  }
}

// Failed with a byte-capped multi-line narrative (omission marker inline).
{
  const parsed = parseSpawnResult("Worker failed (profile a, session session-1, run run-2). Full transcript: session session-1.\n\nhead\n[... 5 bytes of output omitted ...]\ntail");
  if (!parsed || parsed.status !== "failed" || parsed.childSessionId !== "session-1" || !parsed.narrative.includes("omitted")) {
    fail(`failed parse wrong: ${JSON.stringify(parsed)}`);
  }
}

// Cancelled with the no-narrative placeholder.
{
  const parsed = parseSpawnResult("Worker cancelled (profile a, session session-9, run run-9). Full transcript: session session-9.\n\n(no narrative - the worker produced no final message)");
  if (!parsed || parsed.status !== "cancelled" || parsed.childSessionId !== "session-9" || !parsed.narrative.startsWith("(no narrative")) {
    fail(`cancelled parse wrong: ${JSON.stringify(parsed)}`);
  }
}

// Non-spawn results are not spawn results.
{
  if (parseSpawnResult("Task 1 marked done.") !== null) fail("plain result parsed as spawn");
  if (parseSpawnResult("") !== null) fail("empty result parsed as spawn");
  if (parseSpawnResult("Worker done (profile a, session session-1, run run-1).") !== null) fail("bad status parsed as spawn");
}

console.log("spawn_test passed");
