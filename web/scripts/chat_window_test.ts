// Bounded display history: the chat's mounted window
// math. The tail is CHAT_WINDOW_SIZE messages; scrolling up grows the
// window lazily but only while fewer than HISTORY_MAX_LINES messages are
// mounted - at the cap the top strip points at search / the session graph
// (anchored jumps bypass the cap), and the durable ledger / model context
// are never touched.
import { HISTORY_MAX_LINES, canLoadEarlier } from "../src/harness";

function fail(message: string): never {
  throw new Error(`chat_window_test failed: ${message}`);
}

// Boundary: exactly the cap mounted -> no auto-expand.
if (canLoadEarlier(100, 600)) fail(`at the cap (600-100=500 mounted) auto-expand must stop`);
// One under the cap -> auto-expand continues.
if (!canLoadEarlier(101, 600)) fail(`one under the cap (499 mounted) must still expand`);
// The window is already at the path's start -> nothing earlier exists.
if (canLoadEarlier(0, 10_000)) fail(`windowStart 0 must never expand`);
// A short chat mounted whole -> nothing earlier exists.
if (canLoadEarlier(0, 40)) fail(`a short chat must never expand`);
// A long chat with the default tail window -> expandable.
if (!canLoadEarlier(1960, 2000)) fail(`the tail window of a long chat must expand`);

// The cap constant matches the roadmap's history_max_lines=500.
if (HISTORY_MAX_LINES !== 500) fail(`HISTORY_MAX_LINES must be 500, got ${HISTORY_MAX_LINES}`);

console.log("chat_window_test passed");
