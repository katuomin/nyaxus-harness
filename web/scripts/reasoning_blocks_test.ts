// Regression test for the reasoning-block fragment.
//
// The event window is capped to its tail. While a step is still streaming,
// its head deltas can fall out of the window, leaving the live reasoning
// block as a mid-sentence suffix of the step's full text. The full
// accumulated text is known (the message row's reasoning/content), so the
// live block must be repaired against it instead of rendering the fragment.
import { advanceEventWindow, compactEventWindow, runBlocksFromEvents, type HarnessEvent } from "../src/harness";

function fail(message: string): never {
  throw new Error(`reasoning_blocks_test failed: ${message}`);
}

const runId = "run-sim";
let seq = 0;
function make(type: HarnessEvent["type"], payload: Record<string, unknown>): HarnessEvent {
  return {
    eventId: `evt-${++seq}`,
    sessionId: "session-sim",
    runId,
    sequence: seq,
    type,
    emittedAt: "2026-09-18T00:00:00.000Z",
    payload,
  };
}
function delta(type: "reasoning.delta" | "text.delta", text: string): HarnessEvent {
  return make(type, { text });
}
function complete(type: "reasoning" | "text", text: string): HarnessEvent {
  return make(type, { text });
}
function toolStarted(): HarnessEvent {
  return make("tool.started", { tool_id: "tool-1", name: "memory.recall" });
}

const R1 = "alpha beta gamma. delta epsilon. First, let me recall the memory related to the previous session and the context. Let me call memory.recall for recent context.";
const R2 = "The user is greeting casually. According to the previous session, the last conversation was 4 days ago.";
const TEXT = "hey. nothing much on my end, mostly idle-cycle work.";

// Snapshot hydration must match live rendering's bounded event tail.
{
  const events = Array.from({ length: 700 }, (_, i) => make("text.delta", { text: `e${i}` }));
  const compacted = compactEventWindow(events);
  if (compacted.length !== 200 || compacted[0].payload.text !== "e500") {
    fail(`snapshot delta tail was not capped: ${compacted.length}`);
  }
  const structural = [...events, toolStarted()];
  const withStructure = compactEventWindow(structural);
  if (!withStructure.some((event) => event.type === "tool.started")) {
    fail("snapshot compaction dropped a structural tool event");
  }
}

// 1. Live step whose head deltas fell out of the window: the block must be
// the full accumulated step, not the surviving suffix.
{
  const tail = R1.slice(40);
  const window = [delta("reasoning.delta", tail)];
  const blocks = runBlocksFromEvents(window, runId, R1, "");
  if (blocks.length !== 1 || blocks[0].kind !== "reasoning" || blocks[0].text !== R1) {
    fail(`head-truncated live step rendered ${JSON.stringify(blocks)}, expected full step`);
  }
  // The un-repaired fragment would have been the tail alone.
  if (blocks[0].text === tail) fail("live step rendered the fragment, not the repaired full step");
}

// 2. Multi-step: the live second step must not be inflated to include the
// first step, and the consolidated first step must stay intact.
{
  const r2Live = R2.slice(0, 20);
  const window = [
    complete("reasoning", R1),
    toolStarted(),
    delta("reasoning.delta", r2Live),
  ];
  const blocks = runBlocksFromEvents(window, runId, R1 + r2Live, "");
  const reasoning = blocks.filter((b) => b.kind === "reasoning").map((b) => b.text);
  if (reasoning.length !== 2 || reasoning[0] !== R1 || reasoning[1] !== r2Live) {
    fail(`multi-step live render ${JSON.stringify(reasoning)}, expected [R1, R2-so-far]`);
  }
}

// 3. Multi-step with the second step's head also truncated: the second block
// repairs to the full second step only (fallback minus the pushed first step).
{
  const r2Tail = R2.slice(20);
  const window = [
    complete("reasoning", R1),
    toolStarted(),
    delta("reasoning.delta", r2Tail),
  ];
  const blocks = runBlocksFromEvents(window, runId, R1 + R2, "");
  const reasoning = blocks.filter((b) => b.kind === "reasoning").map((b) => b.text);
  if (reasoning.length !== 2 || reasoning[1] !== R2) {
    fail(`truncated second step rendered ${JSON.stringify(reasoning[1])}, expected full R2`);
  }
}

// 4. Consolidated steps render in order with their complete texts.
{
  const window = [
    complete("reasoning", R1),
    toolStarted(),
    complete("reasoning", R2),
    complete("text", TEXT),
  ];
  const blocks = runBlocksFromEvents(window, runId, R1 + R2, TEXT);
  const kinds = blocks.map((b) => b.kind).join(",");
  if (kinds !== "reasoning,tools,reasoning,text") fail(`consolidated kinds were ${kinds}`);
  const textBlock = blocks.find((b) => b.kind === "text");
  if (!textBlock || textBlock.text !== TEXT) fail("consolidated text block mismatch");
}

// 5. A fully in-window live step is untouched by the repair.
{
  const window = [delta("reasoning.delta", R1)];
  const blocks = runBlocksFromEvents(window, runId, R1, "");
  if (blocks.length !== 1 || blocks[0].text !== R1) fail("in-window live step changed");
}

// 6. Live text with a truncated head repairs to the full response.
{
  const tail = TEXT.slice(20);
  const window = [delta("text.delta", tail)];
  const blocks = runBlocksFromEvents(window, runId, "", TEXT);
  if (blocks.length !== 1 || blocks[0].kind !== "text" || blocks[0].text !== TEXT) {
    fail(`head-truncated live text rendered ${JSON.stringify(blocks)}, expected full text`);
  }
}

// 7. No fallback (e.g. fixture runs): behavior is unchanged.
{
  const window = [delta("reasoning.delta", R1.slice(40))];
  const blocks = runBlocksFromEvents(window, runId);
  if (blocks.length !== 1 || blocks[0].text !== R1.slice(40)) fail("no-fallback path changed");
}

// 8. Chatty multi-step run: one step's delta flood must not evict the earlier
// steps' consolidated events from the window and merge the run into one live
// block (the 06:32 regression: reasoning 1 -> message 1 -> appended to
// reasoning 1, i.e. a later step's reasoning rendered before the prior text).
{
  const steps = [
    { r: 150, t: 40 },
    { r: 150, t: 0 },
    { r: 150, t: 40 },
    { r: 150, t: 40 },
    { r: 150, t: 0 },
    { r: 900, t: 40 },
  ];
  // Words are tagged s<step>w<word> so a block can be checked for step purity:
  // any block carrying two different step indices mixed two steps' text.
  const word = (s: number, w: number) => `s${s}w${w}`;
  const full: { r: string; t: string }[] = steps.map((s, i) => ({
    r: Array.from({ length: s.r }, (_, w) => word(i, w)).join(" "),
    t: Array.from({ length: s.t }, (_, w) => word(i, w)).join(" "),
  }));

  let window: HarnessEvent[] = [];
  let fallbackReasoning = "";
  let fallbackContent = "";
  const stream = (event: HarnessEvent) => {
    window = advanceEventWindow(window, event);
    if (event.type === "reasoning.delta") fallbackReasoning += event.payload.text;
    if (event.type === "text.delta") fallbackContent += event.payload.text;
  };
  const assertBlocks = (label: string) => {
    const blocks = runBlocksFromEvents(window, runId, fallbackReasoning, fallbackContent);
    for (const block of blocks) {
      if (block.kind === "tools") continue;
      const seenSteps = new Set<number>();
      for (const match of block.text.matchAll(/s(\d+)w\d+/g)) seenSteps.add(Number(match[1]));
      if (seenSteps.size > 1) {
        fail(`${label}: a block mixes steps ${[...seenSteps].join("/")}: ${JSON.stringify(block.text.slice(0, 60))}`);
      }
    }
    return blocks;
  };

  for (let s = 0; s < steps.length; s++) {
    // Mid-flood checkpoint in the chatty final step: the earlier steps'
    // consolidated blocks must survive the delta flood.
    const midFloodAt = s === steps.length - 1 ? 450 : -1;
    for (let w = 0; w < steps[s].r; w++) {
      stream(delta("reasoning.delta", `${word(s, w)} `));
      if (w + 1 === midFloodAt) {
        const floodBlocks = assertBlocks(`step ${s} mid-flood`);
        const floodReasoning = floodBlocks.filter((b) => b.kind === "reasoning");
        // Steps 0..s-1 consolidated plus the live step: the flood must not
        // have evicted the earlier steps from the window.
        if (floodReasoning.length < s + 1) {
          fail(`mid-flood: only ${floodReasoning.length} reasoning blocks survived, expected ${s + 1}`);
        }
      }
    }
    stream(complete("reasoning", full[s].r));
    assertBlocks(`step ${s} consolidated`);
    if (steps[s].t > 0) {
      for (let w = 0; w < steps[s].t; w++) stream(delta("text.delta", `${word(s, w)} `));
      stream(complete("text", full[s].t));
      assertBlocks(`step ${s} text`);
    }
    stream(toolStarted());
    stream(make("tool.completed", { tool_id: "tool-1", name: "memory.recall", result: `step ${s} done` }));
  }
  const blocks = assertBlocks("final");
  const reasoning = blocks.filter((b) => b.kind === "reasoning").map((b) => b.text);
  if (reasoning.length !== 6) fail(`expected 6 reasoning blocks, got ${reasoning.length}`);
  if (reasoning[0] !== full[0].r) fail("first step's reasoning block was clobbered by the final flood");
  const text = blocks.filter((b) => b.kind === "text").map((b) => b.text);
  if (text.length !== 4) fail(`expected 4 text blocks, got ${text.length}`);
  if (text[0] !== full[0].t) fail("first step's text block was clobbered by the final flood");
}

// 9. Long text stream: the reasoning deltas rotated out of the capped window
// entirely and the step's sealed `reasoning` event does not exist yet (it
// lands at stream end). The reasoning block must not vanish mid-stream; the
// message row's accumulated reasoning covers it.
{
  const tail = TEXT.slice(20);
  const window = [delta("text.delta", tail)];
  const blocks = runBlocksFromEvents(window, runId, R1, TEXT);
  if (blocks.length !== 2 || blocks[0].kind !== "reasoning" || blocks[0].text !== R1
    || blocks[1].kind !== "text" || blocks[1].text !== TEXT) {
    fail(`rotated-out reasoning rendered ${JSON.stringify(blocks)}, expected [reasoning R1, text TEXT]`);
  }
}

// 10. Multi-step variant: step 1 sealed normally; step 2's reasoning deltas
// rotated out during its long text stream. Sealed events are pinned by the
// window, so R1 stays and R2 is recovered from the fallback in order
// (before the live text) - no mid-stream vanish/reappear.
{
  const tail = "continued answer tail";
  const window = [
    complete("reasoning", R1),
    toolStarted(),
    delta("text.delta", tail),
  ];
  const blocks = runBlocksFromEvents(window, runId, R1 + R2, TEXT + tail);
  const kinds = blocks.map((b) => b.kind).join(",");
  if (kinds !== "reasoning,tools,reasoning,text") fail(`sealed-past kinds were ${kinds}`);
  const reasoning = blocks.filter((b) => b.kind === "reasoning").map((b) => b.text);
  if (reasoning.length !== 2 || reasoning[0] !== R1 || reasoning[1] !== R2) {
    fail(`sealed-past reasoning rendered ${JSON.stringify(reasoning)}, expected [R1, R2]`);
  }
}

// 12. Window rotation across the cap: a reasoning-heavy step followed by a
// text-heavy one must keep every sealed step event in the window (advance-
// EventWindow pins structural events) and the final block list must carry
// both steps' reasoning plus the text, even though the raw deltas long
// exceeded EVENT_WINDOW.
{
  let window: HarnessEvent[] = [];
  let fallbackReasoning = "";
  let fallbackContent = "";
  const stream = (event: HarnessEvent) => {
    window = advanceEventWindow(window, event);
    if (event.type === "reasoning.delta") fallbackReasoning += event.payload.text;
    if (event.type === "text.delta") fallbackContent += event.payload.text;
  };
  stream(delta("reasoning.delta", R1));
  stream(complete("reasoning", R1));
  for (let i = 0; i < 600; i++) stream(delta("text.delta", `${i} `));
  // Mid-text-flood: R1's sealed event must still be in the window, and the
  // fallback recovery must not duplicate it.
  const midBlocks = runBlocksFromEvents(window, runId, R1, fallbackContent);
  const midReasoning = midBlocks.filter((b) => b.kind === "reasoning").map((b) => b.text);
  if (midReasoning.length !== 1 || midReasoning[0] !== R1) {
    fail(`rotation mid-flood reasoning ${JSON.stringify(midReasoning)}`);
  }
  stream(complete("text", fallbackContent));
  const blocks = runBlocksFromEvents(window, runId, R1, fallbackContent);
  const kinds = blocks.map((b) => b.kind).join(",");
  if (kinds !== "reasoning,text") fail(`rotation final kinds were ${kinds}`);
  if (window.some((event) => event.type === "reasoning" && event.payload.text === R1) === false) {
    fail("rotation: the sealed reasoning event did not survive the window cap");
  }
}

// 11. No half-doubles: a run whose reasoning is fully sealed must not gain a
// fallback duplicate block, and a run with no fallback sees no change.
{
  const window = [
    complete("reasoning", R1),
    complete("text", TEXT),
  ];
  const blocksSealed = runBlocksFromEvents(window, runId, R1, TEXT);
  const kinds = blocksSealed.map((b) => b.kind).join(",");
  if (kinds !== "reasoning,text") fail(`sealed-only kinds were ${kinds}`);
  // A live reasoning step with no text yet gains no duplicate block.
  const blocksLive = runBlocksFromEvents([delta("reasoning.delta", R1.slice(40))], runId, R1, "");
  if (blocksLive.length !== 1 || blocksLive[0].kind !== "reasoning" || blocksLive[0].text !== R1) {
    fail(`live-reasoning step changed: ${JSON.stringify(blocksLive)}`);
  }
}

console.log("reasoning_blocks_test passed");
