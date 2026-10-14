// Regression test for the ⌘K palette's data path: FixtureHarnessClient
// searchAll mirrors the daemon's bounded cross-session scan - case-insensitive
// match over active-path messages, newest session first, snippets centered on
// the hit, capped results.
import { FixtureHarnessClient, type GlobalSearchMatch } from "../src/harness";

function fail(message: string): never {
  throw new Error(`search_palette_test failed: ${message}`);
}

const client = new FixtureHarnessClient();

// The fixture's first session carries a user + assistant message with known
// phrases; the other two fixture sessions have no messages.

// Case-insensitive hit on the user message, with session metadata.
{
  const response = await client.searchAll("filing cabinet");
  if (response.matches.length !== 1) fail(`expected 1 match, got ${response.matches.length}`);
  const hit: GlobalSearchMatch = response.matches[0];
  if (hit.sessionId !== "session-memory") fail(`wrong session: ${hit.sessionId}`);
  if (hit.sessionTitle !== "memory routing audit") fail(`wrong title: ${hit.sessionTitle}`);
  if (hit.role !== "user") fail(`wrong role: ${hit.role}`);
  if (!hit.snippet.includes("filing cabinet")) fail(`snippet lost the match: ${hit.snippet}`);
}

// Case-insensitive hit on the assistant message.
{
  const response = await client.searchAll("STABLE IDENTITY");
  if (response.matches.length !== 1) fail(`expected 1 match, got ${response.matches.length}`);
  if (response.matches[0].role !== "assistant") fail(`wrong role: ${response.matches[0].role}`);
  if (response.matches[0].messageId !== 2) fail(`wrong message id: ${response.matches[0].messageId}`);
}

// No hit: empty matches, not an error.
{
  const response = await client.searchAll("zebra");
  if (response.matches.length !== 0) fail(`expected 0 matches, got ${response.matches.length}`);
  if (response.truncated !== false) fail("no-match result must not be truncated");
}

// Snippets are centered on the match with ellipses on both sides when the
// message is longer than the window.
{
  const response = await client.searchAll("request-scoped memory");
  if (response.matches.length !== 1) fail(`expected 1 match, got ${response.matches.length}`);
  const snippet = response.matches[0].snippet;
  if (!snippet.startsWith("\u2026")) fail(`snippet should open with an ellipsis: ${snippet}`);
  if (!snippet.endsWith("\u2026")) fail(`snippet should close with an ellipsis: ${snippet}`);
}

console.log("search_palette_test passed");
