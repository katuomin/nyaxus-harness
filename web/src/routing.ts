export type View =
  | "home"
  | "chat"
  | "runs"
  | "graph"
  | "memory"
  | "workspace"
  | "observatory"
  | "providers"
  | "profiles"
  | "knowledge"
  | "prompts"
  | "skills"
  | "tools"
  | "settings";

// Hash-based routing: the app ships as static assets (Vite, base "./") with no
// server-side SPA fallback, so hash urls work from any path prefix and survive
// a refresh without a 404. The hash encodes the active view and, for chat, the
// current session, so reloading restores both. The base route (#/) is the home
// overview, not a fresh conversation.
export const ROUTE_VIEWS: readonly string[] = [
  "home",
  "chat",
  "runs",
  "graph",
  "memory",
  "workspace",
  "observatory",
  "providers",
  "profiles",
  "knowledge",
  "prompts",
  "skills",
  "tools",
  "settings",
];

export function parseHash(hash: string): { view: View; sessionId?: string } | null {
  // The leading "#" and an optional "/" are the route prefix; a bare "#" is the
  // empty route. Trailing slashes are trimmed.
  const path = hash.replace(/^#\/?/, "").replace(/\/+$/, "");
  // The empty route and a bare "session" (no id) are the home overview.
  if (path === "" || path === "session") return { view: "home" };
  if (path.startsWith("session/")) {
    const encoded = path.slice("session/".length);
    try {
      return { view: "chat", sessionId: decodeURIComponent(encoded) };
    } catch {
      return { view: "chat", sessionId: encoded };
    }
  }
  if (ROUTE_VIEWS.includes(path)) return { view: path as View };
  return null;
}

export function hashFor(view: View, sessionId: string): string {
  if (view === "home") return "#/";
  if (view === "chat") {
    return sessionId ? `#/session/${encodeURIComponent(sessionId)}` : "#/chat";
  }
  return `#/${view}`;
}
