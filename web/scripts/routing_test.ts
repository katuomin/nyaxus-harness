import { hashFor, parseHash, ROUTE_VIEWS } from "../src/routing";

function assert(condition: boolean, message: string): void {
  if (!condition) throw new Error(`routing_test failed: ${message}`);
}

// parseHash: the base route (and a bare session) is the home overview.
assert(parseHash("") !== null && parseHash("")!.view === "home", "empty hash -> home");
assert(parseHash("#") !== null && parseHash("#")!.view === "home", "# -> home");
assert(parseHash("#/") !== null && parseHash("#/")!.view === "home", "#/ -> home");
assert(parseHash("#/home") !== null && parseHash("#/home")!.view === "home", "#/home -> home");
const bareSession = parseHash("#/session");
assert(bareSession !== null && bareSession.view === "home" && !bareSession.sessionId, "bare session -> home, no session");
const emptySession = parseHash("#/session/");
assert(emptySession !== null && emptySession.view === "home" && !emptySession.sessionId, "empty session id -> home, no session");

// parseHash: every named route view resolves to itself.
for (const view of ROUTE_VIEWS) {
  const parsed = parseHash(`#/${view}`);
  assert(parsed !== null && parsed.view === view, `#${view} -> ${view}`);
}

// parseHash: session routes carry the id.
const session = parseHash("#/session/abc123");
assert(session !== null && session.view === "chat" && session.sessionId === "abc123", "session hash carries id");
const encoded = parseHash("#/session/sess%2F42");
assert(encoded !== null && encoded.sessionId === "sess/42", "encoded session id is decoded");

// parseHash: unknown routes are rejected.
assert(parseHash("#/unknown") === null, "unknown hash -> null");

// hashFor: home is the base route; chat encodes the session (or #/chat when empty).
assert(hashFor("home", "") === "#/", "home -> #/");
assert(hashFor("chat", "") === "#/chat", "chat, no session -> #/chat");
assert(hashFor("chat", "abc123") === "#/session/abc123", "chat, with session");
assert(hashFor("settings", "abc123") === "#/settings", "settings ignores session");
assert(hashFor("profiles", "") === "#/profiles", "profiles");

// Round-trip: hashFor then parseHash returns the same view and session.
for (const view of ROUTE_VIEWS) {
  const parsed = parseHash(hashFor(view, ""));
  assert(parsed !== null && parsed.view === view, `round-trip view ${view}`);
}
const roundTrip = parseHash(hashFor("chat", "sess/42"));
assert(roundTrip !== null && roundTrip.view === "chat" && roundTrip.sessionId === "sess/42", "round-trip session with slash");

console.log("routing_test passed");
