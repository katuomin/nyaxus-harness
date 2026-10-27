export type HarnessMode = "fixture" | "http";
export type ForkMode = "selected" | "summary";

export type RunStatus =
  | "queued"
  | "running"
  | "waiting_approval"
  | "waiting_input"
  | "completed"
  | "cancelled"
  | "failed";

export type HarnessEventType =
  | "run.accepted"
  | "run.started"
  | "run.retrying"
  // Stall watchdog fired: the provider went quiet for the configured silence
  // cap (run.stall_timeout_s). The daemon retries once when nothing was
  // delivered and fails the run (partial text persisted) when content already
  // streamed - run.failed follows in that case.
  | "run.stalled"
  | "reasoning.delta"
  | "tool.started"
  | "tool.progress"
  | "tool.completed"
  | "text.delta"
  // Per-step consolidated events: emitted when a step's streaming deltas are
  // collapsed (the deltas are dropped from the ledger), so the ledger carries
  // the per-step sequence - each provider call owns its own reasoning/text
  // block. Payload `text` is that step's full reasoning / response.
  | "reasoning"
  | "text"
  | "approval.requested"
  | "approval.resolved"
  // The daemon skipped the approval gate (read-only argv proven the call
  // changes no state): ledgered for operator provenance, model-invisible.
  | "approval.auto"
  // Deny-only monotonic guard fired in front of the approval waterfall
  // (obviously-destructive argv / credential-shaped arguments): the call is
  // denied without ever opening a gate; ledgered for operator provenance.
  | "approval.guard"
  // Loop guard fired (reminder / suppress / give_up): the deterministic
  // non-progress breaker's ledgered intervention.
  | "loop.guard"
  // Stream rule fired: a dormant regex over the streamed assistant text
  // matched and injected one volatile reminder into the prompt tail. One
  // per rule per run.
  | "stream.rule"
  // A tool result's dropped middle was persisted to the session's spill
  // store; the result carries the opaque locator to read it back in-band.
  | "tool.spilled"
  // A queued follow-up was promoted into a live mid-turn steer: the active
  // run continues with the queued text in context.
  | "run.steered_live"
  // A prompt-edit was approved and applied (pack revision bumped). The
  // session stays pinned to its snapshot; the edit lands in new sessions.
  | "prompt.pack_edited"
  | "input.requested"
  | "input.resolved"
  | "metrics.snapshot"
  // Prompt-cache tripwire fired: a long prompt was served with zero cached
  // tokens (full re-prefill). `kind` is "cold" (no cache hit earlier in the
  // run) or "regression" (the previous provider call was cached). Emitted
  // right after the metrics.snapshot that reports the miss.
  | "kv_cache_miss"
  | "run.completed"
  | "run.cancelled"
  | "run.failed"
  | "session.rewound"
  | "context.preparing"
  | "context.metrics"
  | "context.completed"
  | "context.failed"
  | "context.fallback"
  | "context.reused"
  | "context.recompressing"
  | "context.recompressed"
  | "context.recompress_failed"
  // Manual/budget compaction lifecycle: the head is being summarized into
  // the checkpoint, and the compacted boundary has committed.
  | "context.compaction_started"
  | "context.compacted"
  // Tier-0 microcompact fired: old in-run tool-result bodies were cleared
  // (LLM-free) before spending the auxiliary model on a full compaction.
  | "context.microcompacted"
  // The auxiliary summary failed; the operator is asked to choose (primary /
  // deterministic / retry). `context.summary_resolved` carries the decision.
  | "context.summary_prompt"
  | "context.summary_resolved"
  // Visible task checklist: tasks.created replaces the list, tasks.updated
  // toggles the indexed item (1-based). The web replays the ledger for the
  // live card, so the checklist survives reloads.
  | "tasks.created"
  | "tasks.updated"
  // Subagents: the parent's ledger marks the worker lifecycle. worker.started
  // carries child_session_id (the inline block streams from it); the
  // terminal events carry the status + narrative byte count.
  | "worker.started"
  | "worker.completed"
  | "worker.failed"
  | "worker.cancelled"
  // Group chat (docs/group-chat.md): the roundtable lifecycle. group.created
  // fires on session creation (the speaker roster); group.started/completed
  // bracket the group run; group.turn.prepared names the next speaker before
  // their turn opens (the banner's "preparing" signal); group.turn.started
  // marks each speaker turn; and group.abstained records a speaker's
  // nothing-to-add (a tool call, not prose - the ledgered closure signal).
  | "group.created"
  // Invite (docs/group-chat.md): a profile added to an existing conversation
  // (a 1:1 becomes a group; a group gains a speaker). Lands immediately on
  // the roster change - it carries the post-join roster and the joined
  // speaker, so it precedes any group.started.
  | "group.joined"
  | "group.started"
  | "group.turn.prepared"
  // Continue-after-error pairing: run.continued names the FAILED run a new
  // run resumes (the web renders the resumed turn without a repeated
  // profile header, inside the interrupted turn's visual block).
  | "run.continued"
  | "group.turn.started"
  | "group.abstained"
  | "group.completed"
  | "heartbeat";

export interface HarnessSession {
  id: string;
  title: string;
  profile: string;
  model: string;
  status: "idle" | "running" | "waiting";
  updatedAt: string;
  messageCount: number;
  unread?: boolean;
  contextMode?: ForkMode;
  contextStatus?: "ready" | "preparing" | "failed";
  contextError?: string;
  contextSummary?: string;
  // Pending compaction fallback prompt (auxiliary summary failed): empty =
  // no prompt owed. Resolved via respondContextDecision.
  contextPrompt?: string;
  sourceSessionId?: string;
  contextWatermark?: number;
  // Pack pinning (drift guard): the snapshot this session is frozen to
  // (0/undefined = never pinned) and the pack revision that snapshot carries.
  // The session keeps this document for its whole life; pack edits apply to
  // new sessions only.
  packSnapshotId?: number;
  packRevision?: number;
  // md-log: the vault note (absolute path) this session is mirrored to.
  // Empty = not linked.
  mdLogPath?: string;
  // Operator labels (max 12, 32 chars each). Empty/undefined = untagged.
  tags?: string[];
  // Subagents: the spawner session's id when this session is a worker
  // (agent.spawn); empty = a normal session.
  parentSessionId?: string;
  // Group chat: the ordered speaker profile ids for a group session; empty/
  // undefined = an ordinary single-profile session.
  speakers?: string[];
  // Per-session round-budget override (the banner's rounds control; absent
  // = the global group.max_rounds setting).
  groupRoundsOverride?: number;
}

// Operator-triggered compaction ("compress now") result: selected mode
// folds the oldest turns into a new checkpoint ("compacted"), summary mode
// re-compresses the active tail ("recompressing") or reuses the checkpoint
// when the delta is below the threshold ("reused").
export interface CompactContextResult {
  ok: boolean;
  status: "compacted" | "recompressing" | "reused" | "skipped";
  detail?: string;
  context_mode?: string;
  context_status?: string;
  watermark?: number;
  tail_messages?: number;
  tail_bytes?: number;
}

// One child of a directory from the fs listing route (path picker).
export interface FsEntry {
  name: string;
  dir: boolean;
}

export interface FsListing {
  entries: FsEntry[];
  truncated: boolean;
}

// One verbatim match from the original conversation (the source session for a
// compact fork), for recovering turns the summary no longer carries.
export interface SearchMatch {
  messageId: number;
  sessionId: string;
  role: string;
  createdAt: string;
  snippet: string;
}

export interface SearchResponse {
  scope: "source" | "self";
  sessionId: string;
  query: string;
  matches: SearchMatch[];
}

// One cross-session hit from the daemon's bounded full-text scan (the ⌘K
// palette). The snippet is centered on the match by the daemon.
export interface GlobalSearchMatch {
  messageId: number;
  sessionId: string;
  sessionTitle: string;
  role: string;
  createdAt: string;
  snippet: string;
}

export interface GlobalSearchResponse {
  query: string;
  matches: GlobalSearchMatch[];
  truncated: boolean;
}

// One recorded character state (from the status.set tool). The daemon stores
// the label verbatim; the prompt constrains the vocabulary per character.
export interface HarnessStatusEvent {
  id: number;
  sessionId: string;
  profile: string;
  state: string;
  note: string;
  createdAt: string;
}

// A session's state log: the latest label ("" when none recorded) plus the
// newest-first history, capped by the daemon.
export interface HarnessSessionStatus {
  sessionId: string;
  latest: string;
  events: HarnessStatusEvent[];
}

export interface SourceInfo {
  id: number;
  url: string;
  title: string;
  status: string;
  used: boolean;
  runId: string;
  fetchedAt: string;
}

export interface HarnessAttachment {
  id?: string;
  name: string;
  // "document" = a binary file (pdf/docx/...) attached as a base64 data
  // URL: it is stored + serialized into the workspace for terminal use,
  // never read into the prompt context.
  kind: "text" | "image" | "document";
  mimeType?: string;
  size: number;
  width?: number;
  height?: number;
  content: string;
  contentUrl?: string;
}

export interface HarnessMessage {
  id: number;
  parentId: number;
  role: "user" | "assistant" | "system";
  content: string;
  createdAt: string;
  reasoning?: string;
  runId?: string;
  isActive?: boolean;
  attachments?: HarnessAttachment[];
  // Group chat: the speaker profile id for an assistant message committed by
  // a group turn; absent for user messages and ordinary sessions.
  speaker?: string;
  // A-7: the client's optimistic row id (web-generated) for a user message;
  // the daemon stores it on the row so snapshot/SSE replay dedupes by it.
  // Temp rows carry it too (id === clientId while the send is in flight).
  clientId?: number;
}

export interface HarnessTool {
  id: string;
  name: string;
  target: string;
  status: "running" | "waiting_approval" | "completed" | "failed";
  durationMs?: number;
  // Live rows tick from tool.started; the payload's duration_ms freezes it.
  startedAt?: string;
  summary?: string;
  arguments?: string;
  result?: string;
  // Ephemeral mid-flight progress (tool.progress): the daemon announces a
  // long-running tool's live state. status "browsing" carries the page
  // title/URL the web.fetch camofox tier opened plus live page frames
  // (frame is a data URL; intermediate frames are SSE-only, the terminal
  // frame is ledgered so the finished row keeps the final page).
  progress?: { status: string; title?: string; url?: string; frame?: string };
}

export interface HarnessMetrics {
  inputTokens: number;
  outputTokens: number;
  reasoningTokens: number;
  contextUsed: number;
  contextWindow: number;
  tokensPerSecond: number;
  /** Time to first output token (run start -> first text event), if known. */
  ttftMs?: number;
  // Accumulated across the run's provider calls (tool iterations). Present
  // once the run's terminal event carries the daemon's usage totals.
  providerCalls?: number;
  totalInputTokens?: number;
  totalOutputTokens?: number;
}

// The run-commands ack (A-7, confirmed delivery ids): the daemon echoes
// the resolved slot-holder so queue actions confirm against the run that
// actually received the action.
export interface CommandAck {
  ok?: boolean;
  runId?: string;
  queuedRunId?: string;
  messageId?: number | string;
}

export interface HarnessRun {
  id: string;
  sessionId: string;
  profile: string;
  /** Profile revision the run started with (drift-guard pinning). */
  profileRevision?: number;
  profileFingerprint?: string;
  /** True when this run adopted a profile revision different from the session's pin. */
  profileAdopted?: boolean;
  /** Fields that drifted when the new revision was adopted. */
  profileDrift?: string[];
  model: string;
  provider: string;
  status: RunStatus;
  startedAt?: string;
  finishedAt?: string;
  error?: string;
  /** Daemon error taxonomy (polishing-pass A-4): authoritative failure kind. */
  errorKind?: string;
  /** Daemon one-line next-step hint for the failure. */
  errorHint?: string;
  /** Set while the daemon is between a failed provider attempt and its retry. */
  retry?: {
    attempt: number;
    maxAttempts: number;
    reason: string;
  };
  /** Prompt-cache tripwire fired for the latest provider call. Cleared by a
   *  later metrics.snapshot that reports a cache hit, or at run end. */
  kvCacheMiss?: {
    promptTokens: number;
    kind: string;
  };
  reasoning: string;
  /** Accumulated response text (text.delta events); the message row itself is persisted only at run end. */
  content?: string;
  tools: HarnessTool[];
  metrics: HarnessMetrics;
  approval?: {
    id: string;
    name: string;
    arguments: string;
    /** prompt.edit only: the module's current text, for the diff view. */
    oldContent?: string;
    scope: string;
    /** Fingerprint rulebook: the derived "always" pattern label. */
    fingerprint?: string;
    /** True when the daemon derived a pattern (false = whole tool). */
    patternAllowable?: boolean;
    /** Structured decision: the call's risk (low/medium/system). */
    riskLevel?: string;
    /** Structured decision: none/workspace/system. */
    sideEffectScope?: string;
  };
  /** Pending interactive tool (quiz / ask_user_question): the question, the
   *  presented (shuffled) options, and whether a "don't know" answer exists. */
  input?: {
    id: string;
    // "sudo" is the terminal tool's password gate (docs/sudo-gate.md); the
    // failed command rides along for display.
    kind: "quiz" | "question" | "sudo";
    question: string;
    options: string[];
    hasDontKnow: boolean;
    command?: string;
  };
}

export interface HarnessEvent {
  eventId: string;
  sessionId: string;
  runId: string;
  sequence: number;
  type: HarnessEventType;
  emittedAt: string;
  payload: Record<string, unknown>;
  /** Global ledger row id; the SSE resume cursor. Absent for fixture events. */
  id?: number;
}

export interface HarnessSnapshot {
  session: HarnessSession;
  messages: HarnessMessage[];
  run: HarnessRun | null;
  events: HarnessEvent[];
  artifacts: { name: string; kind: string; detail: string }[];
}

export interface HarnessProfile {
  id: string;
  name: string;
  description?: string;
  identityPrompt?: string;
  baseModel?: string;
  // Name of the provider (connection) this profile routes to; empty = default.
  provider?: string;
  temperature?: number;
  // null = unset: the connection's vision capability decides
  vision: boolean | null;
  // Prompt-pack association. promptPackId empty = legacy identityPrompt.
  promptPackId: string;
  // Ordered module ids resolved against the pack's manifest at run time.
  promptPlan: string[];
  // Subagents (docs/subagents.md): the profile ids this profile may spawn as
  // workers via agent.spawn. Empty = may not spawn (the tool is not declared
  // for it).
  childProfileAllowlist: string[];
}

// ── Prompt packs ────────────────────────────────────────────────────────────

export interface PromptPackSummary {
  id: string;
  name: string;
  description: string;
  revision: number;
  validationStatus: "valid" | "invalid" | "unchecked" | (string & {});
  fingerprint: string;
  fileCount: number;
  createdAt: string;
  updatedAt: string;
}

export interface PromptPackFile {
  path: string;
  content: string;
  contentHash: string;
  revision: number;
  createdAt: string;
  updatedAt: string;
}

export interface PackValidation {
  valid: boolean;
  errors: string[];
  warnings: string[];
}

export interface PromptPackDetail extends PromptPackSummary {
  files: PromptPackFile[];
  validation: PackValidation;
}

// A frozen resolved [PROMPT PACK] document at one pack revision. Sessions
// reference a snapshot id instead of carrying a copy; a session keeps its
// pinned snapshot for its whole life, and pack edits land in new sessions.
interface RawPackSnapshot {
  id: number;
  pack_id?: string;
  revision?: number;
  fingerprint?: string;
  created_at?: string;
  document?: string;
}

export interface PackSnapshotSummary {
  id: number;
  packId: string;
  revision: number;
  fingerprint: string;
  createdAt: string;
}

export interface PackSnapshot extends PackSnapshotSummary {
  document: string;
}

export interface PromptPackModuleInfo {
  id: string;
  path: string;
  category: string;
  bytes: number;
  approxTokens: number;
  required: boolean;
}

export interface PromptPackPreview {
  ok: boolean;
  error?: string;
  pack: PromptPackSummary;
  validation: PackValidation;
  modules: PromptPackModuleInfo[];
  daemonBlocks: { name: string; bytes: number }[];
  dynamicBlocks: string[];
  // The exact static system prompt a run would receive (pack document plus
  // the daemon-owned blocks, joined for a single readable view).
  prompt: string;
  totalBytes: number;
  approxTokens: number;
  fingerprint: string;
}

// A named model connection (a saved provider connection): an OpenAI-compatible
// endpoint plus the sampling parameters to send to it. Profiles reference a
// provider by name; the default provider is the live provider.* settings.
export interface HarnessProvider {
  name: string;
  endpoint: string;
  model: string;
  apiKey: string;  // masked ("••••") or empty; write-only
  params: Record<string, string | number | boolean>;
  createdAt: string;
  updatedAt: string;
}

export interface ProviderInput {
  name: string;
  endpoint: string;
  model?: string;
  apiKey?: string;
  params?: Record<string, string | number | boolean>;
}

// Infrastructure observability (GET /api/v1/infra): the daemon's bounded
// snapshot of the box it runs on - host memory, docker containers (the
// llama OOM-kill counters), and provider reachability. The observatory
// view polls this; nothing here is reconstructed from the browser.
export interface InfraHost {
  available: boolean;
  memTotalMb: number;
  memAvailableMb: number;
  swapTotalMb: number;
  swapFreeMb: number;
  load1: number;
  load5: number;
  load15: number;
  cores: number;
}

export interface InfraContainer {
  name: string;
  state: string;
  health: string;
  restarts: number;
  memMb: number;  // -1 = not reported (stopped container)
}

export interface InfraProviderLed {
  name: string;
  endpoint: string;
  online: boolean;
  latencyMs: number;
}

export interface InfraGpuDevice {
  name: string;
  vramTotalMb: number;
  vramUsedMb: number;
  utilPct: number;
}

export interface InfraGpu {
  available: boolean;
  vendor: string;
  devices: InfraGpuDevice[];
}

export interface InfraReport {
  host: InfraHost;
  gpu: InfraGpu;
  dockerAvailable: boolean;
  containers: InfraContainer[];
  providers: InfraProviderLed[];
  collectedAtMs: number;
}

// Vision is a resolved capability (mirrors the daemon's vision_enabled):
// an explicit profile setting wins; an unset profile inherits the resolved
// provider's capability. A profile with a named connection inherits that
// connection's vision; a connection-less profile inherits the primary
// connection's (the default provider), or none when there is no primary.
export function profileVisionEnabled(
  profile: HarnessProfile | undefined,
  providers: HarnessProvider[],
  defaultProvider = "",
): boolean {
  if (!profile) return false;
  if (profile.vision === true) return true;
  if (profile.vision === false) return false;
  const name = profile.provider || defaultProvider;
  if (!name) return false;
  return providers.find((item) => item.name === name)?.params["vision"] === true;
}

// The journey document the session historian maintains (docs/memory.md):
// a compact, chronological narrative of the session, rewritten as runs
// complete. The inspector's journey section shows it with a manual refresh.
export interface JourneyInfo {
  sessionId: string;
  profile: string;
  text: string;
  // Last message id the journey covers (0 = nothing covered).
  watermark: number;
  updatedAt: string;
}

interface RawSession {
  id: string;
  title?: string;
  profile?: string;
  model?: string;
  status?: string;
  updated_at?: string;
  message_count?: number;
  context_mode?: string;
  context_status?: string;
  context_error?: string;
  context_summary?: string;
  context_prompt?: string;
  source_session_id?: string;
  pack_snapshot_id?: number;
  pack_revision?: number;
  md_log_path?: string;
  // Operator labels. The daemon sends a JSON array; the array shape is
  // accepted too (forward compatibility).
  tags?: string[] | string;
  // Subagents: set when this session was spawned as a worker; the spawner's
  // session id. Empty = not a spawn child.
  parent_session_id?: string;
  // Group chat: the ordered speaker profile ids (JSON array; empty = an
  // ordinary session).
  speakers?: string[];
  // Per-session round-budget override (0/absent = the global default).
  group_rounds_override?: number;
}

interface RawProfile {
  id: string;
  name: string;
  description?: string;
  identity_prompt?: string;
  base_model?: string;
  provider?: string;
  temperature?: number;
  vision: boolean | null;
  prompt_pack_id?: string;
  prompt_plan?: string[];
  child_profile_allowlist?: string[];
}

interface RawProvider {
  name: string;
  endpoint: string;
  model?: string;
  api_key?: string;
  params?: Record<string, string | number | boolean>;
  created_at?: string;
  updated_at?: string;
}

export interface ProfileInput {
  id: string;
  name: string;
  description?: string;
  identityPrompt?: string;
  baseModel?: string;
  provider?: string;
  temperature?: number;
  // null = unset: the connection's vision capability decides
  vision: boolean | null;
  // Subagents: profile ids this profile may spawn as workers; empty/omitted
  // = may not spawn.
  childProfileAllowlist?: string[];
  // Prompt-pack association; empty id = legacy identityPrompt behavior.
  promptPackId?: string;
  // Ordered module ids (the profile's prompt plan).
  promptPlan?: string[];
}

export interface HarnessHealth {
  provider: string;
  model: string;
  contextWindow: number;
  // The resolved default provider's (primary connection's) vision capability.
  vision: boolean;
  // Name of the primary connection serving as the default provider ("" = none).
  defaultProvider: string;
}

export type SettingType = "string" | "integer" | "number" | "boolean";
export type SettingSource = "cli" | "env" | "saved" | "default";

export interface HarnessSetting {
  key: string;
  type: SettingType;
  secret: boolean;
  restartRequired: boolean;
  source: SettingSource;
  active: boolean;
  value: string | number | boolean | null;
  saved: string | number | boolean | null;
}

export interface ProviderModelOption {
  id: string;
  contextWindow: number;
}

export interface ProviderCheckResult {
  ok: boolean;
  models: ProviderModelOption[];
  error?: string;
}

// One installed skill (SKILL.md): the frontmatter fields plus the full
// instruction body. `disableModelInvocation` marks user-invoked-only skills,
// which the daemon excludes from the model's catalog. `enabled` is the
// operator toggle: a disabled skill stays installed and visible here but is
// hidden from the model (catalog + skill tool).
export interface HarnessSkill {
  name: string;
  description: string;
  path: string;
  /** The skill's directory - auxiliary files (references, schemas, scripts) live here. */
  dir?: string;
  /** Files in the skill directory, relative to it (includes SKILL.md). */
  files?: string[];
  disableModelInvocation: boolean;
  content: string;
  enabled: boolean;
}

/** A trashed skill: deleted skills are moved to the trash root, not removed, so they can be restored. */
export interface HarnessSkillTrash {
  name: string;
  dir: string;
}

export interface SkillInput {
  name: string;
  description: string;
  content: string;
  disableModelInvocation: boolean;
}

export interface MemoryStatus {
  enabled: boolean;
  provider?: string;
  endpoint: string;
  bank: string;
  healthy: boolean;
  error?: string;
  jobs: Record<string, number>;
  suppressions?: Record<string, number>;
  records?: Record<string, number>;
  // Last consolidation sweep (audit: trigger, selection, changes, fixpoint).
  last_sweep?: MemorySweepInfo;
  // Custom (in-process) provider wiring: whether the embedding model and
  // reranker actually loaded from their configured paths.
  custom?: {
    ready: boolean;
    reranker_loaded: boolean;
    embed_model?: string;
    rerank_model?: string;
    units?: Record<string, number>;
    last_sweep?: MemorySweepInfo;
    // Consolidation candidates by status (pending | approved | rejected).
    consolidations?: Record<string, number>;
    // Active core-pinned units (injected verbatim into every prompt).
    core?: number;
  };
}

export interface MemoryRecallItem {
  id: string;
  text: string;
  type: string;
  document_id?: string;
  tags?: string[];
  session_id?: string;
  role?: string;
  created_at?: string;
}

export interface MemoryRecallResponse {
  source: string;
  fallback?: boolean;
  query: string;
  results: MemoryRecallItem[];
}

export interface MemoryBankUnit {
  id: string;
  type: string;
  state: string;
  state_reason?: string;
  scope: string;
  session_id?: string;
  profile?: string;
  run_id?: string;
  text: string;
  store_reason?: string;
  model_id?: string;
  confidence?: number;
  mentioned_at?: string;
  created_at?: string;
  updated_at?: string;
  core?: boolean;  // pinned: injected verbatim into every prompt
}

export interface MemoryBankResponse {
  total: number;
  units: MemoryBankUnit[];
}

export interface MemoryUnitVersion {
  id: string;
  text: string;
  model_id?: string;
  store_reason?: string;
  created_at?: string;
  current: boolean;
}

export interface MemoryUnitLink {
  target_id: number;
  link_type: string;
}

export interface MemoryUnitFeedback {
  offered: number;
  last_offered_at?: string;
  cited: number;
  last_cited_at?: string;
  used: number;
  last_used_at?: string;
  acted_on: number;
  last_acted_on_at?: string;
}

export interface MemorySweepInfo {
  trigger: string;
  watermark?: string;
  selected: number;
  changes: number;
  fixpoint: boolean;
  finished_at?: string;
}

export interface MemoryUnitDetail {
  unit: MemoryBankUnit;
  versions: MemoryUnitVersion[];
  links: MemoryUnitLink[];
  feedback?: MemoryUnitFeedback;
}

export type MemoryRecordStatus = "candidate" | "approved" | "rejected" | "superseded";

export interface MemoryRecord {
  id: number;
  status: MemoryRecordStatus;
  type: string;
  text: string;
  scope: string;
  session_id?: string;
  profile?: string;
  run_id?: string;
  source_message_ids?: string;
  provenance?: string;
  contradicts_id?: number;
  superseded_by_id?: number;
  created_at?: string;
  updated_at?: string;
  decided_at?: string;
}

export interface MemoryRecordInput {
  text: string;
  type?: string;
  scope?: string;
  session_id?: string;
  profile?: string;
  run_id?: string;
  source_message_ids?: string;
  provenance?: string;
}

export type MemoryConsolidationStatus = "pending" | "approved" | "rejected";

// near_dup: the same fact reworded (cosine above the sweep threshold).
// conflict: same-topic texts with disjoint values (mid-band cosine).
export type MemoryConsolidationKind = "near_dup" | "conflict";

export interface MemoryConsolidation {
  id: number;
  older_unit_id: number;
  newer_unit_id: number;
  cosine: number;
  scope: string;
  kind?: MemoryConsolidationKind;
  status: MemoryConsolidationStatus;
  created_at?: string;
  decided_at?: string;
  older_text?: string;
  newer_text?: string;
}

interface RawSetting {
  key: string;
  type?: string;
  secret?: boolean;
  restart_required?: boolean;
  source?: string;
  active?: boolean;
  value?: string | number | boolean | null;
  saved?: string | number | boolean | null;
}

interface RawSkill {
  name?: string;
  description?: string;
  path?: string;
  dir?: string;
  files?: string[];
  disable_model_invocation?: boolean;
  content?: string;
  enabled?: boolean;
}

interface RawSkillTrash {
  name?: string;
  dir?: string;
}

interface RawPromptSnippet {
  name?: string;
  placement?: string;
  order?: number;
  content?: string;
}

interface RawAttachment {
  id?: string | number;
  name?: string;
  kind?: string;
  mime_type?: string;
  size?: number;
  width?: number;
  height?: number;
  content?: string;
  content_url?: string;
}

function normalizeSession(raw: RawSession): HarnessSession {
  const status = raw.status === "running" || raw.status === "waiting" ? raw.status : "idle";
  const contextMode = raw.context_mode === "summary" ? "summary" : "selected";
  const contextStatus = raw.context_status === "preparing" || raw.context_status === "failed" ? raw.context_status : "ready";
  return {
    id: raw.id,
    title: raw.title || "new conversation",
    profile: raw.profile || "orchestrator",
    model: raw.model || "",
    status,
    updatedAt: raw.updated_at || nowIso(),
    messageCount: Number(raw.message_count || 0),
    contextMode,
    contextStatus,
    contextError: raw.context_error || "",
    contextSummary: raw.context_summary || "",
    contextPrompt: raw.context_prompt || "",
    sourceSessionId: raw.source_session_id || "",
    packSnapshotId: Number(raw.pack_snapshot_id || 0) || undefined,
    packRevision: Number(raw.pack_revision || 0) || undefined,
    mdLogPath: raw.md_log_path || "",
    tags: Array.isArray(raw.tags)
      ? raw.tags.filter((tag) => typeof tag === "string" && tag.length > 0)
      : typeof raw.tags === "string" && raw.tags.length > 0
        ? raw.tags.split(",").map((tag) => tag.trim()).filter(Boolean)
        : undefined,
    parentSessionId: raw.parent_session_id || "",
    speakers: Array.isArray(raw.speakers)
      ? raw.speakers.filter((speaker) => typeof speaker === "string" && speaker.length > 0)
      : undefined,
    groupRoundsOverride: Number(raw.group_rounds_override || 0) > 0
      ? Number(raw.group_rounds_override)
      : undefined,
  };
}

function optionalDimension(value: number | undefined): number | undefined {
  return value !== undefined && Number.isFinite(value) && value > 0 ? value : undefined;
}

function normalizeAttachment(raw: RawAttachment): HarnessAttachment {
  const content = raw.content ?? "";
  const size = Number(raw.size ?? content.length);
  return {
    ...(raw.id !== undefined ? { id: String(raw.id) } : {}),
    name: raw.name || "attachment",
    kind: raw.kind === "image" ? "image" : "text",
    ...(raw.mime_type ? { mimeType: raw.mime_type } : {}),
    size: Number.isFinite(size) && size >= 0 ? size : content.length,
    ...(optionalDimension(raw.width) !== undefined ? { width: optionalDimension(raw.width) } : {}),
    ...(optionalDimension(raw.height) !== undefined ? { height: optionalDimension(raw.height) } : {}),
    content,
    ...(raw.content_url ? { contentUrl: raw.content_url } : {}),
  };
}

function normalizeProfile(raw: RawProfile): HarnessProfile {
  return {
    id: raw.id,
    name: raw.name,
    description: raw.description,
    identityPrompt: raw.identity_prompt,
    baseModel: raw.base_model,
    ...(raw.provider ? { provider: raw.provider } : {}),
    temperature: raw.temperature,
    vision: raw.vision === true ? true : raw.vision === false ? false : null,
    promptPackId: raw.prompt_pack_id || "",
    promptPlan: Array.isArray(raw.prompt_plan) ? raw.prompt_plan.filter((id) => typeof id === "string") : [],
    childProfileAllowlist: Array.isArray(raw.child_profile_allowlist) ? raw.child_profile_allowlist.filter((id) => typeof id === "string" && id.length > 0) : [],
  };
}

function normalizeProvider(raw: RawProvider): HarnessProvider {
  return {
    name: raw.name,
    endpoint: raw.endpoint,
    model: raw.model || "",
    apiKey: raw.api_key || "",
    params: raw.params && typeof raw.params === "object" ? raw.params : {},
    createdAt: raw.created_at || "",
    updatedAt: raw.updated_at || "",
  };
}

function normalizeSetting(raw: RawSetting): HarnessSetting {
  const type: SettingType =
    raw.type === "integer" || raw.type === "number" || raw.type === "boolean" ? raw.type : "string";
  const source: SettingSource =
    raw.source === "cli" || raw.source === "env" || raw.source === "saved" ? raw.source : "default";
  return {
    key: raw.key,
    type,
    secret: raw.secret === true,
    restartRequired: raw.restart_required === true,
    source,
    active: raw.active === true,
    value: raw.value === null || raw.value === undefined ? null : raw.value,
    saved: raw.saved === null || raw.saved === undefined ? null : raw.saved,
  };
}

function normalizeSkill(raw: RawSkill): HarnessSkill {
  return {
    name: String(raw.name || ""),
    description: String(raw.description || ""),
    path: String(raw.path || ""),
    dir: raw.dir ? String(raw.dir) : undefined,
    files: Array.isArray(raw.files) ? raw.files.map(String) : [],
    disableModelInvocation: raw.disable_model_invocation === true,
    content: String(raw.content || ""),
    enabled: raw.enabled !== false,
  };
}

function normalizeSkillTrash(raw: RawSkillTrash): HarnessSkillTrash {
  return {
    name: String(raw.name || ""),
    dir: String(raw.dir || ""),
  };
}

function normalizePromptSnippet(raw: RawPromptSnippet): PromptSnippet {
  return {
    name: String(raw.name || ""),
    placement: raw.placement === "prepend" ? "prepend" : "append",
    order: Number(raw.order || 0),
    content: String(raw.content || ""),
  };
}

function serializeAttachment(attachment: HarnessAttachment): Record<string, unknown> {
  return {
    ...(attachment.id ? { id: attachment.id } : {}),
    name: attachment.name,
    kind: attachment.kind,
    ...(attachment.mimeType ? { mime_type: attachment.mimeType } : {}),
    size: attachment.size,
    ...(attachment.width !== undefined ? { width: attachment.width } : {}),
    ...(attachment.height !== undefined ? { height: attachment.height } : {}),
    content: attachment.content,
    ...(attachment.contentUrl ? { content_url: attachment.contentUrl } : {}),
  };
}

export interface HarnessClient {
  readonly mode: HarnessMode;
  readonly connectionTarget: string;
  listSessions(): Promise<HarnessSession[]>;
  // Group chat: pass 2+ speaker profile ids to create a group session (one
  // shared transcript, ordered round-robin turns).
  createSession(profile?: string, speakers?: string[], groupRounds?: number): Promise<HarnessSession>;
  // Set a group session's round-budget override (null = clear, the global
  // group.max_rounds setting applies again). Applies from the next group run.
  setSessionGroupRounds(sessionId: string, rounds: number | null): Promise<void>;
  // Group chat invite: append a profile to an existing conversation (a 1:1
  // becomes a group; a group gains a speaker). Returns the post-join order.
  addSessionSpeaker(sessionId: string, profileId: string): Promise<string[]>;
  loadSession(sessionId: string): Promise<HarnessSnapshot>;
  listProfiles(): Promise<HarnessProfile[]>;
  listModels(): Promise<{ default: string; models: string[] }>;
  listSkills(): Promise<HarnessSkill[]>;
  // Save (create or overwrite) a skill's SKILL.md in the workspace's
  // .agents/skills/; the daemon re-discovers and returns the updated list.
  saveSkill(input: SkillInput): Promise<HarnessSkill[]>;
  deleteSkill(name: string): Promise<HarnessSkill[]>;
  // Trash/restore: delete moves a skill to the trash root (recovery path for
  // model-self-editable skills); these manage the trashed set.
  listSkillTrash(): Promise<HarnessSkillTrash[]>;
  restoreSkill(name: string): Promise<HarnessSkill[]>;
  deleteSkillTrash(name: string): Promise<HarnessSkillTrash[]>;
  // Per-skill enable toggle: a disabled skill stays installed but is hidden
  // from the model (catalog + skill tool).
  setSkillEnabled(name: string, enabled: boolean): Promise<HarnessSkill[]>;
  // Prompt snippets: per-message toggleable rule files (the composer merges
  // selected bodies into the sent message; toggles reset after each send).
  listPromptSnippets(): Promise<PromptSnippet[]>;
  savePromptSnippet(input: PromptSnippetInput): Promise<PromptSnippet[]>;
  deletePromptSnippet(name: string): Promise<PromptSnippet[]>;
  health(): Promise<HarnessHealth>;
  listSettings(): Promise<HarnessSetting[]>;
  updateSettings(updates: Record<string, string | number | boolean>): Promise<HarnessSetting[]>;
  checkProvider(input: { endpoint: string; apiKey?: string }): Promise<ProviderCheckResult>;
  createProfile(input: ProfileInput): Promise<HarnessProfile>;
  updateProfile(input: ProfileInput): Promise<HarnessProfile>;
  deleteProfile(id: string): Promise<void>;
  renameProfile(oldId: string, newId: string): Promise<void>;
  // Prompt packs: upload, edit, validate, and preview modular prompts.
  listPromptPacks(): Promise<PromptPackSummary[]>;
  createPromptPack(input: {
    id: string;
    name: string;
    description?: string;
    files?: { path: string; content: string }[];
  }): Promise<PromptPackDetail>;
  getPromptPack(id: string): Promise<PromptPackDetail>;
  updatePromptPack(id: string, name: string, description: string): Promise<PromptPackSummary>;
  upsertPackFile(packId: string, path: string, content: string): Promise<PackValidation>;
  deletePackFile(packId: string, path: string): Promise<PackValidation>;
  deletePromptPack(id: string): Promise<void>;
  validatePromptPack(id: string): Promise<PackValidation>;
  previewPromptPack(packId: string, plan: string[], profileId?: string): Promise<PromptPackPreview>;
  // Pack snapshots: frozen resolved documents that sessions reference.
  listPackSnapshots(packId: string): Promise<PackSnapshotSummary[]>;
  getPackSnapshot(id: number): Promise<PackSnapshot>;
  selectProfile(sessionId: string, profileId: string): Promise<void>;
  answerInput(runId: string, inputId: string, choice: number): Promise<void>;
  // The sudo password gate's answer (docs/sudo-gate.md): the password rides
  // to the daemon over the same authorized command channel as approvals and
  // is consumed once - echoed nowhere.
  answerSudo(runId: string, inputId: string, password: string, scope: string): Promise<void>;
  // Resolve the pending compaction fallback prompt (auxiliary summary
  // failed): summarize with the primary model, take the deterministic
  // document, or retry the auxiliary once.
  respondContextDecision(
    sessionId: string,
    decision: "primary" | "deterministic" | "retry",
  ): Promise<void>;
  // Operator-triggered compaction ("compress now"); idle sessions only.
  compactContext(sessionId: string): Promise<CompactContextResult>;
  listProviders(): Promise<HarnessProvider[]>;
  upsertProvider(input: ProviderInput): Promise<HarnessProvider>;
  deleteProvider(name: string): Promise<void>;
  checkProviderRoute(name: string): Promise<ProviderCheckResult>;
  getInfra(): Promise<InfraReport>;
  deleteSession(sessionId: string): Promise<void>;
  renameSession(sessionId: string, title: string): Promise<void>;
  setSessionTags(sessionId: string, tags: string[]): Promise<void>;
  retitleSession(sessionId: string): Promise<void>;
  // null = no journey written for the session yet (daemon 404).
  getJourney(sessionId: string): Promise<JourneyInfo | null>;
  // Enqueue a historian job; the rewrite lands a few seconds later.
  requestJourneyUpdate(sessionId: string): Promise<void>;
  linkVault(sessionId: string, path: string): Promise<void>;
  unlinkVault(sessionId: string): Promise<void>;
  listFs(path: string): Promise<FsListing>;
  getSessionStatus(sessionId: string): Promise<HarnessSessionStatus>;
  listSources(sessionId: string): Promise<SourceInfo[]>;
  rewind(input: { sessionId: string; messageId: number; trim?: boolean }): Promise<void>;
  deleteMessage(input: { sessionId: string; messageId: number }): Promise<void>;
  editMessage(input: {
    sessionId: string;
    messageId: number;
    content: string;
  }): Promise<{ runId: string; newMessageId?: number }>;
  selectMessage(input: { sessionId: string; messageId: number }): Promise<void>;
  forkSession(input: { sourceId: string; path: number[] | "active"; mode?: ForkMode }): Promise<HarnessSession>;
  sendMessage(input: {
    sessionId: string;
    message: string;
    profile: string;
    attachments?: HarnessAttachment[];
    mentions?: string[];
    messageId?: number;
  }): Promise<{ runId: string; messageId?: number }>;
  regenerate(input: { sessionId: string; runId: string }): Promise<{ runId: string }>;
  continueRun(input: { sessionId: string; runId: string }): Promise<{ runId: string }>;
  selectIteration(input: { sessionId: string; runId: string }): Promise<void>;
  searchSession(sessionId: string, query: string): Promise<SearchResponse>;
  searchAll(query: string): Promise<GlobalSearchResponse>;
  memoryStatus(): Promise<MemoryStatus>;
  memoryRecall(input: { query: string; session?: string; profile?: string }): Promise<MemoryRecallResponse>;
  memoryBank(input?: {
    text?: string;
    type?: string;
    scope?: string;
    state?: string;
    limit?: number;
    offset?: number;
  }): Promise<MemoryBankResponse>;
  getMemoryUnit(id: string): Promise<MemoryUnitDetail>;
  invalidateMemoryUnit(id: string, reason?: string): Promise<void>;
  reactivateMemoryUnit(id: string, reason?: string): Promise<void>;
  setMemoryUnitCore(id: string, on: boolean): Promise<void>;
  supersedeMemoryUnit(id: string, text: string, reason?: string): Promise<void>;
  deleteMemoryUnit(id: string): Promise<void>;
  consolidateMemory(): Promise<{ ok: boolean; job_id: number }>;
  listConsolidations(status?: string): Promise<MemoryConsolidation[]>;
  approveConsolidation(id: number): Promise<MemoryConsolidation>;
  rejectConsolidation(id: number): Promise<MemoryConsolidation>;
  listMemoryRecords(status?: string): Promise<MemoryRecord[]>;
  createMemoryRecord(input: MemoryRecordInput): Promise<MemoryRecord>;
  approveMemoryRecord(id: number): Promise<MemoryRecord>;
  rejectMemoryRecord(id: number): Promise<MemoryRecord>;
  supersedeMemoryRecord(id: number): Promise<MemoryRecord>;
  cancelRun(runId: string): Promise<void>;
  steerRun(runId: string): Promise<void>;
  steerSession(sessionId: string): Promise<CommandAck>;
  deliverSession(sessionId: string, queuedRunId: string): Promise<CommandAck>;
  cancelSession(sessionId: string): Promise<CommandAck>;
  respondApproval(
    runId: string,
    approvalId: string,
    decision: "allow_once" | "allow_turn" | "allow_session" | "allow_always" | "deny",
  ): Promise<void>;
  // Promote a queued follow-up into a live mid-turn steer of the run
  // holding the session slot: the active run continues with the
  // queued text in context instead of ending.
  promoteQueuedRun(input: { liveRunId: string; queuedRunId: string }): Promise<void>;
  // Tiered always-allow: the rules approved with "always" scope (persisted
  // server-side; they run ungated in every session and survive restarts).
  listAlwaysAllowed(): Promise<ApprovalRule[]>;
  removeAlwaysAllowed(tool: string): Promise<void>;
  streamSession(
    sessionId: string,
    onEvent: (event: HarnessEvent) => void,
    onConnection: (connected: boolean) => void,
  ): () => void;
}

const EVENT_TYPES: HarnessEventType[] = [
  "run.accepted",
  "run.started",
  "run.retrying",
  "run.stalled",
  "reasoning.delta",
  "tool.started",
  "tool.progress",
  "tool.completed",
  "text.delta",
  "reasoning",
  "text",
  "approval.requested",
  "approval.resolved",
  "approval.auto",
  "approval.guard",
  "loop.guard",
  "stream.rule",
  "tool.spilled",
  "run.steered_live",
  "prompt.pack_edited",
  "input.requested",
  "input.resolved",
  "metrics.snapshot",
  "run.completed",
  "run.cancelled",
  "run.failed",
  "session.rewound",
  "context.preparing",
  "context.metrics",
  "context.completed",
  "context.failed",
  "context.fallback",
  "context.reused",
  "context.recompressing",
  "context.recompressed",
  "context.recompress_failed",
  "context.compaction_started",
  "context.compacted",
  "context.microcompacted",
  "context.summary_prompt",
  "context.summary_resolved",
  "tasks.created",
  "tasks.updated",
  "worker.started",
  "worker.completed",
  "worker.failed",
  "worker.cancelled",
  "group.created",
  "group.joined",
  "group.started",
  "group.turn.prepared",
  "run.continued",
  "group.turn.started",
  "group.abstained",
  "group.completed",
  "heartbeat",
];

function clone<T>(value: T): T {
  return JSON.parse(JSON.stringify(value)) as T;
}

function nowIso(): string {
  return new Date().toISOString();
}

// Daemon timestamps arrive as ISO strings (occasionally raw epoch values);
// normalize to epoch milliseconds for duration math.
function timestampMs(value: string): number {
  const numeric = Number(value);
  if (Number.isFinite(numeric) && value !== "") {
    if (numeric > 1e17) return numeric / 1e6;  // nanoseconds
    if (numeric > 1e14) return numeric / 1e3;  // microseconds
    if (numeric > 1e11) return numeric;        // milliseconds
    if (numeric > 1e8) return numeric * 1000;  // seconds
  }
  const parsed = Date.parse(value);
  return Number.isFinite(parsed) ? parsed : Number.NaN;
}

function normalizeEvent(
  raw: Record<string, unknown>,
  sessionId: string,
): HarnessEvent {
  const payload =
    raw.payload && typeof raw.payload === "object"
      ? (raw.payload as Record<string, unknown>)
      : raw;
  const type = String(raw.type || raw.event_type || "heartbeat") as HarnessEventType;
  const rowId = Number(raw.id);
  return {
    eventId: String(raw.event_id || `${sessionId}:${raw.seq || Date.now()}`),
    sessionId: String(raw.session_id || sessionId),
    runId: String(raw.run_id || payload.run_id || ""),
    sequence: Number(raw.seq || raw.sequence || 0),
    type: EVENT_TYPES.includes(type) ? type : "heartbeat",
    emittedAt: String(raw.emitted_at || raw.created_at || nowIso()),
    payload,
    ...(Number.isFinite(rowId) && rowId > 0 ? { id: rowId } : {}),
  };
}

// The daemon's snapshot carries no run object, but the persisted event ledger
// holds everything the inspector needs: the route (run.started), token usage
// (metrics.snapshot), and the terminal status. Rebuilding from events keeps
// the run's final state visible after it completes or the page reloads.
function buildRunFromEvents(events: HarnessEvent[], sessionId: string, start: HarnessEvent): HarnessRun {
  const runId = start.runId;
  const adopted = start.payload.profile_adopted;
  const drift = typeof start.payload.profile_drift === "string" && start.payload.profile_drift
    ? String(start.payload.profile_drift).split(",").filter(Boolean)
    : [];
  const run: HarnessRun = {
    id: runId,
    sessionId,
    profile: String(start.payload.profile || "orchestrator"),
    profileRevision: Number(start.payload.profile_revision || 0),
    profileFingerprint: start.payload.profile_fingerprint
      ? String(start.payload.profile_fingerprint)
      : undefined,
    profileAdopted: adopted === "true" || adopted === true,
    profileDrift: drift,
    model: String(start.payload.model || ""),
    provider: String(start.payload.provider || "openai"),
    status: "running",
    startedAt: start.emittedAt,
    reasoning: "",
    content: "",
    tools: [],
    metrics: {
      inputTokens: 0,
      outputTokens: 0,
      reasoningTokens: 0,
      contextUsed: 0,
      // 0 = unknown: the daemon did not report this connection's window.
      contextWindow: Number(start.payload.context_window) > 0 ? Number(start.payload.context_window) : 0,
      tokensPerSecond: 0,
    },
  };
  // Per-step complete events supersede the deltas of the same step (the
  // daemon drops a step's deltas when its complete event is emitted). When
  // deltas are present (live stream) they already carry the step's text;
  // when they are absent (ledger after the run ended) the complete event
  // carries it. Track which we saw so neither path double-counts.
  let sawReasoningDelta = false;
  let sawTextDelta = false;
  // Time to first output token: the first text event of the run.
  let firstTextAt: number | undefined;
  const startAt = timestampMs(start.emittedAt);
  for (const event of events) {
    if (event.runId !== runId) continue;
    if (event.type === "reasoning.delta") {
      sawReasoningDelta = true;
      run.reasoning += String(event.payload.text || "");
    } else if (event.type === "reasoning") {
      if (!sawReasoningDelta) run.reasoning += String(event.payload.text || "");
      sawReasoningDelta = false;
    } else if (event.type === "text.delta") {
      if (firstTextAt === undefined) firstTextAt = timestampMs(event.emittedAt);
      sawTextDelta = true;
      run.content += String(event.payload.text || "");
    } else if (event.type === "text") {
      if (firstTextAt === undefined) firstTextAt = timestampMs(event.emittedAt);
      if (!sawTextDelta) run.content += String(event.payload.text || "");
      sawTextDelta = false;
    } else if (event.type === "metrics.snapshot") {
      const prompt = Number(event.payload.prompt_tokens || 0);
      const completion = Number(event.payload.completion_tokens || 0);
      if (prompt > 0) {
        run.metrics.inputTokens = prompt;
        run.metrics.contextUsed = prompt;
      }
      if (completion > 0) run.metrics.outputTokens = completion;
    } else if (event.type === "tool.started") {
      const id = String(event.payload.tool_id || event.eventId);
      const existing = run.tools.find((candidate) => candidate.id === id);
      if (existing) {
        existing.status = "running";
        existing.name = String(event.payload.name || existing.name);
        existing.target = String(event.payload.target || existing.target);
        if (event.payload.arguments !== undefined) existing.arguments = String(event.payload.arguments || "");
      } else {
        run.tools.push({
          id,
          name: String(event.payload.name || "tool"),
          target: String(event.payload.target || "runtime"),
          status: "running",
          startedAt: event.emittedAt,
          ...(event.payload.arguments !== undefined ? { arguments: String(event.payload.arguments || "") } : {}),
        });
      }
    } else if (event.type === "tool.progress") {
      // Ephemeral mid-flight state (e.g. the camofox tier's live page
      // title/URL). Ledgered, so a refresh mid-fetch restores the card.
      const id = String(event.payload.tool_id || "");
      const tool = run.tools.find((candidate) => candidate.id === id && candidate.status === "running");
      if (tool && event.payload.status) {
        tool.progress = {
          status: String(event.payload.status),
          ...(typeof event.payload.title === "string" && event.payload.title ? { title: String(event.payload.title) } : {}),
          ...(typeof event.payload.url === "string" && event.payload.url ? { url: String(event.payload.url) } : {}),
          ...(typeof event.payload.frame === "string" && event.payload.frame ? { frame: String(event.payload.frame) } : {}),
        };
      }
    } else if (event.type === "tool.completed") {
      const id = String(event.payload.tool_id || event.eventId);
      const tool = run.tools.find((candidate) => candidate.id === id);
      if (tool) {
        tool.status = event.payload.error ? "failed" : "completed";
        tool.durationMs = Number(event.payload.duration_ms || 0);
        tool.summary = String(event.payload.summary || event.payload.result || event.payload.error || "");
        tool.result = String(event.payload.result || event.payload.summary || event.payload.error || "");
      } else {
        run.tools.push({
          id,
          name: String(event.payload.name || "tool"),
          target: String(event.payload.target || "runtime"),
          status: event.payload.error ? "failed" : "completed",
          durationMs: Number(event.payload.duration_ms || 0),
          result: String(event.payload.result || event.payload.summary || event.payload.error || ""),
        });
      }
    } else if (event.type === "approval.requested") {
      const approval = {
        id: String(event.payload.approval_id || event.eventId),
        name: String(event.payload.name || "tool"),
        arguments: String(event.payload.arguments || ""),
        scope: String(event.payload.scope || "once"),
        oldContent: event.payload.old_content ? String(event.payload.old_content) : undefined,
        fingerprint: event.payload.fingerprint ? String(event.payload.fingerprint) : undefined,
        patternAllowable: event.payload.pattern_allowable === "true",
        riskLevel: event.payload.risk_level ? String(event.payload.risk_level) : undefined,
        sideEffectScope: event.payload.side_effect_scope ? String(event.payload.side_effect_scope) : undefined,
      };
      const existing = run.tools.find((candidate) => candidate.id === approval.id);
      if (existing) {
        existing.status = "waiting_approval";
        existing.name = approval.name;
        existing.arguments = approval.arguments;
      } else {
        run.tools.push({
          id: approval.id,
          name: approval.name,
          target: "runtime",
          status: "waiting_approval",
          arguments: approval.arguments,
        });
      }
      run.status = "waiting_approval";
      run.approval = approval;
    } else if (event.type === "approval.resolved") {
      const id = String(event.payload.approval_id || "");
      const tool = run.tools.find((candidate) => candidate.id === id);
      if (tool && event.payload.decision !== "deny") tool.status = "running";
      run.approval = undefined;
      if (run.status === "waiting_approval") run.status = "running";
    } else if (event.type === "input.requested") {
      // Parse the presented options defensively: the daemon ships a JSON
      // array string, and a malformed payload must not break run rebuild.
      let options: string[] = [];
      try {
        const raw = JSON.parse(String(event.payload.options || "[]")) as unknown;
        if (Array.isArray(raw)) options = raw.map((item) => String(item));
      } catch {
        options = [];
      }
      run.status = "waiting_input";
      run.input = {
        id: String(event.payload.input_id || event.eventId),
        kind: event.payload.kind === "question"
          ? "question"
          : event.payload.kind === "sudo" ? "sudo" : "quiz",
        question: String(event.payload.question || ""),
        options,
        hasDontKnow: event.payload.has_dont_know === "true" || event.payload.has_dont_know === true,
        ...(event.payload.kind === "sudo"
          ? { command: String(event.payload.command || "") }
          : {}),
      };
    } else if (event.type === "input.resolved") {
      run.input = undefined;
      if (run.status === "waiting_input") run.status = "running";
    } else if (event.type === "run.completed" || event.type === "run.cancelled" || event.type === "run.failed") {
      run.status = event.type === "run.completed" ? "completed" : event.type === "run.cancelled" ? "cancelled" : "failed";
      run.finishedAt = event.emittedAt;
      run.approval = undefined;
      if (event.type !== "run.completed") {
        const result = event.type === "run.cancelled" ? "run cancelled" : String(event.payload.error || "run failed");
        for (const tool of run.tools) {
          if (tool.status === "running" || tool.status === "waiting_approval") {
            tool.status = "failed";
            tool.result = result;
            tool.summary = result;
          }
        }
      }
      if (event.type === "run.failed") run.error = String(event.payload.error || "");
      // Terminal events carry the run's accumulated usage across all provider
      // calls (tool iterations), which per-call snapshots do not.
      const calls = Number(event.payload.provider_calls || 0);
      const totalIn = Number(event.payload.total_prompt_tokens || 0);
      const totalOut = Number(event.payload.total_completion_tokens || 0);
      if (calls > 0) run.metrics.providerCalls = calls;
      if (totalIn > 0) run.metrics.totalInputTokens = totalIn;
      if (totalOut > 0) run.metrics.totalOutputTokens = totalOut;
    }
  }
  if (firstTextAt !== undefined && Number.isFinite(startAt) && firstTextAt >= startAt) {
    run.metrics.ttftMs = firstTextAt - startAt;
  }
  return run;
}

// Every run in the session, keyed by run id. Lets per-message UI (the info
// indicator on each assistant turn) resolve model, profile, usage, and
// duration for that specific run, including after a page reload.
export function runsFromEvents(events: HarnessEvent[], sessionId: string): Map<string, HarnessRun> {
  const runs = new Map<string, HarnessRun>();
  for (const event of events) {
    if (event.type === "run.started" && event.runId && !runs.has(event.runId)) {
      runs.set(event.runId, buildRunFromEvents(events, sessionId, event));
    }
  }
  return runs;
}

// A decision the operator still owes: an approval gate or a structured
// question, with the run and session it belongs to. Deduped by id; the
// matching resolved event (or a terminal run event) clears it.
export interface PendingAction {
  kind: "approval" | "input";
  id: string;
  sessionId: string;
  runId: string;
  requestedAt: string;
  name?: string;
  arguments?: string;
  question?: string;
  options?: string[];
  hasDontKnow?: boolean;
  /** Fingerprint rulebook: the derived "always" pattern label ("whole tool"
   *  when the call is not pattern-able). */
  fingerprint?: string;
}

// A persisted always-allow rule: the tool plus its fingerprint label
// ("whole tool" when the approval covered the tool with any arguments).
export interface ApprovalRule {
  tool: string;
  fingerprint: string;
}

// Provenance: the daemon decided without the operator (read-only argv
// auto-approve, loop-guard intervention), ledgered for the record.
export interface ProvenanceEntry {
  id: string;
  kind: "approval.auto" | "approval.guard" | "loop.guard";
  label: string;
  detail: string;
  at: string;
}

export function pendingActionsFromEvents(events: HarnessEvent[]): {
  pending: PendingAction[];
  provenance: ProvenanceEntry[];
} {
  const pending = new Map<string, PendingAction>();
  const provenance: ProvenanceEntry[] = [];
  for (const event of events) {
    if (event.type === "approval.requested") {
      const id = String(event.payload.approval_id || event.eventId);
      if (!pending.has(id)) {
        pending.set(id, {
          kind: "approval",
          id,
          sessionId: event.sessionId,
          runId: event.runId,
          requestedAt: event.emittedAt,
          name: String(event.payload.name || "a tool call"),
          arguments: String(event.payload.arguments || ""),
          fingerprint: event.payload.fingerprint ? String(event.payload.fingerprint) : undefined,
        });
      }
    } else if (event.type === "approval.resolved") {
      pending.delete(String(event.payload.approval_id || ""));
    } else if (event.type === "input.requested") {
      const id = String(event.payload.input_id || event.eventId);
      if (!pending.has(id)) {
        let options: string[] = [];
        try {
          const raw = JSON.parse(String(event.payload.options || "[]")) as unknown;
          if (Array.isArray(raw)) options = raw.map((item) => String(item));
        } catch {
          options = [];
        }
        pending.set(id, {
          kind: "input",
          id,
          sessionId: event.sessionId,
          runId: event.runId,
          requestedAt: event.emittedAt,
          question: String(event.payload.question || ""),
          options,
          hasDontKnow: event.payload.has_dont_know === "true" || event.payload.has_dont_know === true,
        });
      }
    } else if (event.type === "input.resolved") {
      pending.delete(String(event.payload.input_id || ""));
    } else if (event.type === "run.completed" || event.type === "run.cancelled" || event.type === "run.failed") {
      // A run that ended can no longer owe a decision (cancelled while the
      // gate was open, answered elsewhere, ...); clear anything it parked.
      for (const [id, action] of pending) {
        if (action.runId === event.runId) pending.delete(id);
      }
    } else if (event.type === "approval.auto") {
      // The rule id names what let the call run (readonly-argv classifier,
      // always-allow fingerprint, session scope) - ledgered provenance.
      const ruleId = event.payload.rule_id ? String(event.payload.rule_id) : "";
      provenance.push({
        id: event.eventId,
        kind: "approval.auto",
        label: ruleId
          ? `auto-approved ${String(event.payload.name || "call")} · ${ruleId}`
          : `auto-approved ${String(event.payload.name || "call")}`,
        detail: String(event.payload.arguments || ""),
        at: event.emittedAt,
      });
    } else if (event.type === "approval.guard") {
      // The deny guard's machine denial: it never opened a gate, so this is
      // provenance (like approval.auto), not a pending action.
      provenance.push({
        id: event.eventId,
        kind: "approval.guard",
        label: `denied by guard · ${String(event.payload.name || "call")}`,
        detail: String(event.payload.reason || ""),
        at: event.emittedAt,
      });
    } else if (event.type === "loop.guard") {
      const tool = event.payload.tool ? String(event.payload.tool) : "";
      provenance.push({
        id: event.eventId,
        kind: "loop.guard",
        label: `loop guard · ${String(event.payload.stage || "note")}${tool ? ` · ${tool}` : ""}${event.payload.inert === "true" ? " (inert)" : ""}`,
        detail: String(event.payload.text || ""),
        at: event.emittedAt,
      });
    }
  }
  // Newest first; the ledger window is bounded, so this stays cheap.
  return {
    pending: [...pending.values()],
    provenance: provenance.slice(-12).reverse(),
  };
}

// ── steer composer state machine ──────────────────────────────────────────
// The composer's placeholder is a state machine over the run status
// It tells the operator what Enter does in the current
// state. While a run holds the slot, Enter queues the message (steer it
// from the queue, or send it now); the attention states (approval / input
// / slot wait) say what the run is waiting on. Pure: same status, same
// placeholder.
const COMPOSER_FILE_HINT = " (@ for a file or folder, paste or drop images and files)";

export function composerPlaceholder(status?: RunStatus | null): string {
  switch (status) {
    case "queued":
      return "waiting for a generation slot. ⏎ queues your message";
    case "running":
      return "type + ⏎ to queue, steer it from the queue (end turn at next pause), or send now" + COMPOSER_FILE_HINT;
    case "waiting_approval":
      return "waiting for approval. ⏎ queues your message";
    case "waiting_input":
      return "waiting for your answer. ⏎ queues your message";
    default:
      return "send a task to the harness..." + COMPOSER_FILE_HINT;
  }
}

// The steer-flash strip copy: a transient confirmation of
// what the steer / send-now just did, rendered above the composer with a
// TTL auto-expiry. Pure: the flash text is decided at the call site.
export function steerFlashText(action: "steer" | "deliver" | "send_now" | "restore"): string {
  return action === "steer"
    ? "steered. the current turn ends at its next pause; this message takes the slot"
    : action === "deliver"
      ? "delivered. the run continues with your message in context"
      : action === "restore"
        ? "stopped. queued prompts are back in the composer for review"
        : "sent now. the current turn was stopped; the queue is starting";
}

// ── prompt snippets ───────────────────────────────────────────────────────
// Tiny single-purpose rule files (docs/prompt-snippets.md): placement
// (prepend|append) + order in the frontmatter,
// toggled per message in the composer. Selected bodies merge into the sent
// message (prepend group -> text -> append group); the toggles reset to
// all-off after each send and at session start.
export interface PromptSnippet {
  name: string;
  placement: "prepend" | "append";
  order: number;
  content: string;
}

export interface PromptSnippetInput {
  name: string;
  placement: "prepend" | "append";
  order: number;
  content: string;
}

// Pure merge: the selected snippets' bodies join the message as
// prepend-group + message + append-group, each group sorted by (order, name)
// and joined with blank lines. An empty selection returns the message
// unchanged.
export function mergePromptSnippets(
  message: string,
  snippets: PromptSnippet[],
  selected: ReadonlySet<string>,
): string {
  if (selected.size === 0) return message;
  const picked = snippets.filter((snippet) => selected.has(snippet.name));
  const sortGroup = (group: PromptSnippet[]) =>
    [...group].sort(
      (a, b) => a.order - b.order || a.name.localeCompare(b.name),
    );
  const parts: string[] = [];
  for (const body of sortGroup(picked.filter((s) => s.placement === "prepend")).map((s) => s.content.trim())) {
    if (body) parts.push(body);
  }
  parts.push(message);
  for (const body of sortGroup(picked.filter((s) => s.placement === "append")).map((s) => s.content.trim())) {
    if (body) parts.push(body);
  }
  return parts.join("\n\n");
}

// ── task checklist ────────────────────────────────────────────────────────────
// The visible checklist the tasks.create/tasks.update tools project to the
// ledger. A single pinned card renders the latest state, replayed from the
// event stream (no server-side state: tasks.created replaces, tasks.updated
// toggles the 1-based index; stale indices are ignored). The card is the
// live view: pinned at the bottom while live (0/N..n/N); once every item is
// done, or the list is discarded (tasks.create []), the card lingers in the
// transcript at the last tick's location - completed items keep their green
// check, discarded ones get a red X. The transcript keeps small markers at
// the points where state changed, so the history stays legible.
export interface TaskItem {
  text: string;
  done: boolean;
}

export function tasksFromEvents(
  events: HarnessEvent[],
  activeRunIds?: Set<string>,
): TaskItem[] {
  let tasks: TaskItem[] = [];
  for (const event of events) {
    // Rewind scoping: the ledger is append-only, so a rewound turn's
    // tasks.created keeps replaying and would resurrect the list as a
    // ghost. When the caller passes the active path's run ids, events
    // from work no longer attached to the transcript are skipped (omit
    // the set to replay everything).
    if (activeRunIds && !activeRunIds.has(event.runId)) continue;
    if (event.type === "tasks.created") {
      try {
        const raw = JSON.parse(String(event.payload.tasks || "[]")) as unknown;
        tasks = Array.isArray(raw) ? raw.map((entry) => ({ text: String(entry), done: false })) : [];
      } catch {
        tasks = [];
      }
    } else if (event.type === "tasks.updated") {
      const index = Number(event.payload.task || 0);
      const status = String(event.payload.status || "");
      if (index >= 1 && index <= tasks.length) {
        tasks[index - 1].done = status === "done";
      }
    }
  }
  return tasks;
}

// A checklist that CLOSED in the ledger, so its card can linger at its own
// transcript location: a completed list anchors at its last tick (all green),
// a discarded list (tasks.create []) anchors at the discard marker (done items
// green, the rest red X'd - they never ran). A list merely replaced while
// live leaves no record. Each closed list is independent, so a later
// create/discard never removes an earlier record.
export interface TaskChecklistRecord {
  anchor: string;
  tasks: TaskItem[];
  kind: "completed" | "discarded";
}

export function taskChecklistRecords(events: HarnessEvent[]): TaskChecklistRecord[] {
  let current: TaskItem[] = [];
  let currentAnchor = "";
  const records: TaskChecklistRecord[] = [];
  const closeCompleted = () => {
    if (current.length > 0 && current.every((task) => task.done)) {
      records.push({ anchor: currentAnchor, tasks: current.map((task) => ({ ...task })), kind: "completed" });
    }
  };
  for (const event of events) {
    if (event.type === "tasks.created") {
      let created: TaskItem[] = [];
      try {
        const raw = JSON.parse(String(event.payload.tasks || "[]")) as unknown;
        created = Array.isArray(raw) ? raw.map((entry) => ({ text: String(entry), done: false })) : [];
      } catch {
        created = [];
      }
      if (created.length === 0) {
        if (current.length > 0) {
          records.push({ anchor: event.eventId, tasks: current.map((task) => ({ ...task })), kind: "discarded" });
        }
        current = [];
        currentAnchor = "";
      } else {
        // A new list closes the previous one if it had completed.
        closeCompleted();
        current = created;
        currentAnchor = event.eventId;
      }
    } else if (event.type === "tasks.updated") {
      const index = Number(event.payload.task || 0);
      const status = String(event.payload.status || "");
      if (index >= 1 && index <= current.length) {
        current[index - 1].done = status === "done";
        currentAnchor = event.eventId;
      }
    }
  }
  closeCompleted();
  return records;
}

// ── group chat (docs/group-chat.md) ──────────────────────────────────────
// The group's live state, replayed from the ledger so it survives a reload:
// the roster (group.created / group.started), the round counter (the last
// group.turn.started's round), the current speaker, and the termination
// (group.completed). The newest group.started that has no group.completed
// after it is the live group.
export interface GroupState {
  speakers: string[];
  active: boolean;
  round: number;
  maxRounds: number;
  currentSpeaker: string | null;
  // The chosen-next speaker between group.turn.prepared and their
  // group.turn.started: the banner's "X is preparing" window.
  prepared: string | null;
  termination: string | null;
}

export function groupStateFromEvents(events: HarnessEvent[]): GroupState {
  const empty: GroupState = {
    speakers: [],
    active: false,
    round: 0,
    maxRounds: 0,
    currentSpeaker: null,
    prepared: null,
    termination: null,
  };
  let state: GroupState = empty;
  for (const event of events) {
    if (event.type === "group.created") {
      const speakers = Array.isArray(event.payload.speakers)
        ? event.payload.speakers.filter((s): s is string => typeof s === "string")
        : [];
      if (speakers.length > 0) state = { ...state, speakers };
    } else if (event.type === "group.joined") {
      // Invite: the roster change lands before any group.started, so it must
      // apply whether or not a group is active (the same rule group.created
      // uses). The payload's speakers is the full post-join roster.
      const speakers = Array.isArray(event.payload.speakers)
        ? event.payload.speakers.filter((s): s is string => typeof s === "string")
        : [];
      if (speakers.length > 0) state = { ...state, speakers };
    } else if (event.type === "group.started") {
      const speakers = Array.isArray(event.payload.speakers)
        ? event.payload.speakers.filter((s): s is string => typeof s === "string")
        : state.speakers;
      state = {
        speakers,
        active: true,
        round: 0,
        maxRounds: Number(event.payload.max_rounds || 0),
        currentSpeaker: null,
        prepared: null,
        termination: null,
      };
    } else if (event.type === "group.turn.prepared" && state.active) {
      state = {
        ...state,
        prepared: typeof event.payload.speaker === "string" ? event.payload.speaker : state.prepared,
      };
    } else if (event.type === "group.turn.started" && state.active) {
      state = {
        ...state,
        round: Number(event.payload.round || state.round),
        currentSpeaker: typeof event.payload.speaker === "string" ? event.payload.speaker : state.currentSpeaker,
        prepared: null,
      };
    } else if (event.type === "group.completed" && state.active) {
      state = {
        ...state,
        active: false,
        currentSpeaker: null,
        prepared: null,
        termination: typeof event.payload.termination === "string" ? event.payload.termination : "stopped",
      };
    }
  }
  return state;
}

// One rendered row of a group transcript: a committed message, or a
// recorded abstention (a speaker with nothing to add leaves a marker at
// their turn's position - the ledgered closure signal made visible).
export type GroupTranscriptItem =
  | { kind: "message"; message: HarnessMessage }
  | { kind: "abstained"; speaker: string; round: number; turnRun: string };

// Interleave the session's active-path messages with the recorded
// abstentions, in ledger order. Ordering keys: an assistant message sorts
// at its run's terminal event (run.completed/cancelled/failed - the moment
// it committed), a user message at its run's run.accepted (the group run
// opens the transcript), and an abstention at its own event. A message
// whose run has not terminated yet (live streaming) sorts last, after
// everything committed.
export function groupTranscriptItems(
  messages: HarnessMessage[],
  events: HarnessEvent[],
): GroupTranscriptItem[] {
  const terminalIndex = new Map<string, number>();
  const acceptedIndex = new Map<string, number>();
  events.forEach((event, index) => {
    if (event.type === "run.accepted" && event.runId && !acceptedIndex.has(event.runId)) {
      acceptedIndex.set(event.runId, index);
    }
    if (
      (event.type === "run.completed" ||
        event.type === "run.cancelled" ||
        event.type === "run.failed") &&
      event.runId &&
      !terminalIndex.has(event.runId)
    ) {
      terminalIndex.set(event.runId, index);
    }
  });
  const last = events.length;
  type Orderable = { pos: number; item: GroupTranscriptItem };
  const ordered: Orderable[] = [];
  for (const message of messages) {
    const runId = message.runId || "";
    const pos =
      message.role === "assistant"
        ? terminalIndex.get(runId) ?? last
        : acceptedIndex.get(runId) ?? 0;
    ordered.push({ pos, item: { kind: "message", message } });
  }
  events.forEach((event, index) => {
    if (event.type !== "group.abstained") return;
    ordered.push({
      pos: index,
      item: {
        kind: "abstained",
        speaker: typeof event.payload.speaker === "string" ? event.payload.speaker : "",
        round: Number(event.payload.round || 0),
        turnRun: event.runId,
      },
    });
  });
  // Stable sort: equal positions keep their push order (messages first), so
  // a committed message and an abstention at the same ledger row render in
  // the message-then-marker order.
  ordered.sort((a, b) => a.pos - b.pos);
  return ordered.map((entry) => entry.item);
}

// ── subagents (agent.spawn) ──────────────────────────────────────────────
// The result string the daemon returns for a spawn:
//   "Worker <status> (profile <p>, session <s>, run <r>). Full transcript:
//    session <s>.\n\n<narrative>"
// status ∈ completed | failed | cancelled; the narrative is the worker's
// final message, head+tail byte-capped (the omission marker stays inline).
export interface SpawnResult {
  status: "completed" | "failed" | "cancelled";
  childSessionId: string;
  narrative: string;
}

export function parseSpawnResult(result: string): SpawnResult | null {
  const match = /^Worker (completed|failed|cancelled) \(profile [^,]+, session [^,]+, run [^)]+\)\. Full transcript: session ([^\n]+)\.\n\n([\s\S]*)$/
    .exec(result || "");
  if (!match) return null;
  return {
    status: match[1] as SpawnResult["status"],
    childSessionId: match[2].trim(),
    narrative: match[3],
  };
}

// Event window for chat block reconstruction; consolidated steps leave only
// a handful of events each, so this holds many runs' full interleaving.
export const EVENT_WINDOW = 500;

// ── bounded display history ──────────────────────────────────────────────
// The chat mounts the latest CHAT_WINDOW_SIZE-message tail; scrolling up
// grows the mounted window lazily, capped at HISTORY_MAX_LINES mounted
// messages. At the cap auto-expand stops and the transcript's top strip
// directs the reader to search / the session graph for anything older
// (anchored jumps bypass the cap). The durable ledger and the model
// context are never trimmed by any of this - it is display-only.
export const HISTORY_MAX_LINES = 500;

export function canLoadEarlier(windowStart: number, pathLength: number): boolean {
  return windowStart > 0 && pathLength - windowStart < HISTORY_MAX_LINES;
}
// Per-run cap on live delta events kept in the window. A chatty step can
// stream hundreds of deltas; uncapped, the flood evicts every earlier step's
// consolidated event and the run renders as one live block. The dropped
// chunks are safe: repairLive rebuilds the full step text from the message's
// accumulated reasoning/content.
export const LIVE_DELTA_CAP = 200;

// Lifecycle/group events that are ordering anchors, not stream payload:
// groupTranscriptItems sorts a turn at its run's terminal event and a user
// message at its run's run.accepted, and groupStateFromEvents replays the
// group.* banner state. Evicting an older run's anchors (a long group
// session outgrows the window) scrambles the transcript order, so these stay
// for every run and only the high-volume stream is capped to the tail.
const EVENT_ANCHOR_TYPES = new Set<HarnessEvent["type"]>([
  "run.accepted",
  "run.completed",
  "run.cancelled",
  "run.failed",
  "group.created",
  "group.joined",
  "group.started",
  "group.turn.prepared",
  "group.turn.started",
  "group.completed",
  "group.abstained",
]);

// Snapshot hydration keeps structural events so a refresh can reconstruct
// reasoning/tool boundaries. Streaming deltas are redundant once their step is
// consolidated, so retain only the latest bounded delta tail.
export function compactEventWindow(events: HarnessEvent[]): HarnessEvent[] {
  if (events.length <= EVENT_WINDOW) return events;
  const anchors = events.filter((event) => EVENT_ANCHOR_TYPES.has(event.type));
  const anchorIds = new Set(anchors.map((event) => event.eventId));
  const deltaIds = new Set(
    events
      .filter((event) => !anchorIds.has(event.eventId) && (event.type === "reasoning.delta" || event.type === "text.delta"))
      .slice(-LIVE_DELTA_CAP)
      .map((event) => event.eventId),
  );
  const nonAnchors = events.filter((event) =>
    !anchorIds.has(event.eventId) &&
    (event.type !== "reasoning.delta" && event.type !== "text.delta" || deltaIds.has(event.eventId)),
  );
  // The anchors are always kept; the remaining stream fills the rest of the
  // budget from the recent tail. If the anchors alone exceed the window (a
  // very long session) they win - dropping an ordering anchor is the bug.
  const budget = Math.max(0, EVENT_WINDOW - anchors.length);
  const keptIds = new Set([...anchors, ...nonAnchors.slice(-budget)].map((event) => event.eventId));
  return events.filter((event) => keptIds.has(event.eventId));
}

// Advance the client's event window with one streamed event. Consolidated
// step events supersede their deltas (the daemon's ledger deletes the same
// way), and per-run delta floods are capped so a single chatty step cannot
// evict the run's earlier steps from the window. The finishing compaction
// pins the structural events (sealed reasoning/text, tools, lifecycle) while
// capping the delta tail - sealed step events must survive the whole run or
// block reconstruction loses history mid-stream.
export function advanceEventWindow(previous: HarnessEvent[], event: HarnessEvent): HarnessEvent[] {
  const superseded = event.type === "reasoning" ? "reasoning.delta"
    : event.type === "text" ? "text.delta"
    : null;
  let kept = superseded
    ? previous.filter((candidate) => candidate.runId !== event.runId || candidate.type !== superseded)
    : previous;
  if (event.type === "reasoning.delta" || event.type === "text.delta") {
    const deltas = kept.filter((candidate) => candidate.runId === event.runId
      && (candidate.type === "reasoning.delta" || candidate.type === "text.delta"));
    if (deltas.length >= LIVE_DELTA_CAP) {
      const drop = new Set(deltas.slice(0, deltas.length - LIVE_DELTA_CAP + 1).map((candidate) => candidate.eventId));
      kept = kept.filter((candidate) => !drop.has(candidate.eventId));
    }
  }
  return compactEventWindow([...kept, event]);
}

// One rendered block of a run, in the order it happened: a reasoning trace,
// a group of tool calls, or a piece of response text.
// task blocks carry the event id so the completed card can anchor to the
// exact transcript position of the last tick (the work's location).
export type RunBlock =
  | { kind: "reasoning"; text: string }
  | { kind: "tools"; toolIds: string[] }
  | { kind: "task_created"; count: number; eventId: string }
  | { kind: "task_updated"; index: number; status: "done" | "pending"; eventId: string }
  | { kind: "text"; text: string };

// Rebuild a run's blocks from the event ledger so multi-step runs render in
// the order they happened (thinking -> tools -> text -> thinking -> tools)
// instead of one merged reasoning block, one merged tool group, and the
// final message. A step's complete event supersedes its live deltas; deltas
// that never consolidated (a failed mid-step) become their own block.
//
// fallbackReasoning/fallbackContent are the run's full accumulated
// reasoning/response (the message row's values). The event window is capped
// to its tail, so a live step's head deltas can fall out mid-stream; the live
// block is then only a suffix of the step's full text and would render as a
// mid-sentence fragment that snaps to the full text when the step
// consolidates. Repairing the live block against the fallback keeps the
// visible head stable while streaming.
export function runBlocksFromEvents(
  events: HarnessEvent[],
  runId: string,
  fallbackReasoning = "",
  fallbackContent = "",
): RunBlock[] {
  if (!runId) return [];
  const blocks: RunBlock[] = [];
  let liveReasoning = "";
  let liveText = "";
  // Consolidated steps pushed so far, by accumulated length; the fallback is
  // the run-wide total, so subtracting them isolates the current step.
  let pushedReasoning = 0;
  let pushedContent = 0;
  const repairLive = (live: string, fallback: string, pushed: number): string => {
    if (fallback.length > live.length && fallback.endsWith(live)) {
      const full = fallback.slice(pushed);
      if (full.length > live.length && full.endsWith(live)) return full;
    }
    return live;
  };
  const flushLive = () => {
    if (liveReasoning) {
      blocks.push({ kind: "reasoning", text: repairLive(liveReasoning, fallbackReasoning, pushedReasoning) });
      liveReasoning = "";
    }
    if (liveText) {
      blocks.push({ kind: "text", text: repairLive(liveText, fallbackContent, pushedContent) });
      liveText = "";
    }
  };
  for (const event of events) {
    if (event.runId !== runId) continue;
    const text = String(event.payload.text || "");
    if (event.type === "reasoning.delta") {
      liveReasoning += text;
    } else if (event.type === "text.delta") {
      liveText += text;
    } else if (event.type === "reasoning") {
      blocks.push({ kind: "reasoning", text });
      pushedReasoning += text.length;
      liveReasoning = "";
    } else if (event.type === "text") {
      blocks.push({ kind: "text", text });
      pushedContent += text.length;
      liveText = "";
    } else if (event.type === "tool.started" || event.type === "approval.requested" || event.type === "input.requested") {
      // A gated/interactive tool emits approval.requested or input.requested
      // before its tool.started, so a tools block must exist by then or the
      // pending approval/input card would be dropped from the chat (it only
      // showed in the inspector, which renders run.tools directly).
      const id = String(event.payload.tool_id || event.payload.approval_id || event.payload.input_id || event.eventId);
      const last = blocks[blocks.length - 1];
      if (last && last.kind === "tools") {
        if (!last.toolIds.includes(id)) last.toolIds.push(id);
      } else {
        blocks.push({ kind: "tools", toolIds: [id] });
      }
    } else if (event.type === "tasks.created") {
      // Small transcript marker at the spawn point (an empty list is a
      // discard): the pinned card is the live view, the marker keeps the
      // point visible after the card unpins.
      let count = 0;
      try {
        const raw = JSON.parse(String(event.payload.tasks || "[]")) as unknown;
        count = Array.isArray(raw) ? raw.length : 0;
      } catch {
        count = 0;
      }
      blocks.push({ kind: "task_created", count, eventId: event.eventId });
    } else if (event.type === "tasks.updated") {
      blocks.push({
        kind: "task_updated",
        index: Number(event.payload.task || 0),
        status: event.payload.status === "done" ? "done" : "pending",
        eventId: event.eventId,
      });
    }
  }
  // The window caps its delta tail: over a long text stream a step's
  // reasoning deltas rotate out while its sealed `reasoning` event does not
  // exist yet (it lands when the step consolidates, at stream end). Sealed
  // events are pinned by the window, so `pushedReasoning` accounts for the
  // whole sealed history; everything the fallback carries beyond it is the
  // current step's reasoning that the window can no longer show. The
  // vanish/reappear window is exactly the live text-streaming phase, so
  // recover the fallback there (the block lands before the live text). At
  // seal the step's own event replaces this deterministically.
  const missingReasoning = fallbackReasoning.slice(pushedReasoning);
  if (!liveReasoning && liveText && missingReasoning) {
    blocks.push({ kind: "reasoning", text: missingReasoning });
  }
  flushLive();
  return blocks;
}

function runFromEvents(events: HarnessEvent[], sessionId: string): HarnessRun | null {
  let start: HarnessEvent | undefined;
  for (const event of events) {
    if (event.type === "run.started" && event.runId) start = event;
  }
  return start ? buildRunFromEvents(events, sessionId, start) : null;
}

class HttpHarnessClient implements HarnessClient {
  readonly mode = "http" as const;
  private readonly baseUrl =
    (import.meta.env.VITE_HARNESS_API_URL || "/api/v1").replace(/\/$/, "");
  get connectionTarget(): string {
    return this.baseUrl;
  }

  // Highest ledger row id seen per session; the SSE stream resumes after it.
  private readonly cursors = new Map<string, number>();

  private async request<T>(path: string, options?: RequestInit): Promise<T> {
    const response = await fetch(`${this.baseUrl}${path}`, {
      credentials: "include",
      headers: { "Content-Type": "application/json", ...(options?.headers || {}) },
      ...options,
    });
    if (!response.ok) {
      // Surface the daemon's error text (e.g. "run in progress") so the UI
      // can say why the request failed, not just that it did.
      let detail = "";
      try {
        const data = (await response.json()) as { error?: unknown };
        if (data && typeof data.error === "string" && data.error) detail = `: ${data.error}`;
      } catch {
        // non-JSON error body; the status code is enough
      }
      throw new Error(`harness request failed: ${response.status}${detail}`);
    }
    return (await response.json()) as T;
  }

  async listSessions(): Promise<HarnessSession[]> {
    const data = await this.request<{ sessions: RawSession[] }>('/sessions');
    return (data.sessions || []).map(normalizeSession);
  }

  async createSession(profile = "", speakers?: string[], groupRounds?: number): Promise<HarnessSession> {
    const body: Record<string, unknown> = {};
    if (profile) body.profile = profile;
    if (speakers && speakers.length > 0) body.speakers = speakers;
    if (groupRounds !== undefined) body.group_rounds = groupRounds;
    const data = await this.request<RawSession>('/sessions', {
      method: "POST",
      body: JSON.stringify(body),
    });
    return normalizeSession(data);
  }

  async setSessionGroupRounds(sessionId: string, rounds: number | null): Promise<void> {
    await this.request(`/sessions/${encodeURIComponent(sessionId)}/group-rounds`, {
      method: "POST",
      body: JSON.stringify({ rounds }),
    });
  }

  // Group chat invite: append a profile to an existing conversation (a 1:1
  // session becomes a group; a group gains a speaker). The daemon composes
  // the roster; the response carries the post-join speaking order.
  async addSessionSpeaker(sessionId: string, profileId: string): Promise<string[]> {
    const data = await this.request<{ ok: boolean; speakers: string[]}>(
      `/sessions/${encodeURIComponent(sessionId)}/speakers`,
      {
        method: "POST",
        body: JSON.stringify({ add: profileId }),
      },
    );
    return Array.isArray(data.speakers)
      ? data.speakers.filter((speaker) => typeof speaker === "string")
      : [];
  }

  async getSessionStatus(sessionId: string): Promise<HarnessSessionStatus> {
    const data = await this.request<{
      session_id?: string;
      latest?: string;
      events?: Record<string, unknown>[];
    }>(`/sessions/${encodeURIComponent(sessionId)}/status`);
    return {
      sessionId: data.session_id || sessionId,
      latest: typeof data.latest === "string" ? data.latest : "",
      events: (data.events || []).map((raw, index) => ({
        id: Number(raw.id ?? index),
        sessionId: String(raw.session_id ?? sessionId),
        profile: String(raw.profile ?? ""),
        state: String(raw.state ?? ""),
        note: String(raw.note ?? ""),
        createdAt: String(raw.created_at ?? ""),
      })),
    };
  }

  async listSources(sessionId: string): Promise<SourceInfo[]> {
    const data = await this.request<{ sources?: Record<string, unknown>[] }>(
      `/sources?session_id=${encodeURIComponent(sessionId)}`,
    );
    return (data.sources || []).map((raw) => ({
      id: Number(raw.id ?? 0),
      url: String(raw.url ?? ""),
      title: String(raw.title ?? ""),
      status: String(raw.status ?? ""),
      used: Boolean(raw.used),
      runId: String(raw.run_id ?? ""),
      fetchedAt: String(raw.fetched_at ?? ""),
    }));
  }

  async listProfiles(): Promise<HarnessProfile[]> {
    const data = await this.request<{ profiles: RawProfile[] }>('/profiles');
    return (data.profiles || []).map(normalizeProfile);
  }

  async listModels(): Promise<{ default: string; models: string[] }> {
    const data = await this.request<{ default?: string; models?: string[] }>('/models');
    return { default: data.default || "", models: data.models || [] };
  }

  async listSkills(): Promise<HarnessSkill[]> {
    const data = await this.request<RawSkill[]>('/skills');
    return (data || []).map(normalizeSkill);
  }

  async saveSkill(input: SkillInput): Promise<HarnessSkill[]> {
    const data = await this.request<RawSkill[]>('/skills', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({
        name: input.name,
        description: input.description,
        content: input.content,
        disable_model_invocation: input.disableModelInvocation,
      }),
    });
    return (data || []).map(normalizeSkill);
  }

  async deleteSkill(name: string): Promise<HarnessSkill[]> {
    const data = await this.request<RawSkill[]>('/skills/delete', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ name }),
    });
    return (data || []).map(normalizeSkill);
  }

  async listSkillTrash(): Promise<HarnessSkillTrash[]> {
    const data = await this.request<RawSkillTrash[]>('/skills/trash', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
    });
    return (data || []).map(normalizeSkillTrash);
  }

  async restoreSkill(name: string): Promise<HarnessSkill[]> {
    const data = await this.request<RawSkill[]>('/skills/restore', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ name }),
    });
    return (data || []).map(normalizeSkill);
  }

  async deleteSkillTrash(name: string): Promise<HarnessSkillTrash[]> {
    const data = await this.request<RawSkillTrash[]>('/skills/trash/delete', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ name }),
    });
    return (data || []).map(normalizeSkillTrash);
  }

  async setSkillEnabled(name: string, enabled: boolean): Promise<HarnessSkill[]> {
    const data = await this.request<RawSkill[]>('/skills/enable', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ name, enabled }),
    });
    return (data || []).map(normalizeSkill);
  }

  async listPromptSnippets(): Promise<PromptSnippet[]> {
    const data = await this.request<RawPromptSnippet[]>('/prompt-snippets');
    return (data || []).map(normalizePromptSnippet);
  }

  async savePromptSnippet(input: PromptSnippetInput): Promise<PromptSnippet[]> {
    const data = await this.request<RawPromptSnippet[]>('/prompt-snippets', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(input),
    });
    return (data || []).map(normalizePromptSnippet);
  }

  async deletePromptSnippet(name: string): Promise<PromptSnippet[]> {
    const data = await this.request<RawPromptSnippet[]>('/prompt-snippets/delete', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ name }),
    });
    return (data || []).map(normalizePromptSnippet);
  }

  async health(): Promise<HarnessHealth> {
    const data = await this.request<{ provider?: string; model?: string; context_window?: number; vision?: boolean; default_provider?: string }>('/health');
    return {
      provider: data.provider || "",
      model: data.model || "",
      contextWindow: Number(data.context_window || 0),
      vision: data.vision === true,
      defaultProvider: data.default_provider || "",
    };
  }

  async listSettings(): Promise<HarnessSetting[]> {
    const data = await this.request<{ settings?: RawSetting[] }>('/settings');
    return (data.settings || []).map(normalizeSetting);
  }

  async updateSettings(updates: Record<string, string | number | boolean>): Promise<HarnessSetting[]> {
    const data = await this.request<{ settings?: RawSetting[] }>('/settings', {
      method: "POST",
      body: JSON.stringify(updates),
    });
    return (data.settings || []).map(normalizeSetting);
  }

  async checkProvider(input: { endpoint: string; apiKey?: string }): Promise<ProviderCheckResult> {
    const data = await this.request<{
      ok?: boolean;
      models?: { id?: string; context_window?: number }[];
      error?: string;
    }>('/provider/check', {
      method: "POST",
      body: JSON.stringify({
        endpoint: input.endpoint,
        ...(input.apiKey ? { api_key: input.apiKey } : {}),
      }),
    });
    return {
      ok: data.ok === true,
      models: (data.models || []).map((model) => ({
        id: String(model.id || ""),
        contextWindow: Number(model.context_window || 0),
      })),
      ...(data.error ? { error: data.error } : {}),
    };
  }

  async createProfile(input: ProfileInput): Promise<HarnessProfile> {
    const data = await this.request<RawProfile>('/profiles', {
      method: "POST",
      body: JSON.stringify({
        id: input.id,
        name: input.name,
        description: input.description || "",
        identity_prompt: input.identityPrompt || "",
        base_model: input.baseModel || "",
        provider: input.provider || "",
        vision: input.vision ?? null,
        ...(input.temperature !== undefined ? { temperature: input.temperature } : {}),
        ...(input.promptPackId ? { prompt_pack_id: input.promptPackId } : {}),
        ...(input.promptPlan ? { prompt_plan: input.promptPlan } : {}),
        child_profile_allowlist: input.childProfileAllowlist || [],
      }),
    });
    return normalizeProfile(data);
  }

  async updateProfile(input: ProfileInput): Promise<HarnessProfile> {
    const data = await this.request<RawProfile>(
      `/profiles/${encodeURIComponent(input.id)}`,
      {
        method: "POST",
        body: JSON.stringify({
          name: input.name,
          description: input.description || "",
          identity_prompt: input.identityPrompt || "",
          base_model: input.baseModel || "",
          provider: input.provider || "",
          vision: input.vision,
          ...(input.temperature !== undefined ? { temperature: input.temperature } : {}),
          ...(input.promptPackId !== undefined ? { prompt_pack_id: input.promptPackId } : {}),
          ...(input.promptPlan !== undefined ? { prompt_plan: input.promptPlan } : {}),
          child_profile_allowlist: input.childProfileAllowlist || [],
        }),
      },
    );
    return normalizeProfile(data);
  }

  // ── Prompt packs ──────────────────────────────────────────────────────────

  private validationFrom(raw: Record<string, unknown>): PackValidation {
    const validation = raw.validation as PackValidation | null | undefined;
    return {
      valid: validation?.valid === true,
      errors: validation && Array.isArray(validation.errors) ? validation.errors : [],
      warnings: validation && Array.isArray(validation.warnings) ? validation.warnings : [],
    };
  }

  private normalizePack(raw: Record<string, unknown>, withFiles: boolean): PromptPackDetail {
    const summary: PromptPackSummary = {
      id: String(raw.id || ""),
      name: String(raw.name || ""),
      description: String(raw.description || ""),
      revision: Number(raw.revision || 0),
      validationStatus: String(raw.validation_status || "unchecked"),
      fingerprint: String(raw.fingerprint || ""),
      fileCount: Number(raw.file_count || 0),
      createdAt: String(raw.created_at || ""),
      updatedAt: String(raw.updated_at || ""),
    };
    const detail: PromptPackDetail = {
      ...summary,
      files: withFiles && Array.isArray(raw.files)
        ? (raw.files as Record<string, unknown>[]).map((file) => ({
            path: String(file.path || ""),
            content: String(file.content || ""),
            contentHash: String(file.content_hash || ""),
            revision: Number(file.revision || 0),
            createdAt: String(file.created_at || ""),
            updatedAt: String(file.updated_at || ""),
          }))
        : [],
      validation: this.validationFrom(raw),
    };
    return detail;
  }

  private normalizePackSnapshot(raw: RawPackSnapshot, withDocument: boolean): PackSnapshot {
    const summary: PackSnapshotSummary = {
      id: Number(raw.id || 0),
      packId: String(raw.pack_id || ""),
      revision: Number(raw.revision || 0),
      fingerprint: String(raw.fingerprint || ""),
      createdAt: String(raw.created_at || ""),
    };
    if (!withDocument) return { ...summary, document: "" };
    return { ...summary, document: String(raw.document || "") };
  }

  private normalizePreview(raw: Record<string, unknown>): PromptPackPreview {
    return {
      ok: raw.ok === true,
      ...(typeof raw.error === "string" && raw.error ? { error: raw.error } : {}),
      pack: this.normalizePack((raw.pack || {}) as Record<string, unknown>, false),
      validation: this.validationFrom(raw),
      modules: Array.isArray(raw.modules)
        ? (raw.modules as Record<string, unknown>[]).map((module) => ({
            id: String(module.id || ""),
            path: String(module.path || ""),
            category: String(module.category || ""),
            bytes: Number(module.bytes || 0),
            approxTokens: Number(module.approx_tokens || 0),
            required: module.required === true,
          }))
        : [],
      daemonBlocks: Array.isArray(raw.daemon_blocks)
        ? (raw.daemon_blocks as Record<string, unknown>[]).map((block) => ({
            name: String(block.name || ""),
            bytes: Number(block.bytes || 0),
          }))
        : [],
      dynamicBlocks: Array.isArray(raw.dynamic_blocks) ? raw.dynamic_blocks.map(String) : [],
      prompt: String(raw.prompt || ""),
      totalBytes: Number(raw.total_bytes || 0),
      approxTokens: Number(raw.approx_tokens || 0),
      fingerprint: String(raw.fingerprint || ""),
    };
  }

  private packSummaryFrom(raw: Record<string, unknown>): PromptPackSummary {
    return this.normalizePack(raw, false);
  }

  async listPromptPacks(): Promise<PromptPackSummary[]> {
    const data = await this.request<{ packs?: Record<string, unknown>[] }>('/prompt-packs');
    return (data.packs || []).map((raw) => this.packSummaryFrom(raw));
  }

  async createPromptPack(input: {
    id: string;
    name: string;
    description?: string;
    files?: { path: string; content: string }[];
  }): Promise<PromptPackDetail> {
    const data = await this.request<Record<string, unknown>>('/prompt-packs', {
      method: "POST",
      body: JSON.stringify({
        id: input.id,
        name: input.name,
        description: input.description || "",
        ...(input.files ? { files: input.files } : {}),
      }),
    });
    return this.normalizePack(data, false);
  }

  async getPromptPack(id: string): Promise<PromptPackDetail> {
    const data = await this.request<Record<string, unknown>>(`/prompt-packs/${encodeURIComponent(id)}`);
    return this.normalizePack(data, true);
  }

  async updatePromptPack(id: string, name: string, description: string): Promise<PromptPackSummary> {
    const data = await this.request<Record<string, unknown>>(
      `/prompt-packs/${encodeURIComponent(id)}`,
      { method: "POST", body: JSON.stringify({ name, description }) },
    );
    return this.normalizePack(data, false);
  }

  async upsertPackFile(packId: string, path: string, content: string): Promise<PackValidation> {
    const data = await this.request<Record<string, unknown>>(
      `/prompt-packs/${encodeURIComponent(packId)}/files`,
      { method: "POST", body: JSON.stringify({ path, content }) },
    );
    return this.validationFrom(data);
  }

  async listPackSnapshots(packId: string): Promise<PackSnapshotSummary[]> {
    const data = await this.request<{ snapshots?: RawPackSnapshot[] }>(
      `/prompt-packs/${encodeURIComponent(packId)}/snapshots`,
    );
    return (data.snapshots || []).map((raw) => this.normalizePackSnapshot(raw, false));
  }

  async getPackSnapshot(id: number): Promise<PackSnapshot> {
    const data = await this.request<RawPackSnapshot>(
      `/prompt-packs/snapshots/${id}`,
    );
    return this.normalizePackSnapshot(data, true);
  }

  async deletePackFile(packId: string, path: string): Promise<PackValidation> {
    const data = await this.request<Record<string, unknown>>(
      `/prompt-packs/${encodeURIComponent(packId)}/files/delete`,
      { method: "POST", body: JSON.stringify({ path }) },
    );
    return this.validationFrom(data);
  }

  async deletePromptPack(id: string): Promise<void> {
    await this.request(`/prompt-packs/${encodeURIComponent(id)}/delete`, { method: "POST", body: "{}" });
  }

  async validatePromptPack(id: string): Promise<PackValidation> {
    const data = await this.request<Record<string, unknown>>(
      `/prompt-packs/${encodeURIComponent(id)}/validate`,
      { method: "POST", body: "{}" },
    );
    return this.validationFrom(data);
  }

  async previewPromptPack(packId: string, plan: string[], profileId?: string): Promise<PromptPackPreview> {
    const data = await this.request<Record<string, unknown>>('/prompt-packs/preview', {
      method: "POST",
      body: JSON.stringify({
        pack_id: packId,
        plan,
        ...(profileId ? { profile_id: profileId } : {}),
      }),
    });
    return this.normalizePreview(data);
  }

  async renameProfile(oldId: string, newId: string): Promise<void> {
    await this.request(`/profiles/${encodeURIComponent(oldId)}/rename`, {
      method: "POST",
      body: JSON.stringify({ id: newId }),
    });
  }

  async deleteProfile(id: string): Promise<void> {
    await this.request(`/profiles/${encodeURIComponent(id)}/delete`, {
      method: "POST",
      body: "{}",
    });
  }

  async selectProfile(sessionId: string, profileId: string): Promise<void> {
    await this.request(`/sessions/${encodeURIComponent(sessionId)}/profile`, {
      method: "POST",
      body: JSON.stringify({ profile: profileId }),
    });
  }

  async listProviders(): Promise<HarnessProvider[]> {
    const data = await this.request<{ providers: RawProvider[] }>('/providers');
    return (data.providers || []).map(normalizeProvider);
  }

  async upsertProvider(input: ProviderInput): Promise<HarnessProvider> {
    const data = await this.request<RawProvider>('/providers', {
      method: "POST",
      body: JSON.stringify({
        name: input.name,
        endpoint: input.endpoint,
        model: input.model || "",
        ...(input.apiKey ? { api_key: input.apiKey } : {}),
        params: input.params || {},
      }),
    });
    return normalizeProvider(data);
  }

  async deleteProvider(name: string): Promise<void> {
    await this.request(`/providers/${encodeURIComponent(name)}/delete`, {
      method: "POST",
      body: "{}",
    });
  }

  async getInfra(): Promise<InfraReport> {
    const data = await this.request<{
      host?: Record<string, unknown>;
      gpu?: {
        available?: boolean;
        vendor?: string;
        devices?: Record<string, unknown>[];
      };
      docker_available?: boolean;
      containers?: Record<string, unknown>[];
      providers?: Record<string, unknown>[];
      collected_at_ms?: number;
    }>('/infra');
    const rawHost = data.host || {};
    const num = (value: unknown, fallback = 0): number =>
      typeof value === "number" && Number.isFinite(value) ? value : fallback;
    return {
      host: {
        available: Boolean(rawHost.available),
        memTotalMb: num(rawHost.mem_total_mb),
        memAvailableMb: num(rawHost.mem_available_mb),
        swapTotalMb: num(rawHost.swap_total_mb),
        swapFreeMb: num(rawHost.swap_free_mb),
        load1: num(rawHost.load1),
        load5: num(rawHost.load5),
        load15: num(rawHost.load15),
        cores: num(rawHost.cores),
      },
      gpu: {
        available: Boolean(data.gpu?.available),
        vendor: String(data.gpu?.vendor ?? ""),
        devices: (data.gpu?.devices || []).map((raw) => ({
          name: String(raw.name ?? ""),
          vramTotalMb: num(raw.vram_total_mb),
          vramUsedMb: num(raw.vram_used_mb),
          utilPct: num(raw.util_pct),
        })),
      },
      dockerAvailable: Boolean(data.docker_available),
      containers: (data.containers || []).map((raw) => ({
        name: String(raw.name ?? ""),
        state: String(raw.state ?? ""),
        health: String(raw.health ?? ""),
        restarts: num(raw.restarts),
        memMb: num(raw.mem_mb, -1),
      })),
      providers: (data.providers || []).map((raw) => ({
        name: String(raw.name ?? ""),
        endpoint: String(raw.endpoint ?? ""),
        online: Boolean(raw.online),
        latencyMs: num(raw.latency_ms),
      })),
      collectedAtMs: num(data.collected_at_ms),
    };
  }

  async checkProviderRoute(name: string): Promise<ProviderCheckResult> {
    const data = await this.request<{
      ok?: boolean;
      models?: Array<{ id?: string; context_window?: number }>;
      error?: string;
    }>(`/providers/${encodeURIComponent(name)}/check`, {
      method: "POST",
      body: "{}",
    });
    return {
      ok: Boolean(data.ok),
      models: (data.models || []).map((model) => ({
        id: String(model.id || ""),
        contextWindow: Number(model.context_window || 0),
      })),
      ...(data.error ? { error: data.error } : {}),
    };
  }

  async deleteSession(sessionId: string): Promise<void> {
    await this.request(`/sessions/${encodeURIComponent(sessionId)}/delete`, {
      method: "POST",
      body: "{}",
    });
  }

  async renameSession(sessionId: string, title: string): Promise<void> {
    await this.request(`/sessions/${encodeURIComponent(sessionId)}/rename`, {
      method: "POST",
      body: JSON.stringify({ title }),
    });
  }

  async setSessionTags(sessionId: string, tags: string[]): Promise<void> {
    await this.request(`/sessions/${encodeURIComponent(sessionId)}/tags`, {
      method: "POST",
      body: JSON.stringify({ tags }),
    });
  }

  async retitleSession(sessionId: string): Promise<void> {
    await this.request(`/sessions/${encodeURIComponent(sessionId)}/retitle`, {
      method: "POST",
      body: "{}",
    });
  }

  async getJourney(sessionId: string): Promise<JourneyInfo | null> {
    // Direct fetch: a 404 is a normal answer (no journey yet), not an error.
    const response = await fetch(
      `${this.baseUrl}/memory/journey?session_id=${encodeURIComponent(sessionId)}`,
      { credentials: "include" },
    );
    if (response.status === 404) return null;
    if (!response.ok) {
      let detail = "";
      try {
        const data = (await response.json()) as { error?: unknown };
        if (data && typeof data.error === "string" && data.error) detail = `: ${data.error}`;
      } catch {
        // non-JSON error body; the status code is enough
      }
      throw new Error(`harness request failed: ${response.status}${detail}`);
    }
    const raw = (await response.json()) as {
      session_id?: string;
      profile?: string;
      text?: string;
      watermark?: number;
      updated_at?: string;
    };
    return {
      sessionId: raw.session_id || sessionId,
      profile: raw.profile || "",
      text: raw.text || "",
      watermark: Number(raw.watermark || 0),
      updatedAt: raw.updated_at || "",
    };
  }

  async requestJourneyUpdate(sessionId: string): Promise<void> {
    await this.request("/memory/journey", {
      method: "POST",
      body: JSON.stringify({ session_id: sessionId }),
    });
  }

  async linkVault(sessionId: string, path: string): Promise<void> {
    await this.request(`/sessions/${encodeURIComponent(sessionId)}/md-log`, {
      method: "POST",
      body: JSON.stringify({ path }),
    });
  }

  async unlinkVault(sessionId: string): Promise<void> {
    await this.request(`/sessions/${encodeURIComponent(sessionId)}/md-log`, {
      method: "POST",
      body: JSON.stringify({ path: "" }),
    });
  }

  async listFs(path: string): Promise<FsListing> {
    return this.request<FsListing>(`/fs?path=${encodeURIComponent(path)}`, { method: "GET" });
  }

  async rewind(input: { sessionId: string; messageId: number; trim?: boolean }): Promise<void> {
    await this.request(`/sessions/${encodeURIComponent(input.sessionId)}/rewind`, {
      method: "POST",
      body: JSON.stringify({
        message_id: input.messageId,
        ...(input.trim ? { trim: true } : {}),
      }),
    });
  }

  async deleteMessage(input: { sessionId: string; messageId: number }): Promise<void> {
    await this.request(
      `/sessions/${encodeURIComponent(input.sessionId)}/messages/${input.messageId}/delete`,
      { method: "POST", body: "{}" },
    );
  }

  async editMessage(input: {
    sessionId: string;
    messageId: number;
    content: string;
  }): Promise<{ runId: string; newMessageId?: number }> {
    const data = await this.request<{ run_id?: string; message_id?: number }>(
      `/sessions/${encodeURIComponent(input.sessionId)}/messages/${input.messageId}`,
      {
        method: "POST",
        body: JSON.stringify({ content: input.content }),
      },
    );
    if (!data.run_id) throw new Error("harness did not return a run id");
    return { runId: data.run_id, newMessageId: data.message_id ? Number(data.message_id) : undefined };
  }

  async selectMessage(input: { sessionId: string; messageId: number }): Promise<void> {
    await this.request(`/sessions/${encodeURIComponent(input.sessionId)}/select`, {
      method: "POST",
      body: JSON.stringify({ message_id: input.messageId }),
    });
  }

  async forkSession(input: { sourceId: string; path: number[] | "active"; mode?: ForkMode }): Promise<HarnessSession> {
    const data = await this.request<RawSession>('/sessions', {
      method: "POST",
      body: JSON.stringify({
        fork_from: input.sourceId,
        path: input.path,
        ...(input.mode ? { mode: input.mode } : {}),
      }),
    });
    return normalizeSession(data);
  }

  async loadSession(sessionId: string): Promise<HarnessSnapshot> {
    const data = await this.request<{
      id: string;
      title?: string;
      profile?: string;
      model?: string;
      status?: string;
      updated_at?: string;
      message_count?: number;
      context_mode?: string;
      context_status?: string;
      context_error?: string;
      context_summary?: string;
      source_session_id?: string;
      speakers?: string[];
      messages?: { id: number; parent_id: number; role: string; content: string; reasoning?: string; run_id?: string; is_active?: boolean; speaker?: string; client_id?: number; attachments?: RawAttachment[] }[];
      events?: Record<string, unknown>[];
    }>(`/sessions/${encodeURIComponent(sessionId)}`);
    const session = normalizeSession({
      ...data,
      message_count: data.messages?.length || 0,
    });
    const messages: HarnessMessage[] = (data.messages || []).map((m, index) => {
      const attachments = (m.attachments || []).map(normalizeAttachment);
      return {
        id: Number(m.id ?? index),
        parentId: Number(m.parent_id ?? 0),
        role:
          m.role === "assistant" ? "assistant" : m.role === "user" ? "user" : "system",
        content: m.content,
        reasoning: m.reasoning,
        runId: m.run_id,
        isActive: m.is_active,
        speaker: m.speaker || undefined,
        clientId: m.client_id || undefined,
        attachments: attachments.length ? attachments : undefined,
        createdAt: nowIso(),
      };
    });
    const allEvents = (data.events || []).map((raw) =>
      normalizeEvent(raw as Record<string, unknown>, sessionId),
    );
    let cursor = this.cursors.get(sessionId) || 0;
    for (const event of allEvents) {
      if (event.id && event.id > cursor) cursor = event.id;
    }
    this.cursors.set(sessionId, cursor);
    // Rebuild the active run from the full ledger, but only retain the same
    // bounded tail used by live SSE rendering. Older daemons can leave tens of
    // thousands of delta rows in a session snapshot.
    return { session, messages, run: runFromEvents(allEvents, sessionId), events: compactEventWindow(allEvents), artifacts: [] };
  }

  async sendMessage(input: {
    sessionId: string;
    message: string;
    profile: string;
    attachments?: HarnessAttachment[];
    mentions?: string[];
    messageId?: number;
  }): Promise<{ runId: string; messageId?: number }> {
    const payload: Record<string, unknown> = {
      session_id: input.sessionId,
      message: input.message,
    };
    // A-7: the optimistic user row's client-generated id travels with the
    // send; the daemon anchors it on the stored row (client_id), so
    // snapshots and SSE replay dedupe by it even when the run id mapping is
    // lost. The daemon's own (snowflake) row id comes back as message_id.
    if (input.messageId) payload.message_id = input.messageId;
    if (input.attachments?.length) payload.attachments = input.attachments.map(serializeAttachment);
    if (input.mentions?.length) payload.mentions = input.mentions;
    const data = await this.request<{ run_id?: string; runId?: string; message_id?: number }>(
      `/sessions/${encodeURIComponent(input.sessionId)}/runs`,
      {
        method: "POST",
        body: JSON.stringify(payload),
      },
    );
    const runId = data.run_id || data.runId;
    if (!runId) throw new Error("harness did not return a run id");
    return { runId, messageId: data.message_id }
  }

  async selectIteration(input: { sessionId: string; runId: string }): Promise<void> {
    await this.request(`/sessions/${encodeURIComponent(input.sessionId)}/select`, {
      method: "POST",
      body: JSON.stringify({ run_id: input.runId }),
    });
  }

  async searchSession(sessionId: string, query: string): Promise<SearchResponse> {
    const data = await this.request<{
      scope?: string;
      session_id?: string;
      query?: string;
      matches?: Array<Record<string, unknown>>;
    }>(`/sessions/${encodeURIComponent(sessionId)}/search?q=${encodeURIComponent(query)}`);
    return {
      scope: (data.scope as "source" | "self") || "self",
      sessionId: String(data.session_id || sessionId),
      query: String(data.query || query),
      matches: (data.matches || []).map((match) => ({
        messageId: Number(match.message_id),
        sessionId: String(match.session_id || ""),
        role: String(match.role || ""),
        createdAt: String(match.created_at || ""),
        snippet: String(match.snippet || ""),
      })),
    };
  }

  async searchAll(query: string): Promise<GlobalSearchResponse> {
    const data = await this.request<{
      query?: string;
      matches?: Array<Record<string, unknown>>;
      truncated?: boolean;
    }>(`/search?q=${encodeURIComponent(query)}`);
    return {
      query: String(data.query || query),
      truncated: Boolean(data.truncated),
      matches: (data.matches || []).map((match) => ({
        messageId: Number(match.message_id),
        sessionId: String(match.session_id || ""),
        sessionTitle: String(match.session_title || ""),
        role: String(match.role || ""),
        createdAt: String(match.created_at || ""),
        snippet: String(match.snippet || ""),
      })),
    };
  }

  async memoryStatus(): Promise<MemoryStatus> {
    const data = await this.request<{
      enabled?: boolean;
      endpoint?: string;
      bank?: string;
      healthy?: boolean;
      provider?: string;
      error?: string;
      jobs?: Record<string, number>;
      custom?: {
        ready?: boolean;
        reranker_loaded?: boolean;
        embed_model?: string;
        rerank_model?: string;
        units?: Record<string, number>;
        last_sweep?: {
          trigger?: string;
          watermark?: string;
          selected?: number;
          changes?: number;
          fixpoint?: boolean;
          finished_at?: string;
        };
      };
    }>('/memory/status');
    return {
      enabled: data.enabled === true,
      provider: data.provider ? String(data.provider) : undefined,
      endpoint: String(data.endpoint || ""),
      bank: String(data.bank || ""),
      healthy: data.healthy === true,
      ...(data.error ? { error: String(data.error) } : {}),
      jobs: data.jobs || {},
      ...(data.custom ? {
        custom: {
          ready: data.custom.ready === true,
          reranker_loaded: data.custom.reranker_loaded === true,
          ...(data.custom.embed_model ? { embed_model: String(data.custom.embed_model) } : {}),
          ...(data.custom.rerank_model ? { rerank_model: String(data.custom.rerank_model) } : {}),
          ...(data.custom.units ? { units: data.custom.units } : {}),
          ...(data.custom.last_sweep
            ? {
                last_sweep: {
                  trigger: String(data.custom.last_sweep.trigger || ""),
                  ...(data.custom.last_sweep.watermark
                    ? { watermark: String(data.custom.last_sweep.watermark) }
                    : {}),
                  selected: Number(data.custom.last_sweep.selected || 0),
                  changes: Number(data.custom.last_sweep.changes || 0),
                  fixpoint: data.custom.last_sweep.fixpoint === true,
                  ...(data.custom.last_sweep.finished_at
                    ? { finished_at: String(data.custom.last_sweep.finished_at) }
                    : {}),
                } as MemorySweepInfo,
              }
            : {}),
        },
      } : {}),
    };
  }

  async consolidateMemory(): Promise<{ ok: boolean; job_id: number }> {
    const data = await this.request<{ ok?: boolean; job_id?: number }>(
      '/memory/consolidate',
      { method: 'POST' }
    );
    return { ok: data.ok === true, job_id: Number(data.job_id || 0) };
  }

  private mapConsolidation(raw: Record<string, unknown>): MemoryConsolidation {
    return {
      id: Number(raw.id || 0),
      older_unit_id: Number(raw.older_unit_id || 0),
      newer_unit_id: Number(raw.newer_unit_id || 0),
      cosine: Number(raw.cosine || 0),
      scope: String(raw.scope || ""),
      status: (raw.status as MemoryConsolidationStatus) || "pending",
      ...(raw.kind ? { kind: raw.kind as MemoryConsolidationKind } : {}),
      ...(raw.created_at ? { created_at: String(raw.created_at) } : {}),
      ...(raw.decided_at ? { decided_at: String(raw.decided_at) } : {}),
      ...(raw.older_text ? { older_text: String(raw.older_text) } : {}),
      ...(raw.newer_text ? { newer_text: String(raw.newer_text) } : {}),
    };
  }

  async listConsolidations(status?: string): Promise<MemoryConsolidation[]> {
    const query = status ? `?status=${encodeURIComponent(status)}` : "";
    const data = await this.request<{ consolidations?: Array<Record<string, unknown>> }>(
      `/memory/consolidations${query}`,
    );
    return (data.consolidations || []).map((raw) => this.mapConsolidation(raw));
  }

  private decideConsolidation(id: number, action: string): Promise<MemoryConsolidation> {
    return this.request<{ consolidation?: Record<string, unknown> }>(
      `/memory/consolidations/${id}/${action}`,
      { method: "POST", body: "{}" },
    ).then((data) => {
      if (!data.consolidation) throw new Error("harness did not return the consolidation");
      return this.mapConsolidation(data.consolidation);
    });
  }

  async approveConsolidation(id: number): Promise<MemoryConsolidation> {
    return this.decideConsolidation(id, "approve");
  }

  async rejectConsolidation(id: number): Promise<MemoryConsolidation> {
    return this.decideConsolidation(id, "reject");
  }

  async memoryRecall(input: { query: string; session?: string; profile?: string }): Promise<MemoryRecallResponse> {
    const data = await this.request<{
      source?: string;
      fallback?: boolean;
      query?: string;
      results?: Array<Record<string, unknown>>;
    }>('/memory/recall', {
      method: "POST",
      body: JSON.stringify({
        query: input.query,
        ...(input.session ? { session: input.session } : {}),
        ...(input.profile ? { profile: input.profile } : {}),
      }),
    });
    return {
      source: String(data.source || ""),
      ...(data.fallback === true ? { fallback: true } : {}),
      query: String(data.query || input.query),
      results: (data.results || []).map((item) => ({
        id: String(item.id || ""),
        text: String(item.text || ""),
        type: String(item.type || ""),
        ...(item.document_id ? { document_id: String(item.document_id) } : {}),
        ...(Array.isArray(item.tags) ? { tags: item.tags.map((tag) => String(tag)) } : {}),
        ...(item.session_id ? { session_id: String(item.session_id) } : {}),
        ...(item.role ? { role: String(item.role) } : {}),
        ...(item.created_at ? { created_at: String(item.created_at) } : {}),
      })),
    };
  }

  private mapRecord(raw: Record<string, unknown>): MemoryRecord {
    return {
      id: Number(raw.id || 0),
      status: (raw.status as MemoryRecordStatus) || "candidate",
      type: String(raw.type || "fact"),
      text: String(raw.text || ""),
      scope: String(raw.scope || "session"),
      ...(raw.session_id ? { session_id: String(raw.session_id) } : {}),
      ...(raw.profile ? { profile: String(raw.profile) } : {}),
      ...(raw.run_id ? { run_id: String(raw.run_id) } : {}),
      ...(raw.source_message_ids ? { source_message_ids: String(raw.source_message_ids) } : {}),
      ...(raw.provenance ? { provenance: String(raw.provenance) } : {}),
      ...(raw.contradicts_id ? { contradicts_id: Number(raw.contradicts_id) } : {}),
      ...(raw.superseded_by_id ? { superseded_by_id: Number(raw.superseded_by_id) } : {}),
      ...(raw.created_at ? { created_at: String(raw.created_at) } : {}),
      ...(raw.updated_at ? { updated_at: String(raw.updated_at) } : {}),
      ...(raw.decided_at ? { decided_at: String(raw.decided_at) } : {}),
    };
  }

  async memoryBank(input?: {
    text?: string;
    type?: string;
    scope?: string;
    state?: string;
    limit?: number;
    offset?: number;
  }): Promise<MemoryBankResponse> {
    const params = new URLSearchParams();
    if (input?.text) params.set("text", input.text);
    if (input?.type) params.set("type", input.type);
    if (input?.scope) params.set("scope", input.scope);
    if (input?.state) params.set("state", input.state);
    params.set("limit", String(input?.limit ?? 50));
    params.set("offset", String(input?.offset ?? 0));
    const data = await this.request<{ total?: number; units?: Array<Record<string, unknown>> }>(
      `/memory/bank?${params.toString()}`,
    );
    return {
      total: Number(data.total || 0),
      units: (data.units || []).map((raw) => this.mapUnit(raw)),
    };
  }

  private unitUrl(id: string): string {
    const numeric = id.startsWith("mem:") ? id.slice("mem:".length) : id;
    return `/memory/units/${encodeURIComponent(numeric)}`;
  }

  private mapUnit(raw: Record<string, unknown>): MemoryBankUnit {
    return {
      id: String(raw.id || ""),
      type: String(raw.type || ""),
      state: String(raw.state || "active"),
      ...(raw.state_reason ? { state_reason: String(raw.state_reason) } : {}),
      scope: String(raw.scope || "global"),
      ...(raw.session_id ? { session_id: String(raw.session_id) } : {}),
      ...(raw.profile ? { profile: String(raw.profile) } : {}),
      ...(raw.run_id ? { run_id: String(raw.run_id) } : {}),
      text: String(raw.text || ""),
      ...(raw.store_reason ? { store_reason: String(raw.store_reason) } : {}),
      ...(raw.model_id ? { model_id: String(raw.model_id) } : {}),
      ...(typeof raw.confidence === "number" ? { confidence: raw.confidence } : {}),
      ...(raw.mentioned_at ? { mentioned_at: String(raw.mentioned_at) } : {}),
      ...(raw.created_at ? { created_at: String(raw.created_at) } : {}),
      ...(raw.updated_at ? { updated_at: String(raw.updated_at) } : {}),
    };
  }

  async getMemoryUnit(id: string): Promise<MemoryUnitDetail> {
    const data = await this.request<Record<string, unknown>>(this.unitUrl(id));
    const unit = this.mapUnit((data.unit || {}) as Record<string, unknown>);
    const versions: MemoryUnitVersion[] = ((data.versions || []) as Array<
      Record<string, unknown>
    >).map((raw) => ({
      id: String(raw.id || ""),
      text: String(raw.text || ""),
      ...(raw.model_id ? { model_id: String(raw.model_id) } : {}),
      ...(raw.store_reason ? { store_reason: String(raw.store_reason) } : {}),
      ...(raw.created_at ? { created_at: String(raw.created_at) } : {}),
      current: Boolean(raw.current),
    }));
    const links: MemoryUnitLink[] = ((data.links || []) as Array<
      Record<string, unknown>
    >).map((raw) => ({
      target_id: Number(raw.target_id || 0),
      link_type: String(raw.link_type || ""),
    }));
    const rawFeedback = (data.feedback || {}) as Record<string, unknown>;
    const feedback: MemoryUnitFeedback = {
      offered: Number(rawFeedback.offered || 0),
      cited: Number(rawFeedback.cited || 0),
      used: Number(rawFeedback.used || 0),
      acted_on: Number(rawFeedback.acted_on || 0),
      ...(rawFeedback.last_offered_at
        ? { last_offered_at: String(rawFeedback.last_offered_at) }
        : {}),
      ...(rawFeedback.last_cited_at
        ? { last_cited_at: String(rawFeedback.last_cited_at) }
        : {}),
      ...(rawFeedback.last_used_at
        ? { last_used_at: String(rawFeedback.last_used_at) }
        : {}),
      ...(rawFeedback.last_acted_on_at
        ? { last_acted_on_at: String(rawFeedback.last_acted_on_at) }
        : {}),
    };
    return { unit, versions, links, feedback };
  }

  async invalidateMemoryUnit(id: string, reason?: string): Promise<void> {
    await this.request(this.unitUrl(id) + "/invalidate", {
      method: "POST",
      body: JSON.stringify(reason ? { reason } : {}),
    });
  }

  async reactivateMemoryUnit(id: string, reason?: string): Promise<void> {
    await this.request(this.unitUrl(id) + "/reactivate", {
      method: "POST",
      body: JSON.stringify(reason ? { reason } : {}),
    });
  }

  async setMemoryUnitCore(id: string, on: boolean): Promise<void> {
    await this.request(this.unitUrl(id) + (on ? "/core" : "/uncore"), {
      method: "POST",
    });
  }

  async supersedeMemoryUnit(id: string, text: string, reason?: string): Promise<void> {
    await this.request(this.unitUrl(id) + "/supersede", {
      method: "POST",
      body: JSON.stringify(reason ? { text, reason } : { text }),
    });
  }

  async deleteMemoryUnit(id: string): Promise<void> {
    await this.request(this.unitUrl(id) + "/delete", {
      method: "POST",
      body: "{}",
    });
  }

  async listMemoryRecords(status?: string): Promise<MemoryRecord[]> {
    const query = status ? `?status=${encodeURIComponent(status)}` : "";
    const data = await this.request<{ records?: Array<Record<string, unknown>> }>(
      `/memory/records${query}`,
    );
    return (data.records || []).map((raw) => this.mapRecord(raw));
  }

  async createMemoryRecord(input: MemoryRecordInput): Promise<MemoryRecord> {
    const data = await this.request<{ record?: Record<string, unknown> }>('/memory/records', {
      method: "POST",
      body: JSON.stringify(input),
    });
    if (!data.record) throw new Error("harness did not return a memory record");
    return this.mapRecord(data.record);
  }

  private decideRecord(id: number, action: string): Promise<MemoryRecord> {
    return this.request<{ record?: Record<string, unknown> }>(
      `/memory/records/${id}/${action}`,
      { method: "POST", body: "{}" },
    ).then((data) => {
      if (!data.record) throw new Error("harness did not return a memory record");
      return this.mapRecord(data.record);
    });
  }

  async approveMemoryRecord(id: number): Promise<MemoryRecord> {
    return this.decideRecord(id, "approve");
  }

  async rejectMemoryRecord(id: number): Promise<MemoryRecord> {
    return this.decideRecord(id, "reject");
  }

  async supersedeMemoryRecord(id: number): Promise<MemoryRecord> {
    return this.decideRecord(id, "supersede");
  }

  // Durable command plane (docs/command-plane.md): every steer/cancel
  // carries a client-generated id + issue time + TTL, so a retry or
  // double-click re-submits the same entry and the daemon answers
  // deterministically (skip / expired) instead of applying it twice.
  private commandStamp(): { commandId: string; issuedAtMs: number } {
    const commandId =
      typeof crypto !== "undefined" && "randomUUID" in crypto
        ? crypto.randomUUID()
        : `cmd-${Date.now()}-${Math.random().toString(36).slice(2)}`;
    return { commandId, issuedAtMs: Date.now() };
  }

  private static readonly COMMAND_TTL_MS = 120_000;

  private async sendRunCommand(
    runId: string,
    payload: Record<string, unknown>,
  ): Promise<void> {
    const response = await fetch(
      `${this.baseUrl}/runs/${encodeURIComponent(runId)}/commands`,
      {
        method: "POST",
        credentials: "include",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      },
    );
    // 410 Gone = the entry expired (past its TTL): a deterministic
    // non-execution, not a failure - the UI's state has moved on.
    if (!response.ok && response.status !== 410) {
      let detail = "";
      try {
        const data = (await response.json()) as { error?: unknown };
        if (data && typeof data.error === "string" && data.error) detail = `: ${data.error}`;
      } catch {
        // non-JSON body; the status code is enough
      }
      throw new Error(`harness request failed: ${response.status}${detail}`);
    }
  }

  async cancelRun(runId: string): Promise<void> {
    const { commandId, issuedAtMs } = this.commandStamp();
    await this.sendRunCommand(runId, {
      command: "cancel",
      command_id: commandId,
      issued_at_ms: issuedAtMs,
      ttl_ms: HttpHarnessClient.COMMAND_TTL_MS,
    });
  }

  async steerRun(runId: string): Promise<void> {
    const { commandId, issuedAtMs } = this.commandStamp();
    await this.sendRunCommand(runId, {
      command: "steer",
      command_id: commandId,
      issued_at_ms: issuedAtMs,
      ttl_ms: HttpHarnessClient.COMMAND_TTL_MS,
    });
  }

  // SESSION-scoped steer / deliver-now / cancel (docs/queue-semantics.md):
  // the daemon resolves the run that holds the session slot, so a stale
  // client-side tracked-run id (failed turn, reload mid-drain) can no
  // longer no-op or mis-target the steer buttons.
  // Confirmed delivery ids (A-7): the daemon resolves the run holding the
  // session slot at action time and echoes it in the ack, so the caller
  // can confirm the action against the run that actually received it
  // (a stale client-side tracked-run id is exactly what these
  // session-scoped routes exist to bypass).
  async steerSession(sessionId: string): Promise<CommandAck> {
    return this.request<CommandAck>(
      `/sessions/${encodeURIComponent(sessionId)}/commands`,
      {
        method: "POST",
        body: JSON.stringify({ command: "steer" }),
      },
    );
  }

  async deliverSession(
    sessionId: string,
    queuedRunId: string,
  ): Promise<CommandAck> {
    return this.request<CommandAck>(
      `/sessions/${encodeURIComponent(sessionId)}/commands`,
      {
        method: "POST",
        body: JSON.stringify({
          command: "deliver_now",
          queued_run_id: queuedRunId,
        }),
      },
    );
  }

  async cancelSession(sessionId: string): Promise<CommandAck> {
    return this.request<CommandAck>(
      `/sessions/${encodeURIComponent(sessionId)}/commands`,
      {
        method: "POST",
        body: JSON.stringify({ command: "cancel" }),
      },
    );
  }

  async promoteQueuedRun(input: {
    liveRunId: string;
    queuedRunId: string;
  }): Promise<void> {
    await this.sendRunCommand(input.liveRunId, {
      command: "steer_live",
      queued_run_id: input.queuedRunId,
    });
  }

  async regenerate(input: {
    sessionId: string;
    runId: string;
  }): Promise<{ runId: string }> {
    const data = await this.request<{ run_id?: string; runId?: string }>(
      `/runs/${encodeURIComponent(input.runId)}/commands`,
      {
        method: "POST",
        body: JSON.stringify({ command: "regenerate", session_id: input.sessionId }),
      },
    );
    const runId = data.run_id || data.runId;
    if (!runId) throw new Error("harness did not return a run id");
    return { runId };
  }

  async continueRun(input: {
    sessionId: string;
    runId: string;
  }): Promise<{ runId: string }> {
    const data = await this.request<{ run_id?: string; runId?: string }>(
      `/runs/${encodeURIComponent(input.runId)}/commands`,
      {
        method: "POST",
        body: JSON.stringify({ command: "continue", session_id: input.sessionId }),
      },
    );
    const runId = data.run_id || data.runId;
    if (!runId) throw new Error("harness did not return a run id");
    return { runId };
  }

  async respondApproval(
    runId: string,
    approvalId: string,
    decision: "allow_once" | "allow_turn" | "allow_session" | "allow_always" | "deny",
  ): Promise<void> {
    await this.request(`/runs/${encodeURIComponent(runId)}/commands`, {
      method: "POST",
      body: JSON.stringify({
        command: decision === "deny" ? "deny" : "approve",
        approval_id: approvalId,
        scope: decision === "allow_session" ? "session"
          : decision === "allow_always" ? "always"
          : decision === "allow_turn" ? "turn" : "once",
      }),
    });
  }

  async listAlwaysAllowed(): Promise<ApprovalRule[]> {
    // Older daemons return bare tool strings; normalize both shapes.
    const data = await this.request<{ always_allowed?: (string | { tool?: string; fingerprint?: string })[] }>("/approvals");
    return (data.always_allowed || []).map((entry) =>
      typeof entry === "string"
        ? { tool: entry, fingerprint: "whole tool" }
        : {
            tool: String(entry.tool || ""),
            fingerprint: entry.fingerprint ? String(entry.fingerprint) : "whole tool",
          },
    );
  }

  async removeAlwaysAllowed(tool: string): Promise<void> {
    await this.request("/approvals/always/remove", {
      method: "POST",
      body: JSON.stringify({ tool }),
    });
  }

  // Answer a pending interactive tool (quiz / ask_user_question). `choice`
  // is the presented option index, or -1 for the quiz's "don't know".
  async answerInput(runId: string, inputId: string, choice: number): Promise<void> {
    await this.request(`/runs/${encodeURIComponent(runId)}/commands`, {
      method: "POST",
      body: JSON.stringify({ command: "answer", input_id: inputId, choice: String(choice) }),
    });
  }

  // Answer the sudo password gate: password + scope (once / ttl / session).
  async answerSudo(runId: string, inputId: string, password: string, scope: string): Promise<void> {
    await this.request(`/runs/${encodeURIComponent(runId)}/commands`, {
      method: "POST",
      body: JSON.stringify({ command: "sudo", input_id: inputId, password, scope }),
    });
  }

  async respondContextDecision(
    sessionId: string,
    decision: "primary" | "deterministic" | "retry",
  ): Promise<void> {
    await this.request(`/sessions/${encodeURIComponent(sessionId)}/context/decision`, {
      method: "POST",
      body: JSON.stringify({ decision }),
    });
  }

  // Operator-triggered compaction. The daemon is idle-only: a run in
  // progress answers 409.
  async compactContext(sessionId: string): Promise<CompactContextResult> {
    return await this.request<CompactContextResult>(
      `/sessions/${encodeURIComponent(sessionId)}/context/compact`,
      { method: "POST" },
    );
  }

  streamSession(
    sessionId: string,
    onEvent: (event: HarnessEvent) => void,
    onConnection: (connected: boolean) => void,
  ): () => void {
    const cursor = this.cursors.get(sessionId) || 0;
    const url = `${this.baseUrl}/sessions/${encodeURIComponent(sessionId)}/events${
      cursor ? `?after=${cursor}` : ""
    }`;
    const source = new EventSource(url, { withCredentials: true });
    const handleMessage = (message: MessageEvent<string>) => {
      try {
        onEvent(normalizeEvent(JSON.parse(message.data) as Record<string, unknown>, sessionId));
      } catch {
        // Ignore malformed frames. The next durable event or reconnect repairs state.
      }
    };
    source.onopen = () => onConnection(true);
    source.onerror = () => onConnection(false);
    source.onmessage = handleMessage;
    for (const type of EVENT_TYPES) {
      source.addEventListener(type, handleMessage as EventListener);
    }
    return () => source.close();
  }
}

// Exported so tests can exercise the offline skill trash/restore/enable
// round-trip without a daemon.
export class FixtureHarnessClient implements HarnessClient {
  readonly mode = "fixture" as const;
  readonly connectionTarget = "fixture harness";
  private sequence = 20;
  private readonly skills: HarnessSkill[] = [
    {
      name: "web-research",
      description: "Search and fetch the web with grounded, ledgered citations.",
      path: "~/.agents/skills/web-research/SKILL.md",
      dir: "~/.agents/skills/web-research",
      files: ["SKILL.md"],
      disableModelInvocation: false,
      content: "Use web.search to find candidate sources, then web.fetch to read the most relevant pages. Cite every claim as [src:N] and record used sources with web.use.",
      enabled: true,
    },
    {
      name: "code-review",
      description: "Review a diff for correctness, safety, and style before merging.",
      path: "~/.agents/skills/code-review/SKILL.md",
      dir: "~/.agents/skills/code-review",
      files: ["SKILL.md"],
      disableModelInvocation: false,
      content: "Review the diff hunk by hunk. Flag correctness bugs first, then safety issues, then style. Cite file and line for every finding.",
      enabled: true,
    },
    {
      name: "release-notes",
      description: "Draft user-facing release notes from the changelog. User-invoked only.",
      path: "~/.agents/skills/release-notes/SKILL.md",
      dir: "~/.agents/skills/release-notes",
      files: ["SKILL.md"],
      disableModelInvocation: true,
      content: "Summarize the changelog as user-facing release notes: group by feature, drop internal plumbing, keep each bullet under one line.",
      enabled: true,
    },
  ];
  private readonly trashedSkills: HarnessSkillTrash[] = [];
  private readonly listeners = new Map<string, Set<(event: HarnessEvent) => void>>();
  private readonly timers = new Map<string, number[]>();
  private readonly snapshots = new Map<string, HarnessSnapshot>();
  private readonly sessions: HarnessSession[] = [
    {
      id: "session-memory",
      title: "memory routing audit",
      profile: "orchestrator",
      model: "Qwen3.8 27B",
      status: "idle",
      updatedAt: "2026-08-31T14:42:00.000Z",
      messageCount: 12,
    },
    {
      id: "session-bench",
      title: "q4 / q6 benchmark",
      profile: "analyst",
      model: "Qwen3.8 27B",
      status: "idle",
      updatedAt: "2026-08-31T12:18:00.000Z",
      messageCount: 8,
    },
    {
      id: "session-shell",
      title: "frontend shell direction",
      profile: "designer",
      model: "Qwen3.8 27B",
      status: "idle",
      updatedAt: "2026-08-30T19:04:00.000Z",
      messageCount: 21,
    },
  ];
  // Representative slice of the daemon's typed registry; the fixture keeps the
  // same provenance/masking semantics so the settings UI behaves identically.
  private readonly settings: HarnessSetting[] = [
    { key: "port", type: "integer", secret: false, restartRequired: true, source: "default", active: true, value: 8787, saved: null },
    { key: "provider", type: "string", secret: false, restartRequired: true, source: "default", active: true, value: "fixture", saved: null },
    { key: "provider.model", type: "string", secret: false, restartRequired: false, source: "default", active: true, value: "fixture-model", saved: null },
    { key: "provider.context_window", type: "integer", secret: false, restartRequired: false, source: "default", active: true, value: 131072, saved: null },
    { key: "provider.temperature", type: "number", secret: false, restartRequired: false, source: "default", active: true, value: 0.7, saved: null },
    { key: "provider.api_key", type: "string", secret: true, restartRequired: false, source: "default", active: true, value: null, saved: null },
    { key: "summary.include_reasoning", type: "boolean", secret: false, restartRequired: false, source: "default", active: true, value: true, saved: null },
    { key: "summary.source_budget_bytes", type: "integer", secret: false, restartRequired: false, source: "default", active: true, value: 262144, saved: null },
    { key: "tool.timeout_ms", type: "integer", secret: false, restartRequired: false, source: "default", active: true, value: 60000, saved: null },
    { key: "web.enabled", type: "boolean", secret: false, restartRequired: false, source: "default", active: true, value: true, saved: null },
    { key: "web.search_endpoint", type: "string", secret: false, restartRequired: false, source: "default", active: true, value: "http://<deployment-lan>:8080", saved: null },
    { key: "web.firecrawl_endpoint", type: "string", secret: false, restartRequired: false, source: "default", active: true, value: "http://<deployment-lan>:3002", saved: null },
  ];

  constructor() {
    const session = this.sessions[0];
    const run: HarnessRun = {
      id: "run-memory-014",
      sessionId: session.id,
      profile: session.profile,
      model: session.model,
      provider: "fixture",
      status: "completed",
      startedAt: "2026-08-31T14:40:12.000Z",
      finishedAt: "2026-08-31T14:41:03.000Z",
      reasoning:
        "The memory path should stay outside the stable prompt. Route only the relevant facts into the request context, then record provenance for replay.",
      tools: [
        {
          id: "tool-1",
          name: "memory.search",
          target: "default",
          status: "completed",
          durationMs: 842,
          summary: "returned 6 relevant memories",
        },
        {
          id: "tool-2",
          name: "file.read",
          target: "docs/memory-contract.md",
          status: "completed",
          durationMs: 214,
          summary: "read 184 lines",
        },
      ],
      metrics: {
        inputTokens: 1482,
        outputTokens: 638,
        reasoningTokens: 921,
        contextUsed: 18204,
        contextWindow: 131072,
        tokensPerSecond: 18.6,
      },
    };
    const events = [
      this.event(session.id, run.id, "run.started", { profile: run.profile }),
      this.event(session.id, run.id, "tool.started", {
        tool_id: "tool-1",
        name: "memory.search",
      }),
      this.event(session.id, run.id, "tool.completed", {
        tool_id: "tool-1",
        duration_ms: 842,
      }),
      this.event(session.id, run.id, "run.completed", { output_tokens: 638 }),
    ];
    this.snapshots.set(session.id, {
      session,
      messages: [
        {
          id: 1,
          parentId: 0,
          role: "user",
          content: "keep the memory layer useful without turning every prompt into a filing cabinet.",
          createdAt: "2026-08-31T14:40:12.000Z",
          runId: run.id,
          isActive: true,
        },
        {
          id: 2,
          parentId: 1,
          role: "assistant",
          content:
            "The clean split is **stable identity**, **on-demand skills**, and **request-scoped memory**.\n\nThe harness can keep the model's personality compact while still giving it the right facts for the current task.",
          createdAt: "2026-08-31T14:41:03.000Z",
          reasoning: run.reasoning,
          runId: run.id,
          isActive: true,
        },
      ],
      run,
      events,
      artifacts: [
        { name: "memory-contract.md", kind: "document", detail: "updated 4m ago" },
        { name: "recall-sample.json", kind: "data", detail: "6 records" },
      ],
    });
    for (const other of this.sessions.slice(1)) {
      this.snapshots.set(other.id, {
        session: other,
        messages: [],
        run: null,
        events: [],
        artifacts: [],
      });
    }
    this.seedFixturePack();
  }

  private event(
    sessionId: string,
    runId: string,
    type: HarnessEventType,
    payload: Record<string, unknown>,
  ): HarnessEvent {
    const sequence = this.sequence++;
    return {
      eventId: `${runId}:${sequence}`,
      sessionId,
      runId,
      sequence,
      type,
      emittedAt: nowIso(),
      payload,
    };
  }

  private emit(event: HarnessEvent): void {
    const snapshot = this.snapshots.get(event.sessionId);
    if (snapshot) snapshot.events = [...snapshot.events, event].slice(-100);
    for (const listener of this.listeners.get(event.sessionId) || []) listener(event);
  }

  async listSessions(): Promise<HarnessSession[]> {
    return clone(this.sessions);
  }

  async createSession(profile = "", speakers?: string[], groupRounds?: number): Promise<HarnessSession> {
    const session: HarnessSession = {
      id: `session-${Date.now()}`,
      title: "new conversation",
      profile: profile || (speakers && speakers[0]) || "orchestrator",
      model: "Qwen3.8 27B",
      status: "idle",
      updatedAt: nowIso(),
      messageCount: 0,
      speakers: speakers && speakers.length > 0 ? [...speakers] : undefined,
      groupRoundsOverride: groupRounds,
    };
    this.sessions.unshift(session);
    this.snapshots.set(session.id, {
      session,
      messages: [],
      run: null,
      events: [],
      artifacts: [],
    });
    return clone(session);
  }

  async setSessionGroupRounds(sessionId: string, rounds: number | null): Promise<void> {
    const session = this.sessions.find((item) => item.id === sessionId);
    if (!session) return;
    session.groupRoundsOverride = rounds === null ? undefined : rounds;
    const snapshot = this.snapshots.get(sessionId);
    if (snapshot?.session) snapshot.session.groupRoundsOverride = session.groupRoundsOverride;
  }

  // Group chat invite: the fixture appends to the session's live roster
  // (the daemon is the authority in http mode; the fixture mirrors the
  // invite contract).
  async addSessionSpeaker(sessionId: string, profileId: string): Promise<string[]> {
    const session = this.sessions.find((item) => item.id === sessionId);
    if (!session) return [];
    const roster = session.speakers && session.speakers.length > 0
      ? [...session.speakers]
      : session.profile
        ? [session.profile]
        : [];
    if (!roster.includes(profileId)) roster.push(profileId);
    session.speakers = roster;
    const snapshot = this.snapshots.get(sessionId);
    if (snapshot?.session) snapshot.session.speakers = [...roster];
    return [...roster];
  }

  async loadSession(sessionId: string): Promise<HarnessSnapshot> {
    const snapshot = this.snapshots.get(sessionId);
    if (!snapshot) throw new Error("session not found");
    return clone(snapshot);
  }

  async sendMessage(input: {
    sessionId: string;
    message: string;
    profile: string;
    attachments?: HarnessAttachment[];
    mentions?: string[];
    messageId?: number;
  }): Promise<{ runId: string }> {
    const snapshot = this.snapshots.get(input.sessionId);
    if (!snapshot) throw new Error("session not found");
    const runId = `run-${Date.now()}`;
    const run: HarnessRun = {
      id: runId,
      sessionId: input.sessionId,
      profile: input.profile,
      model: snapshot.session.model,
      provider: "fixture",
      status: "queued",
      reasoning: "",
      tools: [],
      metrics: {
        inputTokens: 0,
        outputTokens: 0,
        reasoningTokens: 0,
        contextUsed: 0,
        contextWindow: 131072,
        tokensPerSecond: 0,
      },
    };
    snapshot.run = run;
    snapshot.session.status = "running";
    snapshot.session.updatedAt = nowIso();
    snapshot.session.messageCount += 1;
    const timers: number[] = [];
    const later = (delay: number, fn: () => void) => {
      const timer = window.setTimeout(fn, delay);
      timers.push(timer);
    };
    later(80, () => this.emit(this.event(input.sessionId, runId, "run.accepted", {})));
    later(260, () => {
      run.status = "running";
      run.startedAt = nowIso();
      this.emit(this.event(input.sessionId, runId, "run.started", { profile: input.profile }));
    });
    later(520, () => {
      const text = "I will keep the durable memory out of the stable prompt and route only relevant facts into this run.";
      run.reasoning += text;
      this.emit(this.event(input.sessionId, runId, "reasoning.delta", { text }));
    });
    later(800, () => {
      const tool: HarnessTool = {
        id: `${runId}:tool-1`,
        name: "memory.search",
        target: "default",
        status: "running",
      };
      run.tools.push(tool);
      this.emit(this.event(input.sessionId, runId, "tool.started", {
        tool_id: tool.id,
        name: tool.name,
        target: tool.target,
      }));
    });
    later(1280, () => {
      const tool = run.tools[0];
      if (!tool) return;
      tool.status = "completed";
      tool.durationMs = 480;
      tool.summary = "returned 4 relevant memories";
      this.emit(this.event(input.sessionId, runId, "tool.completed", {
        tool_id: tool.id,
        duration_ms: tool.durationMs,
        summary: tool.summary,
      }));
    });
    const answer = "That gives the agent a small, stable identity with a deeper context layer available only when the task needs it.";
    for (const [index, text] of answer.split(" ").entries()) {
      later(1550 + index * 55, () => {
        run.status = "running";
        run.metrics.outputTokens = index + 1;
        this.emit(this.event(input.sessionId, runId, "text.delta", { text: `${index ? " " : ""}${text}` }));
      });
    }
    later(2500, () => {
      run.status = "completed";
      run.finishedAt = nowIso();
      run.metrics = {
        ...run.metrics,
        inputTokens: 412,
        outputTokens: answer.split(" ").length,
        reasoningTokens: 28,
        contextUsed: 4860,
        tokensPerSecond: 19.2,
      };
      snapshot.session.status = "idle";
      snapshot.session.updatedAt = nowIso();
      this.emit(this.event(input.sessionId, runId, "run.completed", {
        output_tokens: run.metrics.outputTokens,
        duration_ms: 2500,
      }));
      this.timers.delete(runId);
    });
    this.timers.set(runId, timers);
    return { runId };
  }

  async cancelRun(runId: string): Promise<void> {
    const timers = this.timers.get(runId) || [];
    for (const timer of timers) window.clearTimeout(timer);
    this.timers.delete(runId);
    const snapshot = [...this.snapshots.values()].find((item) => item.run?.id === runId);
    if (!snapshot?.run) return;
    snapshot.run.status = "cancelled";
    snapshot.run.finishedAt = nowIso();
    snapshot.session.status = "idle";
    this.emit(this.event(snapshot.session.id, runId, "run.cancelled", {}));
  }

  async steerRun(runId: string): Promise<void> {
    // The fixture has no tool iterations to bound, so its next message
    // boundary is now: complete the run immediately.
    const timers = this.timers.get(runId) || [];
    for (const timer of timers) window.clearTimeout(timer);
    this.timers.delete(runId);
    const snapshot = [...this.snapshots.values()].find((item) => item.run?.id === runId);
    if (!snapshot?.run) return;
    snapshot.run.status = "completed";
    snapshot.run.finishedAt = nowIso();
    snapshot.session.status = "idle";
    this.emit(this.event(snapshot.session.id, runId, "run.completed", {}));
  }

  steerSession(): Promise<CommandAck> {
    // Fixture has one fake run; nothing to track.
    return Promise.resolve({ runId: "fixture-run" });
  }

  deliverSession(): Promise<CommandAck> {
    // Fixture has no queue plumbing.
    return Promise.resolve({ runId: "fixture-run" });
  }

  cancelSession(): Promise<CommandAck> {
    // Fixture cancels its fake run below.
    return Promise.resolve({ runId: "fixture-run" });
  }

  async promoteQueuedRun(): Promise<void> {
    // Fixture mode has no live tool loop: promotion would be a no-op.
  }

  async regenerate(input: {
    sessionId: string;
    runId: string;
  }): Promise<{ runId: string }> {
    return this.sendMessage({ sessionId: input.sessionId, message: "regenerate", profile: "orchestrator" });
  }

  async continueRun(input: {
    sessionId: string;
    runId: string;
  }): Promise<{ runId: string }> {
    return this.regenerate(input);
  }

  async listProfiles(): Promise<HarnessProfile[]> {
    return [
      // The fixture orchestrator carries a worker allowlist so the profile
      // editor's subagents field has something to show.
      { id: "orchestrator", name: "orchestrator", description: "default session profile", vision: true, promptPackId: "", promptPlan: [], childProfileAllowlist: ["analyst", "designer"] },
      { id: "analyst", name: "analyst", description: "benchmark and evidence review", vision: false, promptPackId: "", promptPlan: [], childProfileAllowlist: [] },
      { id: "designer", name: "designer", description: "visual and interface review", vision: true, promptPackId: "", promptPlan: [], childProfileAllowlist: [] },
    ];
  }

  async listModels(): Promise<{ default: string; models: string[] }> {
    return { default: "fixture-model", models: ["fixture-model"] };
  }

  async listSkills(): Promise<HarnessSkill[]> {
    return this.skills.map((skill) => ({ ...skill }));
  }

  async saveSkill(input: SkillInput): Promise<HarnessSkill[]> {
    const existing = this.skills.filter((skill) => skill.name !== input.name);
    existing.push({
      name: input.name,
      description: input.description,
      path: `.agents/skills/${input.name}/SKILL.md`,
      dir: `.agents/skills/${input.name}`,
      files: ["SKILL.md"],
      disableModelInvocation: input.disableModelInvocation,
      content: input.content,
      enabled: true,
    });
    this.skills.length = 0;
    this.skills.push(...existing);
    return this.listSkills();
  }

  async deleteSkill(name: string): Promise<HarnessSkill[]> {
    const index = this.skills.findIndex((skill) => skill.name === name);
    if (index >= 0) {
      const [removed] = this.skills.splice(index, 1);
      this.trashedSkills.push({ name: removed.name, dir: removed.dir || `` });
    }
    return this.listSkills();
  }

  async listSkillTrash(): Promise<HarnessSkillTrash[]> {
    return this.trashedSkills.map((entry) => ({ ...entry }));
  }

  async restoreSkill(name: string): Promise<HarnessSkill[]> {
    const index = this.trashedSkills.findIndex((entry) => entry.name === name);
    if (index >= 0) {
      const [entry] = this.trashedSkills.splice(index, 1);
      this.skills.push({
        name: entry.name,
        description: "restored from the trash",
        path: `${entry.dir}/SKILL.md`,
        dir: entry.dir,
        files: ["SKILL.md"],
        disableModelInvocation: false,
        content: "",
        enabled: true,
      });
    }
    return this.listSkills();
  }

  async deleteSkillTrash(name: string): Promise<HarnessSkillTrash[]> {
    const index = this.trashedSkills.findIndex((entry) => entry.name === name);
    if (index >= 0) this.trashedSkills.splice(index, 1);
    return this.listSkillTrash();
  }

  async setSkillEnabled(name: string, enabled: boolean): Promise<HarnessSkill[]> {
    const skill = this.skills.find((entry) => entry.name === name);
    if (skill) skill.enabled = enabled;
    return this.listSkills();
  }

  private readonly snippets: PromptSnippet[] = [];

  async listPromptSnippets(): Promise<PromptSnippet[]> {
    return this.snippets.map((snippet) => ({ ...snippet }));
  }

  async savePromptSnippet(input: PromptSnippetInput): Promise<PromptSnippet[]> {
    const existing = this.snippets.filter((snippet) => snippet.name !== input.name);
    existing.push({ ...input });
    this.snippets.length = 0;
    this.snippets.push(...existing);
    return this.listPromptSnippets();
  }

  async deletePromptSnippet(name: string): Promise<PromptSnippet[]> {
    const index = this.snippets.findIndex((snippet) => snippet.name === name);
    if (index >= 0) this.snippets.splice(index, 1);
    return this.listPromptSnippets();
  }

  async health(): Promise<HarnessHealth> {
    return { provider: "fixture", model: "fixture-model", contextWindow: 131072, vision: false, defaultProvider: "" };
  }

  async listSettings(): Promise<HarnessSetting[]> {
    return this.settings.map((setting) => ({ ...setting }));
  }

  async updateSettings(updates: Record<string, string | number | boolean>): Promise<HarnessSetting[]> {
    for (const [key, value] of Object.entries(updates)) {
      const setting = this.settings.find((item) => item.key === key);
      if (!setting) throw new Error(`unknown setting '${key}'`);
      if (setting.type === "string" && typeof value !== "string") throw new Error(`setting '${key}' expects a string`);
      if (setting.type === "boolean" && typeof value !== "boolean") throw new Error(`setting '${key}' expects a boolean`);
      if ((setting.type === "integer" || setting.type === "number") && typeof value !== "number") {
        throw new Error(`setting '${key}' expects a number`);
      }
      setting.value = setting.secret ? "••••" : value;
      setting.saved = setting.secret ? "••••" : value;
      setting.source = "saved";
    }
    return this.settings.map((setting) => ({ ...setting }));
  }

  async checkProvider(input: { endpoint: string; apiKey?: string }): Promise<ProviderCheckResult> {
    if (!input.endpoint) throw new Error("endpoint required");
    return { ok: true, models: [{ id: "fixture-model", contextWindow: 131072 }] };
  }

  async createProfile(input: ProfileInput): Promise<HarnessProfile> {
    return {
      id: input.id,
      name: input.name,
      description: input.description,
      identityPrompt: input.identityPrompt,
      baseModel: input.baseModel,
      ...(input.provider ? { provider: input.provider } : {}),
      temperature: input.temperature,
      vision: input.vision,
      promptPackId: input.promptPackId || "",
      promptPlan: input.promptPlan || [],
      childProfileAllowlist: input.childProfileAllowlist || [],
    };
  }

  async updateProfile(input: ProfileInput): Promise<HarnessProfile> {
    return this.createProfile(input);
  }

  // ── Prompt packs (in-memory fixture: one demo pack) ──────────────────────

  private readonly fixturePacks = new Map<string, {
    id: string;
    name: string;
    description: string;
    revision: number;
    validationStatus: "valid" | "invalid" | "unchecked";
    fingerprint: string;
    createdAt: string;
    updatedAt: string;
    files: PromptPackFile[];
  }>();

  private seedFixturePack() {
    if (this.fixturePacks.has("demo")) return;
    const now = new Date().toISOString();
    const demoFiles: { path: string; content: string }[] = [
      { path: "manifest.json", content: JSON.stringify({
        pack: "demo",
        manifest_version: 1,
        modules: [
          { id: "shared.policy", path: "shared/policy.md", category: "policy", required: true, reorderable: false },
          { id: "demo.core", path: "demo/CORE.md", category: "identity", reorderable: false },
          { id: "demo.voice", path: "demo/voice.md", category: "voice" },
          { id: "demo.quirks", path: "demo/quirks.md", category: "optional" },
        ],
      }, null, 2) },
      { path: "shared/policy.md", content: "# shared policy\n\n- evidence before agreement\n- reversible actions first\n" },
      { path: "demo/CORE.md", content: "# demo core\n\nA compact fixture identity for exercising the prompt-pack editor.\n" },
      { path: "demo/voice.md", content: "# demo voice\n\nConcise, dry, low-energy.\n" },
      { path: "demo/quirks.md", content: "# demo quirks\n\nNotices the remaining inefficiency after a task completes.\n" },
    ];
    const pack = {
      id: "demo",
      name: "demo",
      description: "fixture pack for the prompt-pack editor",
      revision: 1,
      validationStatus: "valid" as const,
      fingerprint: "fixture-demo-pack",
      createdAt: now,
      updatedAt: now,
      files: demoFiles.map((file) => ({
        path: file.path,
        content: file.content,
        contentHash: `fixture-${file.path}`,
        revision: 1,
        createdAt: now,
        updatedAt: now,
      })),
    };
    this.fixturePacks.set("demo", pack);
  }

  private fixturePackOrThrow(id: string) {
    const pack = this.fixturePacks.get(id);
    if (!pack) throw new Error(`harness request failed: 404: pack not found`);
    return pack;
  }

  private fixtureValidate(pack: {
    files: PromptPackFile[];
  }): PackValidation {
    const errors: string[] = [];
    const warnings: string[] = [];
    const files = new Map(pack.files.map((file) => [file.path, file.content]));
    if (!files.has("manifest.json")) {
      errors.push("pack must contain the manifest.json file");
      return { valid: false, errors, warnings };
    }
    let manifest: {
      pack?: string;
      manifest_version?: number;
      modules?: { id?: string; path?: string; category?: string; required?: boolean }[];
      slots?: Record<string, string[]>;
    };
    try {
      manifest = JSON.parse(files.get("manifest.json") || "");
    } catch {
      errors.push("manifest.json is not valid JSON");
      return { valid: false, errors, warnings };
    }
    if (manifest.manifest_version !== 1) errors.push("unsupported manifest_version (expected 1)");
    const modules = Array.isArray(manifest.modules) ? manifest.modules : [];
    if (modules.length === 0) errors.push("manifest: no modules declared");
    const known = new Set(modules.map((module) => module.id || ""));
    for (const module of modules) {
      if (!module.id || !known.has(module.id)) errors.push(`invalid or duplicate module id '${module.id}'`);
      if (!module.path || !files.has(module.path)) errors.push(`module '${module.id}': path not in pack`);
    }
    for (const [path] of files) {
      if (path !== "manifest.json" && !modules.some((module) => module.path === path)) {
        warnings.push(`file '${path}' is not referenced by any module`);
      }
    }
    return { valid: errors.length === 0, errors, warnings };
  }

  private fixtureFingerprint(pack: { id: string; files: PromptPackFile[] }): string {
    // The daemon computes a real SHA-256; the fixture only needs a stable
    // content-derived value so edits change it.
    let hash = 0;
    const canonical = pack.id + pack.files
      .map((file) => `${file.path}:${file.content.length}:${file.content}`)
      .join("\x1f");
    for (let i = 0; i < canonical.length; i++) {
      hash = (hash * 31 + canonical.charCodeAt(i)) >>> 0;
    }
    return hash.toString(16).padStart(8, "0");
  }

  async listPromptPacks(): Promise<PromptPackSummary[]> {
    return [...this.fixturePacks.values()].map((pack) => ({
      id: pack.id,
      name: pack.name,
      description: pack.description,
      revision: pack.revision,
      validationStatus: pack.validationStatus,
      fingerprint: pack.fingerprint,
      fileCount: pack.files.length,
      createdAt: pack.createdAt,
      updatedAt: pack.updatedAt,
    }));
  }

  async createPromptPack(input: {
    id: string;
    name: string;
    description?: string;
    files?: { path: string; content: string }[];
  }): Promise<PromptPackDetail> {
    if (this.fixturePacks.has(input.id)) {
      throw new Error(`harness request failed: 409: pack already exists`);
    }
    const now = new Date().toISOString();
    const pack: {
      id: string;
      name: string;
      description: string;
      revision: number;
      validationStatus: "valid" | "invalid" | "unchecked";
      fingerprint: string;
      createdAt: string;
      updatedAt: string;
      files: PromptPackFile[];
    } = {
      id: input.id,
      name: input.name,
      description: input.description || "",
      revision: 1,
      validationStatus: "unchecked",
      fingerprint: "",
      createdAt: now,
      updatedAt: now,
      files: (input.files || []).map((file) => ({
        path: file.path,
        content: file.content,
        contentHash: `fixture-${file.path}`,
        revision: 1,
        createdAt: now,
        updatedAt: now,
      })),
    };
    const validation = this.fixtureValidate(pack);
    pack.validationStatus = validation.valid ? "valid" : "invalid";
    pack.fingerprint = this.fixtureFingerprint(pack);
    this.fixturePacks.set(input.id, pack);
    return this.getPromptPack(input.id);
  }

  async getPromptPack(id: string): Promise<PromptPackDetail> {
    const pack = this.fixturePackOrThrow(id);
    const validation = this.fixtureValidate(pack);
    return {
      id: pack.id,
      name: pack.name,
      description: pack.description,
      revision: pack.revision,
      validationStatus: validation.valid ? "valid" : "invalid",
      fingerprint: this.fixtureFingerprint(pack),
      fileCount: pack.files.length,
      createdAt: pack.createdAt,
      updatedAt: pack.updatedAt,
      files: pack.files.map((file) => ({ ...file })),
      validation,
    };
  }

  async listPackSnapshots(packId: string): Promise<PackSnapshotSummary[]> {
    this.fixturePackOrThrow(packId);
    return [];
  }

  async getPackSnapshot(id: number): Promise<PackSnapshot> {
    throw new Error(`fixture: pack snapshot ${id} not found`);
  }

  async updatePromptPack(id: string, name: string, description: string): Promise<PromptPackSummary> {
    const pack = this.fixturePackOrThrow(id);
    pack.name = name;
    pack.description = description;
    pack.updatedAt = new Date().toISOString();
    return {
      id: pack.id,
      name: pack.name,
      description: pack.description,
      revision: pack.revision,
      validationStatus: pack.validationStatus,
      fingerprint: pack.fingerprint,
      fileCount: pack.files.length,
      createdAt: pack.createdAt,
      updatedAt: pack.updatedAt,
    };
  }

  async upsertPackFile(packId: string, path: string, content: string): Promise<PackValidation> {
    const pack = this.fixturePackOrThrow(packId);
    const now = new Date().toISOString();
    const existing = pack.files.find((file) => file.path === path);
    if (existing) {
      existing.content = content;
      existing.revision += 1;
      existing.updatedAt = now;
    } else {
      pack.files.push({
        path,
        content,
        contentHash: `fixture-${path}`,
        revision: 1,
        createdAt: now,
        updatedAt: now,
      });
    }
    pack.files.sort((a, b) => a.path.localeCompare(b.path));
    pack.revision += 1;
    pack.updatedAt = now;
    return this.fixtureValidate(pack);
  }

  async deletePackFile(packId: string, path: string): Promise<PackValidation> {
    const pack = this.fixturePackOrThrow(packId);
    const index = pack.files.findIndex((file) => file.path === path);
    if (index === -1) throw new Error(`harness request failed: 404: file not found`);
    pack.files.splice(index, 1);
    pack.revision += 1;
    pack.updatedAt = new Date().toISOString();
    return this.fixtureValidate(pack);
  }

  async deletePromptPack(id: string): Promise<void> {
    if (!this.fixturePacks.has(id)) throw new Error(`harness request failed: 404: pack not found`);
    this.fixturePacks.delete(id);
  }

  async validatePromptPack(id: string): Promise<PackValidation> {
    return this.fixtureValidate(this.fixturePackOrThrow(id));
  }

  async previewPromptPack(packId: string, plan: string[], profileId?: string): Promise<PromptPackPreview> {
    const pack = this.fixturePackOrThrow(packId);
    const validation = this.fixtureValidate(pack);
    const files = new Map(pack.files.map((file) => [file.path, file.content]));
    let manifest: { modules?: { id?: string; path?: string; category?: string; required?: boolean }[] };
    try {
      manifest = JSON.parse(files.get("manifest.json") || "");
    } catch {
      manifest = {};
    }
    const modules = Array.isArray(manifest.modules) ? manifest.modules : [];
    const byId = new Map(modules.map((module) => [module.id, module]));
    const errors: string[] = [];
    if (plan.length === 0) errors.push("plan is empty");
    for (const id of plan) {
      if (!byId.has(id)) errors.push(`plan references unknown module '${id}'`);
    }
    for (const module of modules) {
      if (module.required && !plan.includes(module.id || "")) {
        errors.push(`required module '${module.id}' is missing from the plan`);
      }
    }
    const previewErrors = [...validation.errors, ...errors];
    const base: PromptPackPreview = {
      ok: previewErrors.length === 0,
      pack: {
        id: pack.id,
        name: pack.name,
        description: pack.description,
        revision: pack.revision,
        validationStatus: validation.valid ? "valid" : "invalid",
        fingerprint: this.fixtureFingerprint(pack),
        fileCount: pack.files.length,
        createdAt: pack.createdAt,
        updatedAt: pack.updatedAt,
      },
      validation,
      modules: [],
      daemonBlocks: [],
      dynamicBlocks: [
        "[COMPACT CONTEXT] (summary sessions, per session)",
        "[APPROVED MEMORY CONTEXT] (recall-on-turn, per session)",
      ],
      prompt: "",
      totalBytes: 0,
      approxTokens: 0,
      fingerprint: "",
    };
    if (previewErrors.length > 0) {
      return { ...base, ok: false, error: previewErrors.join("; ") };
    }
    let prompt = `[PROMPT PACK]\npack: ${pack.id}\nprofile: ${profileId || "unspecified"}\nrevision: ${pack.revision}\n\n`;
    const resolved: PromptPackModuleInfo[] = [];
    let totalBytes = 0;
    for (const id of plan) {
      const module = byId.get(id);
      if (!module?.path) continue;
      const content = files.get(module.path) || "";
      const text = content.endsWith("\n") ? content : `${content}\n`;
      prompt += `[MODULE ${id}]\n${text}\n`;
      totalBytes += text.length;
      resolved.push({
        id: module.id || id,
        path: module.path,
        category: module.category || "",
        bytes: text.length,
        approxTokens: Math.ceil(text.length / 4),
        required: module.required === true,
      });
    }
    return {
      ...base,
      ok: true,
      modules: resolved,
      prompt,
      totalBytes,
      approxTokens: Math.ceil(totalBytes / 4),
      fingerprint: this.fixtureFingerprint({ id: pack.id, files: pack.files }) + ":plan:" + plan.join(","),
    };
  }

  async renameProfile(): Promise<void> {
    // Fixture mode is a visual prototype; profile renames only exist
    // against the daemon.
    return;
  }

  async deleteProfile(): Promise<void> {
    return;
  }

  async selectProfile(): Promise<void> {
    return;
  }

  async listProviders(): Promise<HarnessProvider[]> {
    return [];
  }

  async getInfra(): Promise<InfraReport> {
    // Fixture mode is a visual prototype: a plausible empty box.
    return {
      host: {
        available: true,
        memTotalMb: 16384,
        memAvailableMb: 8192,
        swapTotalMb: 0,
        swapFreeMb: 0,
        load1: 0.1,
        load5: 0.1,
        load15: 0.1,
        cores: 8,
      },
      gpu: { available: false, vendor: "", devices: [] },
      dockerAvailable: false,
      containers: [],
      providers: [
        { name: "primary", endpoint: "http://127.0.0.1:8080/v1", online: true, latencyMs: 4 },
      ],
      collectedAtMs: Date.now(),
    };
  }

  async upsertProvider(input: ProviderInput): Promise<HarnessProvider> {
    return {
      name: input.name,
      endpoint: input.endpoint,
      model: input.model || "",
      apiKey: "••••",
      params: input.params || {},
      createdAt: new Date().toISOString(),
      updatedAt: new Date().toISOString(),
    };
  }

  async deleteProvider(): Promise<void> {
    return;
  }

  async checkProviderRoute(): Promise<ProviderCheckResult> {
    return { ok: true, models: [] };
  }

  async deleteSession(sessionId: string): Promise<void> {
    this.snapshots.delete(sessionId);
    const index = this.sessions.findIndex((item) => item.id === sessionId);
    if (index !== -1) this.sessions.splice(index, 1);
  }

  async renameSession(sessionId: string, title: string): Promise<void> {
    const index = this.sessions.findIndex((item) => item.id === sessionId);
    if (index !== -1) this.sessions[index] = { ...this.sessions[index], title };
  }

  async setSessionTags(sessionId: string, tags: string[]): Promise<void> {
    const index = this.sessions.findIndex((item) => item.id === sessionId);
    if (index !== -1) this.sessions[index] = { ...this.sessions[index], tags: [...tags] };
  }

  async retitleSession(): Promise<void> {
    // Fixture mode is a visual prototype; model re-titling only exists
    // against the daemon.
  }

  async getJourney(sessionId: string): Promise<JourneyInfo | null> {
    // The fixture serves a canned journey so the section's layout is visible.
    return {
      sessionId,
      profile: "nyaxus",
      text: "2026-10-06 08:14 UTC - The session opened with a request to investigate the daemon log for a recurring provider timeout. Decision: keep the mock provider for the e2e pass; the timeout was traced to the summary job's 3s probe cap.\n\n2026-10-06 09:02 UTC - Follow-up confirmed the fix; the operator re-ran the suite and the run wrapped up with a green pass. The journey is current.",
      watermark: 42,
      updatedAt: nowIso(),
    };
  }

  async requestJourneyUpdate(): Promise<void> {
    return;
  }

  async linkVault(sessionId: string, path: string): Promise<void> {
    const index = this.sessions.findIndex((item) => item.id === sessionId);
    if (index !== -1) this.sessions[index] = { ...this.sessions[index], mdLogPath: path };
  }

  async unlinkVault(sessionId: string): Promise<void> {
    const index = this.sessions.findIndex((item) => item.id === sessionId);
    if (index !== -1) this.sessions[index] = { ...this.sessions[index], mdLogPath: "" };
  }

  async listFs(): Promise<FsListing> {
    // Fixture mode has no daemon filesystem to browse.
    return { entries: [], truncated: false };
  }

  async getSessionStatus(sessionId: string): Promise<HarnessSessionStatus> {
    // Fixture mode has no daemon state log; report none recorded.
    return { sessionId, latest: "", events: [] };
  }

  async listSources(): Promise<SourceInfo[]> {
    // Fixture mode has no fetched web sources.
    return [];
  }

  async selectIteration(): Promise<void> {
    return;
  }

  async searchSession(sessionId: string, query: string): Promise<SearchResponse> {
    // Fixture mode has no source session; search this session's own messages.
    const snapshot = this.snapshots.get(sessionId);
    const needle = query.toLowerCase();
    const matches: SearchMatch[] = [];
    for (const message of snapshot?.messages || []) {
      if (matches.length >= 20) break;
      const haystack = message.content.toLowerCase();
      const pos = haystack.indexOf(needle);
      if (pos === -1) continue;
      const begin = Math.max(0, pos - 60);
      const end = Math.min(message.content.length, pos + query.length + 60);
      let snippet = message.content.slice(begin, end).replace(/\s+/g, " ");
      if (begin > 0) snippet = "\u2026" + snippet;
      if (end < message.content.length) snippet += "\u2026";
      matches.push({
        messageId: message.id,
        sessionId,
        role: message.role,
        createdAt: message.createdAt,
        snippet,
      });
    }
    return { scope: "self", sessionId, query, matches };
  }

  async searchAll(query: string): Promise<GlobalSearchResponse> {
    // Fixture mode mirrors the daemon's bounded scan over the loaded
    // snapshots: newest session first, active-path messages only, capped.
    const needle = query.toLowerCase();
    const maxMatches = 50;
    const matches: GlobalSearchMatch[] = [];
    const sessions = [...this.sessions].sort((a, b) => b.updatedAt.localeCompare(a.updatedAt));
    for (const session of sessions) {
      if (matches.length >= maxMatches) break;
      const snapshot = this.snapshots.get(session.id);
      if (!snapshot) continue;
      for (const message of snapshot.messages) {
        if (!message.isActive) continue;
        const haystack = message.content.toLowerCase();
        const pos = haystack.indexOf(needle);
        if (pos === -1) continue;
        const begin = Math.max(0, pos - 60);
        const end = Math.min(message.content.length, pos + query.length + 60);
        let snippet = message.content.slice(begin, end).replace(/\s+/g, " ");
        if (begin > 0) snippet = "\u2026" + snippet;
        if (end < message.content.length) snippet += "\u2026";
        matches.push({
          messageId: message.id,
          sessionId: session.id,
          sessionTitle: session.title,
          role: message.role,
          createdAt: message.createdAt,
          snippet,
        });
        if (matches.length >= maxMatches) break;
      }
    }
    return { query, matches, truncated: matches.length >= maxMatches };
  }

  async memoryStatus(): Promise<MemoryStatus> {
    return {
      enabled: true,
      provider: "custom",
      endpoint: "fixture",
      bank: "fixture-bank",
      healthy: true,
      jobs: {},
      custom: { ready: true, reranker_loaded: true, units: { world: 1 } },
    };
  }

  async memoryRecall(input: { query: string; session?: string; profile?: string }): Promise<MemoryRecallResponse> {
    // Fixture mode has no provider; echo a couple of canned facts so the
    // surface is exercisable in the visual prototype.
    const needle = input.query.toLowerCase();
    const facts = [
      { id: "mem-fixture-1", text: "the vault passphrase is copper-meridian-7", type: "world" },
      { id: "mem-fixture-2", text: "the user prefers minimal implementations over abstractions", type: "preference" },
    ];
    const results = facts.filter((fact) => fact.text.toLowerCase().includes(needle) || needle.length < 3);
    return { source: "local", fallback: true, query: input.query, results };
  }

  // Fixture mode keeps a small in-memory curation overlay so the unit
  // operations surface is exercisable in the visual prototype.
  private fixtureUnitOps = new Map<
    string,
    {
      state?: string;
      state_reason?: string;
      versions?: MemoryUnitVersion[];
      deleted?: boolean;
      core?: boolean;
    }
  >();

  private fixtureCannedUnits(): MemoryBankUnit[] {
    const now = new Date().toISOString();
    return [
      {
        id: "mem-fixture-1",
        type: "world",
        state: "active",
        scope: "global",
        text: "the vault passphrase is copper-meridian-7",
        created_at: now,
        updated_at: now,
      },
      {
        id: "mem-fixture-2",
        type: "experience",
        state: "active",
        scope: "session:fixture",
        text: "the user prefers minimal implementations over abstractions",
        created_at: now,
        updated_at: now,
      },
    ];
  }

  private fixtureUnitById(id: string): MemoryBankUnit | undefined {
    const overlay = this.fixtureUnitOps.get(id);
    if (overlay?.deleted) return undefined;
    const unit = this.fixtureCannedUnits().find((candidate) => candidate.id === id);
    if (!unit) return undefined;
    const versions = overlay?.versions || [];
    const latest = versions[versions.length - 1];
    return {
      ...unit,
      state: overlay?.state || unit.state,
      ...(overlay?.state_reason ? { state_reason: overlay.state_reason } : {}),
      ...(latest ? { text: latest.text } : {}),
      ...(overlay?.core !== undefined ? { core: overlay.core } : {}),
    };
  }

  async memoryBank(input?: {
    text?: string;
    type?: string;
    scope?: string;
    state?: string;
    limit?: number;
    offset?: number;
  }): Promise<MemoryBankResponse> {
    // Fixture mode has no bank; a couple of canned units keep the browser
    // surface exercisable in the visual prototype.
    const units = this.fixtureCannedUnits()
      .map((unit) => this.fixtureUnitById(unit.id))
      .filter((unit): unit is MemoryBankUnit => unit !== undefined);
    let filtered = units;
    if (input?.text) {
      const needle = input.text.toLowerCase();
      filtered = filtered.filter((unit) => unit.text.toLowerCase().includes(needle));
    }
    if (input?.type) filtered = filtered.filter((unit) => unit.type === input.type);
    if (input?.state && input.state !== "all") {
      filtered = filtered.filter((unit) => unit.state === input.state);
    }
    const limit = input?.limit ?? 50;
    const offset = input?.offset ?? 0;
    return { total: filtered.length, units: filtered.slice(offset, offset + limit) };
  }

  async getMemoryUnit(id: string): Promise<MemoryUnitDetail> {
    const unit = this.fixtureUnitById(id);
    if (!unit) throw new Error("unit not found");
    const overlay = this.fixtureUnitOps.get(id);
    const versions =
      overlay?.versions && overlay.versions.length > 0
        ? overlay.versions
        : [{ id: `${id}-v1`, text: unit.text, current: true }];
    return { unit, versions, links: [], feedback: { offered: 0, cited: 0, used: 0, acted_on: 0 } };
  }

  async invalidateMemoryUnit(id: string, reason?: string): Promise<void> {
    if (!this.fixtureUnitById(id)) throw new Error("unit not found");
    const overlay = this.fixtureUnitOps.get(id) || {};
    this.fixtureUnitOps.set(id, {
      ...overlay,
      state: "invalidated",
      state_reason: reason || "invalidated by operator",
    });
  }

  async setMemoryUnitCore(id: string, on: boolean): Promise<void> {
    if (!this.fixtureUnitById(id)) throw new Error("unit not found");
    const overlay = this.fixtureUnitOps.get(id) || {};
    this.fixtureUnitOps.set(id, {
      ...overlay,
      core: on,
    });
  }

  async reactivateMemoryUnit(id: string, reason?: string): Promise<void> {
    if (!this.fixtureUnitById(id)) throw new Error("unit not found");
    const overlay = this.fixtureUnitOps.get(id) || {};
    this.fixtureUnitOps.set(id, {
      ...overlay,
      state: "active",
      state_reason: reason || "reactivated by operator",
    });
  }

  async supersedeMemoryUnit(id: string, text: string, reason?: string): Promise<void> {
    if (!this.fixtureUnitById(id)) throw new Error("unit not found");
    const overlay = this.fixtureUnitOps.get(id) || {};
    const versions = (overlay.versions || []).map((version) => ({ ...version, current: false }));
    versions.push({
      id: `${id}-v${versions.length + 1}`,
      text,
      store_reason: reason || "superseded by operator",
      created_at: new Date().toISOString(),
      current: true,
    });
    this.fixtureUnitOps.set(id, { ...overlay, versions });
  }

  async consolidateMemory(): Promise<{ ok: boolean; job_id: number }> {
    // Fixture mode proposes a candidate so the approval surface is
    // exercisable in the visual prototype.
    this.fixtureConsolidationSeq += 1;
    this.fixtureConsolidations = [
      {
        id: this.fixtureConsolidationSeq,
        older_unit_id: 101 + this.fixtureConsolidationSeq,
        newer_unit_id: 102 + this.fixtureConsolidationSeq,
        cosine: 0.941,
        scope: "profile:ops",
        status: "pending",
        created_at: new Date().toISOString(),
        older_text: "The deployment target is the homelab server at <deployment-lan>.",
        newer_text: "The deployment target is the homelab server at <deployment-lan> today.",
      },
      ...this.fixtureConsolidations,
    ];
    return { ok: true, job_id: this.fixtureConsolidationSeq };
  }

  private fixtureConsolidations: MemoryConsolidation[] = [
    {
      id: 1,
      older_unit_id: 101,
      newer_unit_id: 102,
      cosine: 0.941,
      scope: "profile:ops",
      status: "pending",
      created_at: new Date().toISOString(),
      older_text: "The deployment target is the homelab server at <deployment-lan>.",
      newer_text: "The deployment target is the homelab server at <deployment-lan> today.",
    },
  ];
  private fixtureConsolidationSeq = 1;

  async listConsolidations(status?: string): Promise<MemoryConsolidation[]> {
    if (!status) return [...this.fixtureConsolidations];
    return this.fixtureConsolidations.filter((item) => item.status === status);
  }

  private decideFixtureConsolidation(
    id: number,
    status: MemoryConsolidationStatus,
  ): MemoryConsolidation {
    const item = this.fixtureConsolidations.find((candidate) => candidate.id === id);
    if (!item) throw new Error("consolidation candidate not found");
    item.status = status;
    item.decided_at = new Date().toISOString();
    return item;
  }

  async approveConsolidation(id: number): Promise<MemoryConsolidation> {
    return this.decideFixtureConsolidation(id, "approved");
  }

  async rejectConsolidation(id: number): Promise<MemoryConsolidation> {
    return this.decideFixtureConsolidation(id, "rejected");
  }

  async deleteMemoryUnit(id: string): Promise<void> {
    if (!this.fixtureUnitById(id)) throw new Error("unit not found");
    const overlay = this.fixtureUnitOps.get(id) || {};
    this.fixtureUnitOps.set(id, { ...overlay, deleted: true });
  }

  // Fixture mode keeps an in-memory record queue so the approval surface is
  // exercisable in the visual prototype.
  private fixtureRecords: MemoryRecord[] = [
    {
      id: 1,
      status: "candidate",
      type: "preference",
      text: "the user prefers minimal implementations over abstractions",
      scope: "session",
      created_at: new Date().toISOString(),
    },
  ];
  private fixtureRecordSeq = 1;

  async listMemoryRecords(status?: string): Promise<MemoryRecord[]> {
    if (!status) return [...this.fixtureRecords];
    return this.fixtureRecords.filter((record) => record.status === status);
  }

  async createMemoryRecord(input: MemoryRecordInput): Promise<MemoryRecord> {
    this.fixtureRecordSeq += 1;
    const record: MemoryRecord = {
      id: this.fixtureRecordSeq,
      status: "candidate",
      type: input.type || "fact",
      text: input.text,
      scope: input.scope || "session",
      ...(input.session_id ? { session_id: input.session_id } : {}),
      created_at: new Date().toISOString(),
    };
    this.fixtureRecords = [record, ...this.fixtureRecords];
    return record;
  }

  private decideFixtureRecord(id: number, status: MemoryRecordStatus): MemoryRecord {
    const record = this.fixtureRecords.find((item) => item.id === id);
    if (!record) throw new Error("memory record not found");
    record.status = status;
    record.decided_at = new Date().toISOString();
    return record;
  }

  async approveMemoryRecord(id: number): Promise<MemoryRecord> {
    return this.decideFixtureRecord(id, "approved");
  }

  async rejectMemoryRecord(id: number): Promise<MemoryRecord> {
    return this.decideFixtureRecord(id, "rejected");
  }

  async supersedeMemoryRecord(id: number): Promise<MemoryRecord> {
    return this.decideFixtureRecord(id, "superseded");
  }

  async listAlwaysAllowed(): Promise<ApprovalRule[]> {
    // Fixture mode has no server-side approvals to list.
    return [];
  }

  async removeAlwaysAllowed(_tool: string): Promise<void> {
    return;
  }

  async rewind(): Promise<void> {
    // Fixture mode is a visual prototype; transcript surgery only exists
    // against the daemon.
    return;
  }

  async deleteMessage(): Promise<void> {
    // Fixture mode is a visual prototype; transcript surgery only exists
    // against the daemon.
    return;
  }

  async editMessage(): Promise<{ runId: string; newMessageId?: number }> {
    // Transcript branching only exists against the daemon.
    throw new Error("message editing requires the daemon (fixture mode is a visual prototype)");
  }

  async selectMessage(): Promise<void> {
    return;
  }

  async forkSession(): Promise<HarnessSession> {
    return this.createSession();
  }

  async respondApproval(
    runId: string,
    approvalId: string,
    decision: "allow_once" | "allow_turn" | "allow_session" | "deny",
  ): Promise<void> {
    const snapshot = [...this.snapshots.values()].find((item) => item.run?.id === runId);
    if (!snapshot?.run || snapshot.run.approval?.id !== approvalId) return;
    snapshot.run.approval = undefined;
    snapshot.run.status = decision === "deny" ? "failed" : "running";
  }

  async answerInput(runId: string, inputId: string, choice: number): Promise<void> {
    void choice; // the fixture has no grading; only the pending state matters
    const snapshot = [...this.snapshots.values()].find((item) => item.run?.id === runId);
    if (!snapshot?.run || snapshot.run.input?.id !== inputId) return;
    snapshot.run.input = undefined;
    snapshot.run.status = "running";
  }

  async answerSudo(runId: string, inputId: string, password: string, scope: string): Promise<void> {
    void password;
    void scope; // the fixture resolves the gate; no password mechanics
    const snapshot = [...this.snapshots.values()].find((item) => item.run?.id === runId);
    if (!snapshot?.run || snapshot.run.input?.id !== inputId) return;
    snapshot.run.input = undefined;
    snapshot.run.status = "running";
  }

  async respondContextDecision(
    sessionId: string,
    decision: "primary" | "deterministic" | "retry",
  ): Promise<void> {
    void decision;
    const session = this.sessions.find((item) => item.id === sessionId);
    if (session) session.contextPrompt = "";
  }

  async compactContext(sessionId: string): Promise<CompactContextResult> {
    void sessionId;
    return { ok: true, status: "skipped", detail: "fixture" };
  }

  streamSession(
    sessionId: string,
    onEvent: (event: HarnessEvent) => void,
    onConnection: (connected: boolean) => void,
  ): () => void {
    const listeners = this.listeners.get(sessionId) || new Set();
    listeners.add(onEvent);
    this.listeners.set(sessionId, listeners);
    onConnection(true);
    return () => {
      listeners.delete(onEvent);
      if (listeners.size === 0) this.listeners.delete(sessionId);
      onConnection(false);
    };
  }
}

export function createHarnessClient(): HarnessClient {
  return import.meta.env.VITE_HARNESS_MODE === "http"
    ? new HttpHarnessClient()
    : new FixtureHarnessClient();
}
