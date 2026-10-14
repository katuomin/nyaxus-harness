import { Fragment, memo, useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState, Component } from "react";
import type { ChangeEvent, ClipboardEvent, DragEvent, FormEvent, KeyboardEvent, ReactNode } from "react";
import { createPortal } from "react-dom";
import type { LucideIcon } from "lucide-react";
import {
  Activity,
  ArrowUpRight,
  BarChart3,
  Bell,
  BookOpen,
  Bot,
  BrainCircuit,
  Cable,
  Check,
  ChevronDown,
  ChevronLeft,
  ChevronRight,
  CircleAlert,
  CircleStop,
  Clock3,
  Command,
  Copy,
  Download,
  Eye,
  FileCode2,
  FileJson,
  FileText,
  FolderOpen,
  Globe,
  Inbox,
  HelpCircle,
  Sparkles,
  Wrench,
  Gauge,
  GitFork,
  GripVertical,
  History,
  Upload,
  Image as ImageIcon,
  Info,
  ListChecks,
  LoaderCircle,
  Menu,
  MessageSquare,
  Moon,
  PanelRight,
  Paperclip,
  Pencil,
  Pin,
  Play,
  Plus,
  RefreshCw,
  RotateCcw,
  Route,
  Search,
  Send,
  ShieldCheck,
  Shrink,
  SlidersHorizontal,
  Sun,
  Table2,
  Trash2,
  TriangleAlert,
  UserRound,
  Users,
  X,
  Zap,
} from "lucide-react";
import {
  advanceEventWindow,
  composerPlaceholder,
  createHarnessClient,
  mergePromptSnippets,
  parseSpawnResult,
  steerFlashText,
  type PromptSnippet,
  pendingActionsFromEvents,
  profileVisionEnabled,
  runsFromEvents,
  runBlocksFromEvents,
  type ForkMode,
  type HarnessAttachment,
  type HarnessClient,
  type HarnessEvent,
  type HarnessMessage,
  type HarnessProfile,
  type HarnessProvider,
  type HarnessRun,
  type InfraReport,
  type HarnessSkill,
  type HarnessSkillTrash,
  type PendingAction,
  type ProvenanceEntry,
  type ProfileInput,
  type SkillInput,
  type HarnessSetting,
  type HarnessSession,
  type HarnessSessionStatus,
  type HarnessSnapshot,
  type HarnessTool,
  type SearchResponse,
  type GlobalSearchMatch,
  type GlobalSearchResponse,
  type ProviderCheckResult,
  type ProviderModelOption,
  type RunStatus,
  type RunBlock,
  type MemoryStatus,
  type MemoryRecallItem,
  type MemoryRecallResponse,
  type MemoryRecord,
  type MemoryRecordInput,
  type MemoryConsolidation,
  type MemoryBankUnit,
  type MemoryBankResponse,
  type MemoryUnitDetail,
  type MemorySweepInfo,
  type PackValidation,
  type PromptPackDetail,
  type PromptPackPreview,
  type PromptPackSummary,
  type FsEntry,
  type FsListing,
  type SourceInfo,
  type ApprovalRule,
  type TaskItem,
  type JourneyInfo,
  tasksFromEvents,
  taskChecklistRecords,
  type TaskChecklistRecord,
  groupStateFromEvents,
  groupTranscriptItems,
  type GroupTranscriptItem,
} from "./harness";
import { chipifyMentions, computeGraphEdges, computeGraphRows, getActiveLeaf, getActivePath, mergeOptimisticSnapshot, seedAssistantFromRun, tempMessageId, upsertAssistant, type GraphRow } from "./messageTree";
import { MarkdownContent } from "./markdown";
import { hashFor, parseHash, type View } from "./routing";

type NotificationKind = "completed" | "failed" | "cancelled" | "approval" | "offline" | "restored";

interface AppNotification {
  id: string;
  kind: NotificationKind;
  text: string;
  sessionId?: string;
  time: string;
  read: boolean;
}

interface QueuedPrompt {
  id: string;
  sessionId: string;
  runId: string;
  content: string;
  attachments: HarnessAttachment[];
}

// A queued prompt pulled back into the composer for editing. The token changes
// on every edit so the composer re-seeds even for identical prompts.
interface ComposerSeed {
  token: number;
  sessionId: string;
  attachments: HarnessAttachment[];
}

const notificationIcon: Record<NotificationKind, LucideIcon> = {
  completed: Check,
  failed: CircleAlert,
  cancelled: X,
  approval: ShieldCheck,
  offline: CircleAlert,
  restored: Check,
};

const navGroups: { label: string; items: { id: View; label: string; icon: LucideIcon }[] }[] = [
  {
    label: "control",
    items: [
      { id: "chat", label: "Conversations", icon: MessageSquare },
      { id: "runs", label: "Active runs", icon: Activity },
    ],
  },
  {
    label: "systems",
    items: [
      { id: "memory", label: "Memory", icon: BrainCircuit },
      { id: "workspace", label: "Workspace", icon: FolderOpen },
      { id: "observatory", label: "Observatory", icon: BarChart3 },
      { id: "providers", label: "Connections", icon: Cable },
      { id: "profiles", label: "Profiles", icon: UserRound },
      { id: "knowledge", label: "Knowledge", icon: BookOpen },
      { id: "prompts", label: "Prompts", icon: FileText },
      { id: "skills", label: "Skills", icon: Sparkles },
      { id: "tools", label: "Tools", icon: Wrench },
      { id: "settings", label: "Settings", icon: SlidersHorizontal },
    ],
  },
];

const initialRunMetrics = {
  inputTokens: 0,
  outputTokens: 0,
  reasoningTokens: 0,
  contextUsed: 0,
  contextWindow: 131072,
  tokensPerSecond: 0,
};

function timestampMs(value?: string): number {
  if (!value) return Number.NaN;
  const numeric = Number(value);
  if (Number.isFinite(numeric)) {
    if (numeric > 1e17) return numeric / 1e6;  // nanoseconds
    if (numeric > 1e14) return numeric / 1e3;  // microseconds
    if (numeric > 1e11) return numeric;        // milliseconds
    if (numeric > 1e8) return numeric * 1000;  // seconds
  }
  const parsed = Date.parse(value);
  return Number.isFinite(parsed) ? parsed : Number.NaN;
}

function formatTime(value?: string): string {
  const timestamp = timestampMs(value);
  if (!Number.isFinite(timestamp)) return "--:--";
  const date = new Date(timestamp);
  const time = date.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  // Cross-day entries need a date, not just a time, so the sidebar ordering
  // is legible (a "06:44" from yesterday is indistinguishable from today's).
  const now = new Date();
  if (date.toDateString() === now.toDateString()) return time;
  const yesterday = new Date(now);
  yesterday.setDate(now.getDate() - 1);
  if (date.toDateString() === yesterday.toDateString()) return `yesterday ${time}`;
  return `${date.toLocaleDateString([], { month: "short", day: "numeric" })} ${time}`;
}

function formatNumber(value: number): string {
  return value.toLocaleString();
}

function formatBytes(value: number): string {
  if (value < 1024) return `${value} b`;
  if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} kb`;
  return `${(value / (1024 * 1024)).toFixed(1)} mb`;
}

const MAX_ATTACHMENTS = 9;
const MAX_TEXT_ATTACHMENT_BYTES = 200 * 1024;
// Binary documents (pdf/docx/xlsx/pptx) attach as kind document: stored +
// serialized for the terminal tool, never read into the prompt. The cap
// matches the daemon's 8 MiB total attachment payload budget.
const MAX_DOCUMENT_ATTACHMENT_BYTES = 6 * 1024 * 1024;
const DOCUMENT_EXTENSIONS = /\.(pdf|docx?|xlsx?|pptx?)$/i;
const DOCUMENT_MIMES = new Set([
  "application/pdf",
  "application/msword",
  "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
  "application/vnd.ms-excel",
  "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
  "application/vnd.ms-powerpoint",
  "application/vnd.openxmlformats-officedocument.presentationml.presentation",
]);

function isDocumentFile(file: File): boolean {
  if (file.type && DOCUMENT_MIMES.has(file.type.toLowerCase())) return true;
  return DOCUMENT_EXTENSIONS.test(file.name);
}

function readAsDataURL(file: File | Blob): Promise<string> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result));
    reader.onerror = () => reject(reader.error ?? new Error("read failed"));
    reader.readAsDataURL(file);
  });
}
const MAX_IMAGE_SOURCE_BYTES = 50 * 1024 * 1024;
const MAX_DECODED_IMAGE_PIXELS = 40_000_000;
const MAX_IMAGE_LONG_EDGE = 2048;
// The UI is not file storage: images are normalized to a bounded budget
// (quality floor jpeg 0.7 or lossless png) instead of keeping large
// lossless originals. The daemon's 4 MiB encoded cap stays the hard limit.
const MAX_NORMALIZED_IMAGE_BYTES = 1.5 * 1024 * 1024;
const MAX_NORMALIZED_IMAGE_DATA_URL_CHARS = 2_100_000;
const MAX_ATTACHMENT_PAYLOAD_BYTES = 8 * 1024 * 1024;
// Composer text above this ships as a .txt attachment instead of inline
// message content, so the chat view stays lean on big pastes.
const LONG_TEXT_ATTACHMENT_BYTES = 8 * 1024;
// How many active-path turns stay mounted at the tail of a long chat; scrolling
// to the top loads more in the same-sized chunks.
const CHAT_WINDOW_SIZE = 40;
const COMPOSER_MAX_HEIGHT_PX = 240;
const VISION_UNAVAILABLE_ERROR = "images unavailable for this profile";
const TEXT_FILE_ACCEPT = "text/*,.md,.json,.yaml,.yml,.toml,.sh,.csv";
const IMAGE_FILE_ACCEPT = "image/jpeg,image/png,image/webp,image/gif,.jpg,.jpeg,.png,.webp,.gif";

function isImageFile(file: File): boolean {
  return imageMimeTypeForFile(file) !== null;
}

function attachmentSummary(attachment: HarnessAttachment): string {
  return attachment.kind === "image"
    ? formatBytes(attachment.size)
    : `~${Math.ceil(attachment.content.length / 4)} tok`;
}

function imageMimeTypeForFile(file: File): string | null {
  const mime = file.type.toLowerCase();
  if (mime === "image/jpeg" || mime === "image/jpg") return "image/jpeg";
  if (mime === "image/png" || mime === "image/webp" || mime === "image/gif") return mime;
  const extension = file.name.toLowerCase().split(".").pop();
  if (extension === "jpg" || extension === "jpeg") return "image/jpeg";
  if (extension === "png") return "image/png";
  if (extension === "webp") return "image/webp";
  if (extension === "gif") return "image/gif";
  return null;
}

function yieldToBrowser(): Promise<void> {
  return new Promise((resolve) => window.setTimeout(resolve, 0));
}

function blobToDataUrl(blob: Blob): Promise<string> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => {
      if (typeof reader.result === "string") resolve(reader.result);
      else reject(new Error("browser returned no image data"));
    };
    reader.onerror = () => reject(new Error("browser could not read the normalized image"));
    reader.readAsDataURL(blob);
  });
}

function canvasToBlob(canvas: HTMLCanvasElement, mimeType: string, quality: number): Promise<Blob | null> {
  return new Promise((resolve) => canvas.toBlob(resolve, mimeType, quality));
}

function dataUrlPayloadLength(dataUrl: string): number {
  const comma = dataUrl.indexOf(",");
  return comma >= 0 ? dataUrl.length - comma - 1 : dataUrl.length;
}

function resampleCanvas(source: HTMLCanvasElement, width: number, height: number): HTMLCanvasElement {
  const canvas = document.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  const context = canvas.getContext("2d");
  if (!context) throw new Error("browser could not prepare an image canvas");
  context.imageSmoothingEnabled = true;
  context.imageSmoothingQuality = "high";
  context.drawImage(source, 0, 0, width, height);
  return canvas;
}

async function encodeCanvas(canvas: HTMLCanvasElement): Promise<{
  dataUrl: string;
  mimeType: string;
  size: number;
}> {
  // llama.cpp's multimodal endpoint accepts png and jpeg reliably. Do not
  // choose webp merely because the browser can encode it: provider image
  // decoders are not required to support every browser output format.
  const candidates = [
    { mimeType: "image/jpeg", quality: 0.85 },
    { mimeType: "image/jpeg", quality: 0.7 },
    { mimeType: "image/png", quality: 1 },
  ];
  // Smallest encoding that fits the budget wins, so flat screenshots can
  // still land as compact lossless png while photos take the jpeg path.
  for (const scale of [1, 0.75, 0.55]) {
    const width = Math.max(1, Math.round(canvas.width * scale));
    const height = Math.max(1, Math.round(canvas.height * scale));
    const surface = scale === 1 ? canvas : resampleCanvas(canvas, width, height);
    let best: { dataUrl: string; mimeType: string; size: number } | null = null;
    for (const candidate of candidates) {
      await yieldToBrowser();
      const blob = await canvasToBlob(surface, candidate.mimeType, candidate.quality);
      if (!blob) continue;
      if (!best || blob.size < best.size) {
        best = {
          dataUrl: await blobToDataUrl(blob),
          mimeType: blob.type || candidate.mimeType,
          size: blob.size,
        };
      }
    }
    if (!best) throw new Error("browser could not encode this image");
    if (best.size <= MAX_NORMALIZED_IMAGE_BYTES &&
        dataUrlPayloadLength(best.dataUrl) <= MAX_NORMALIZED_IMAGE_DATA_URL_CHARS) {
      return best;
    }
    // Nothing fits the budget at this size; shrink and retry.
  }
  throw new Error(`image remains too large after compression (max ${formatBytes(MAX_NORMALIZED_IMAGE_BYTES)})`);
}

async function normalizeImageFile(file: File, id: string): Promise<HarnessAttachment> {
  if (file.size > MAX_IMAGE_SOURCE_BYTES) {
    throw new Error(`source image exceeds ${formatBytes(MAX_IMAGE_SOURCE_BYTES)}`);
  }
  const sourceMimeType = imageMimeTypeForFile(file);
  if (!sourceMimeType) throw new Error("use jpeg, png, webp, or gif images");
  if (typeof createImageBitmap !== "function") throw new Error("this browser cannot decode images safely");

  await yieldToBrowser();
  let bitmap: ImageBitmap;
  try {
    bitmap = await createImageBitmap(file);
  } catch {
    throw new Error("could not decode image");
  }
  try {
    const sourceWidth = Math.floor(bitmap.width);
    const sourceHeight = Math.floor(bitmap.height);
    if (sourceWidth < 1 || sourceHeight < 1 || sourceWidth > MAX_DECODED_IMAGE_PIXELS / sourceHeight) {
      throw new Error("image has too many decoded pixels (max 40 megapixels)");
    }
    const scale = Math.min(1, MAX_IMAGE_LONG_EDGE / Math.max(sourceWidth, sourceHeight));
    const width = Math.max(1, Math.round(sourceWidth * scale));
    const height = Math.max(1, Math.round(sourceHeight * scale));
    const canvas = document.createElement("canvas");
    canvas.width = width;
    canvas.height = height;
    const context = canvas.getContext("2d");
    if (!context) throw new Error("browser could not prepare an image canvas");
    context.imageSmoothingEnabled = true;
    context.imageSmoothingQuality = "high";
    await yieldToBrowser();
    context.drawImage(bitmap, 0, 0, width, height);
    const encoded = await encodeCanvas(canvas);
    return {
      id,
      name: file.name,
      kind: "image",
      mimeType: encoded.mimeType,
      size: encoded.size,
      width,
      height,
      content: encoded.dataUrl,
    };
  } finally {
    bitmap.close();
  }
}

function AttachmentIcon({ attachment }: { attachment: HarnessAttachment }) {
  return attachment.kind === "image"
    ? <ImageIcon size={12} aria-hidden="true" />
    : <FileText size={12} aria-hidden="true" />;
}

function formatElapsed(startedAt?: string): string {
  const timestamp = timestampMs(startedAt);
  if (!Number.isFinite(timestamp)) return "00:00";
  const seconds = Math.max(0, Math.floor((Date.now() - timestamp) / 1000));
  const minutes = Math.floor(seconds / 60).toString().padStart(2, "0");
  const rest = (seconds % 60).toString().padStart(2, "0");
  return `${minutes}:${rest}`;
}

function isRunActive(status: RunStatus): boolean {
  return status === "running" || status === "queued" || status === "waiting_approval" || status === "waiting_input";
}

// Per-session reading position: module-level because switching lives inside
// ChatView's render-time reset.
const sessionScrollMemory = new Map<string, number>();

// Error grammar: decode known payload shapes (nested error objects, raw
// JSON), classify by pattern, and carry a one-next-step hint - one-sentence
// opaque walls were the observed rough corner, and the field has converged
// on kind → copy. Client-side by design: the ledger's error strings stay
// freeform.
interface ErrorGrammar {
  title: string;
  detail: string | null;
  hint: string | null;
}

const ERROR_RULES: [RegExp, string, string][] = [
  [/Loading model|HTTP 503/, "model starting",
    "the endpoint is booting the model (or paused); the provider retry ladder rides this out - retrying an hour later usually succeeds"],
  [/provider closed the stream|before the first payload|before \[DONE\]/, "stream closed",
    "the endpoint dropped mid-turn; retry continues from the persisted progress"],
  [/interrupted by a daemon restart/, "interrupted",
    "the daemon restarted mid-turn; streamed progress was salvaged into this turn - retry continues from here"],
  [/context (window|budget)|exceeds the context/, "context",
    "the context is full - compress it from the context meter, or fork the conversation"],
  [/rate.?limit|429|overloaded/i, "rate limit",
    "provider backpressure - wait out the quota window; the queue resumes after"],
  [/connection failed|could not be reached|unreachable|failed to connect/i, "unreachable",
    "the endpoint is not answering - check it is running and the address is current"],
  [/HTTP 40[13]/, "access",
    "the endpoint refused access - check the API key or provider config"],
];

// Daemon failure kind -> short title (polishing-pass A-4). The daemon is
// authoritative when it carries a kind; the pattern rules below are the
// fallback for freeform errors it did not classify.
const KIND_TITLES: Record<string, string> = {
  "rate-limit": "rate limited",
  "overloaded": "provider overloaded",
  "context-window": "context too large",
  "stream-disconnected": "stream disconnected",
  "interrupted": "interrupted by restart",
  "invalid-input": "invalid input",
  "denied": "denied by operator",
  "internal": "internal error",
};

function errorGrammar(raw: string, kind?: string, daemonHint?: string): ErrorGrammar {
  const text = (raw || "").trim();
  if (!text) return { title: "failed", detail: null, hint: daemonHint ?? null };
  let unwrapped = text;
  try {
    const parsed = JSON.parse(text) as unknown;
    if (parsed && typeof parsed === "object") {
      const obj = parsed as Record<string, unknown>;
      let candidate: unknown = parsed;
      if (obj.error && typeof obj.error === "object") {
        candidate = (obj.error as Record<string, unknown>).message ?? obj.error;
      } else if (typeof obj.detail === "string") {
        candidate = obj.detail;
      } else if (typeof obj.message === "string") {
        candidate = obj.message;
      }
      if (typeof candidate === "string" && candidate) unwrapped = candidate;
    }
  } catch {
    // Plain text: the common shape.
  }
  const firstLine = unwrapped.split(/\r?\n/)[0] || unwrapped;
  const detail = unwrapped.length > 170 ? unwrapped : null;
  if (kind) {
    const title = KIND_TITLES[kind] ?? (firstLine.length > 160 ? `${firstLine.slice(0, 157)}...` : firstLine);
    return { title, detail, hint: daemonHint ?? null };
  }
  let hint: string | null = null;
  for (const [pattern, , text_hint] of ERROR_RULES) {
    if (pattern.test(unwrapped)) {
      hint = text_hint;
      break;
    }
  }
  const title = firstLine.length > 160 ? `${firstLine.slice(0, 157)}...` : firstLine;
  return { title, detail, hint };
}

// Live elapsed counter for a streaming/run strip (1s interval scoped here).
function LiveElapsed({ startedAt }: { startedAt?: string }) {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    const timer = window.setInterval(() => setNow(Date.now()), 1000);
    return () => window.clearInterval(timer);
  }, []);
  const started = startedAt ? Date.parse(startedAt) : NaN;
  if (!Number.isFinite(started)) return null;
  const seconds = Math.max(0, Math.floor((now - started) / 1000));
  return <span className="streaming-elapsed">· {formatToolDuration(seconds * 1000)}</span>;
}

function statusLabel(status: RunStatus): string {
  return status.replace("_", " ");
}

function statusClass(status: RunStatus): string {
  if (status === "completed") return "status-complete";
  if (status === "failed") return "status-failed";
  if (status === "cancelled") return "status-cancelled";
  if (status === "waiting_approval" || status === "waiting_input") return "status-waiting";
  return "status-live";
}

function eventText(event: HarnessEvent): string {
  const payload = event.payload;
  if (event.type === "tool.started") return `started ${String(payload.name || "tool")}`;
  if (event.type === "tool.completed") return `completed ${String(payload.summary || "tool")}`;
  if (event.type === "reasoning.delta") return "reasoning updated";
  if (event.type === "reasoning") return "reasoning complete";
  if (event.type === "text.delta") return "assistant response streamed";
  if (event.type === "text") return "assistant response";
  if (event.type === "run.completed") return "run completed";
  if (event.type === "run.cancelled") return "run cancelled";
  if (event.type === "run.failed") return String(payload.error || "run failed");
  return event.type.replaceAll(".", " ");
}

// Clipboard with a legacy fallback for non-secure contexts (plain http on a
// LAN address), where navigator.clipboard is unavailable.
async function copyText(text: string): Promise<void> {
  try {
    await navigator.clipboard.writeText(text);
    return;
  } catch {
    // Fall through to the selection-based path.
  }
  const area = document.createElement("textarea");
  area.value = text;
  area.style.position = "fixed";
  area.style.opacity = "0";
  document.body.appendChild(area);
  area.select();
  const ok = document.execCommand("copy");
  area.remove();
  if (!ok) throw new Error("copy unavailable");
}

function AttachmentPills({
  attachments,
  variant,
  onOpen,
  onRemove,
}: {
  attachments: HarnessAttachment[];
  variant: "composer" | "message";
  onOpen?: (index: number) => void;
  onRemove?: (index: number) => void;
}) {
  if (attachments.length === 0) return null;
  return (
    <div className={variant === "composer" ? "attachment-strip" : "message-attachments"} aria-label={variant === "composer" ? "Selected attachments" : "Message attachments"}>
      {attachments.map((attachment, index) => {
        const tokenBody = (
          <>
            <span className="attachment-order" aria-hidden="true">{index + 1}</span>
            <AttachmentIcon attachment={attachment} />
            <span className="attachment-name">{attachment.name}</span>
            <span className="attachment-tok">{attachmentSummary(attachment)}</span>
          </>
        );
        return (
          <span className={variant === "composer" ? "attachment-chip" : "message-attachment-chip"} key={`${attachment.id || attachment.name}-${index}`}>
            {onOpen ? (
              <button
                type="button"
                className="attachment-token-main"
                onClick={() => onOpen(index)}
                title={`open ${attachment.name}`}
                aria-label={`Open ${attachment.kind} attachment ${index + 1}: ${attachment.name}`}
              >
                {tokenBody}
              </button>
            ) : <span className="attachment-token-main">{tokenBody}</span>}
            {onRemove && (
              <button
                type="button"
                className="attachment-token-remove"
                onClick={() => onRemove(index)}
                aria-label={`Remove attachment ${index + 1}: ${attachment.name}`}
              >
                <X size={11} />
              </button>
            )}
          </span>
        );
      })}
    </div>
  );
}

function AttachmentOverlay({
  attachments,
  index,
  onClose,
  onSelect,
}: {
  attachments: HarnessAttachment[];
  index: number | null;
  onClose: () => void;
  onSelect: (index: number) => void;
}) {
  const [copiedIndex, setCopiedIndex] = useState<number | null>(null);
  const attachment = index === null ? undefined : attachments[index];
  useEffect(() => {
    if (index === null || !attachment) return;
    const onKey = (event: globalThis.KeyboardEvent) => {
      if (event.key === "Escape") onClose();
      if (event.key === "ArrowLeft" && index > 0) onSelect(index - 1);
      if (event.key === "ArrowRight" && index < attachments.length - 1) onSelect(index + 1);
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [attachment, attachments.length, index, onClose, onSelect]);
  if (!attachment || index === null) return null;
  // Normalize line endings for display and copy so CRLF/CR files render
  // deterministically in the pre block.
  const content = attachment.content || "";
  const imageSource = content || attachment.contentUrl || "";
  return createPortal(
    <div className="overlay-backdrop" onClick={onClose}>
      <div className="attachment-overlay" onClick={(event) => event.stopPropagation()} role="dialog" aria-modal="true" aria-label={attachment.name}>
        <div className="overlay-head">
          <div>
            <span className="eyebrow">attachment</span>
            <h2>{attachment.name}</h2>
          </div>
          <button type="button" className="overlay-close" onClick={onClose} aria-label="Close attachment">
            <X size={15} />
          </button>
        </div>
        <div className="overlay-meta">
          <AttachmentIcon attachment={attachment} />
          {attachment.kind} · {formatBytes(attachment.size)}
          {attachment.width && attachment.height ? ` · ${attachment.width} × ${attachment.height}` : ""}
        </div>
        {attachments.length > 1 && (
          <div className="overlay-nav" aria-label="Attachment navigation">
            <button type="button" onClick={() => onSelect(index - 1)} disabled={index === 0} aria-label="Previous attachment">
              <ChevronLeft size={13} /> previous
            </button>
            <span aria-live="polite">{index + 1} / {attachments.length}</span>
            <button type="button" onClick={() => onSelect(index + 1)} disabled={index === attachments.length - 1} aria-label="Next attachment">
              next <ChevronRight size={13} />
            </button>
          </div>
        )}
        {attachment.kind === "document" ? (
          <div className="overlay-media">
            <div className="overlay-empty" role="status">binary document attachment - stored with the run; the model works on the workspace copy</div>
          </div>
        ) : attachment.kind === "text" ? (
          <>
            <pre className="overlay-content">{content.replace(/\r\n?/g, "\n")}</pre>
            <div className="overlay-actions">
              <button
                type="button"
                onClick={() => {
                  void navigator.clipboard.writeText(content).then(() => {
                    setCopiedIndex(index);
                    setTimeout(() => setCopiedIndex((current) => current === index ? null : current), 1500);
                  });
                }}
              >
                {copiedIndex === index ? <Check size={13} /> : <Copy size={13} />} {copiedIndex === index ? "copied" : "copy"}
              </button>
            </div>
          </>
        ) : (
          <div className="overlay-media">
            {imageSource ? <img src={imageSource} alt={attachment.name} /> : <div className="overlay-empty" role="status">image content unavailable</div>}
          </div>
        )}
      </div>
    </div>,
    document.body,
  );
}


function MessageBlock({
  message,
  onRegenerate,
  onEdit,
  onDelete,
  canRegenerate,
  branchNav,
}: {
  message: HarnessMessage;
  onRegenerate?: (runId: string) => void;
  onEdit?: (message: HarnessMessage, content: string) => void;
  onDelete?: (message: HarnessMessage) => void;
  canRegenerate?: boolean;
  branchNav?: ReactNode;
}) {
  const isUser = message.role === "user";
  const [overlayIndex, setOverlayIndex] = useState<number | null>(null);
  const [editing, setEditing] = useState(false);
  const [editText, setEditText] = useState("");
  const [confirmDelete, setConfirmDelete] = useState(false);
  const confirmTimerRef = useRef<number | undefined>(undefined);
  const requestDelete = () => {
    if (confirmDelete) {
      window.clearTimeout(confirmTimerRef.current);
      setConfirmDelete(false);
      onDelete?.(message);
      return;
    }
    setConfirmDelete(true);
    window.clearTimeout(confirmTimerRef.current);
    confirmTimerRef.current = window.setTimeout(() => setConfirmDelete(false), 2500);
  };
  const startEdit = () => {
    setEditText(message.content);
    setEditing(true);
  };
  const saveEdit = () => {
    const content = editText.trim();
    if (!content || content === message.content) {
      setEditing(false);
      return;
    }
    setEditing(false);
    onEdit?.(message, content);
  };
  return (
    <article className={`message-block ${isUser ? "message-user" : "message-agent"}`}>
      <div className="message-marker">{isUser ? "you" : "nx"}</div>
      <div className="message-main">
        <div className="message-meta">
          <strong>{isUser ? "you" : "nyaxus"}</strong>
          <span>{formatTime(message.createdAt)}</span>
          {message.runId && <span className="message-run">{message.runId.slice(-8)}</span>}
        </div>
        {message.attachments && <AttachmentPills attachments={message.attachments} variant="message" onOpen={setOverlayIndex} />}
        {editing && isUser ? (
          <div className="message-edit">
            <textarea
              value={editText}
              onChange={(event) => setEditText(event.target.value)}
              onKeyDown={(event) => {
                if (event.key === "Enter" && (event.metaKey || event.ctrlKey)) {
                  event.preventDefault();
                  saveEdit();
                }
                if (event.key === "Escape") setEditing(false);
              }}
              rows={3}
              autoFocus
              aria-label="edit message"
            />
            <div className="message-edit-actions">
              <button type="button" className="regenerate-button" onClick={() => setEditing(false)}>
                <X size={12} /> cancel
              </button>
              <button type="button" className="regenerate-button" onClick={saveEdit} disabled={!editText.trim()}>
                <Check size={12} /> save
              </button>
            </div>
          </div>
        ) : (
          <div className="message-body markdown-body">
            <MarkdownContent content={isUser ? chipifyMentions(message.content) : message.content} />
          </div>
        )}
        {!editing && isUser && (onEdit || onDelete || branchNav || onRegenerate) && (
          <div className="message-actions">
            {branchNav}
            {onRegenerate && message.runId && (
              <button
                className="regenerate-button"
                onClick={() => onRegenerate(message.runId!)}
                disabled={!canRegenerate}
                title={canRegenerate ? "no response was produced for this message - run it again" : "stop the active run before resending"}
              >
                <RefreshCw size={12} />
                resend
              </button>
            )}
            {onEdit && (
              <button className="regenerate-button" onClick={startEdit} title="edit this message; later turns become a branch">
                <Pencil size={12} />
                edit
              </button>
            )}
            {onDelete && (
              <button
                className={`regenerate-button ${confirmDelete ? "branch-chip-active" : ""}`}
                onClick={requestDelete}
                title="remove this message and everything after it; the tail stays reachable as a branch"
              >
                <Trash2 size={12} />
                {confirmDelete ? "sure?" : "delete"}
              </button>
            )}
          </div>
        )}
        {!isUser && message.reasoning && (
          <details className="message-reasoning">
            <summary>
              <BrainCircuit size={13} />
              reasoning trace
              <ChevronDown size={13} />
            </summary>
            <p>{message.reasoning}</p>
          </details>
        )}
        {!isUser && message.runId && onRegenerate && (
          <button className="regenerate-button" onClick={() => onRegenerate(message.runId!)}>
            <RefreshCw size={12} />
            regenerate
          </button>
        )}
      </div>
      <AttachmentOverlay
        attachments={message.attachments || []}
        index={overlayIndex}
        onClose={() => setOverlayIndex(null)}
        onSelect={setOverlayIndex}
      />
    </article>
  );
}

// The chat renders only the active lineage. Prompts waiting behind an active
// run live in the queue above the composer until their run starts.
function getDisplayPath(messages: HarnessMessage[]): HarnessMessage[] {
  return getActivePath(messages);
}

function reasoningSegmentsForRun(
  events: HarnessEvent[],
  runId: string,
  fallback: string,
): string[] {
  if (!runId) return fallback ? [fallback] : [];
  const segments = [""];
  let afterTool = false;
  for (const event of events) {
    if (event.runId !== runId) continue;
    if (event.type === "tool.completed") {
      afterTool = true;
      continue;
    }
    if (event.type !== "reasoning.delta") continue;
    const text = String(event.payload.text || "");
    if (!text) continue;
    if (afterTool) {
      segments.push("");
      afterTool = false;
    }
    segments[segments.length - 1] += text;
  }
  if (segments.every((segment) => !segment)) return fallback ? [fallback] : [];
  const streamed = segments.join("");
  // The event ledger is intentionally capped to its tail. Once the head of a
  // long reasoning trace has fallen out, the remaining event text is no longer
  // a prefix of the full persisted reasoning; offset-appending it would make
  // the visible head slide as each new delta arrives.
  if (!fallback.startsWith(streamed)) return fallback ? [fallback] : segments;
  if (fallback.length > streamed.length) {
    segments[segments.length - 1] += fallback.slice(streamed.length);
  }
  return segments;
}

// A compact fork replaces the pre-fork transcript with the auxiliary model's
// summary. Surface that summary at the top of the thread, openable like the
// reasoning trace, so the reader can see exactly what the model works from.
// The search affordance recovers verbatim turns from the original
// conversation that the summary no longer carries.
// Compaction fallback prompt: the auxiliary model failed to summarize, so
// the daemon parks its summary job and asks the operator to choose. The
// buttons resolve the decision; the daemon wakes and proceeds with the
// chosen channel (the primary model is guaranteed to fit: the conversation
// is already mid-flight inside its window).
function CompactionPromptCard({
  prompt,
  onDecide,
}: {
  prompt: string;
  onDecide: (decision: "primary" | "deterministic" | "retry") => void;
}) {
  return (
    <div className="context-prompt-card" role="alertdialog">
      <div className="cp-header">
        <HelpCircle size={14} />
        <span>{prompt}</span>
      </div>
      <div className="cp-actions">
        <button type="button" className="cp-allow" onClick={() => onDecide("primary")}>use primary model</button>
        <button type="button" className="cp-retry" onClick={() => onDecide("retry")}>retry auxiliary</button>
        <button type="button" className="cp-deny" onClick={() => onDecide("deterministic")}>deterministic</button>
      </div>
    </div>
  );
}

function CompactedSummaryBlock({
  summary,
  onSearch,
}: {
  summary: string;
  onSearch: (query: string) => Promise<SearchResponse>;
}) {
  const doc = useMemo<Record<string, unknown> | null>(() => {
    try {
      const parsed: unknown = JSON.parse(summary);
      if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
        return parsed as Record<string, unknown>;
      }
    } catch {
      // Not JSON; the raw text renders below instead.
    }
    return null;
  }, [summary]);

  const list = (key: string): string[] => {
    const value = doc ? doc[key] : undefined;
    if (Array.isArray(value)) {
      return value.filter((item): item is string => typeof item === "string");
    }
    return [];
  };

  const goal = doc && typeof doc.goal === "string" ? doc.goal : "";
  const latestRequest = doc && typeof doc.latest_user_request === "string" ? doc.latest_user_request : "";
  const sections = doc
    ? [
        { label: "decisions", items: list("decisions") },
        { label: "constraints", items: list("constraints") },
        { label: "unresolved tasks", items: list("unresolved_tasks") },
        { label: "relevant artifacts", items: list("relevant_artifacts") },
        { label: "next steps", items: list("next_steps") },
      ].filter((section) => section.items.length > 0)
    : [];
  const anchors = list("anchors");

  const [query, setQuery] = useState("");
  const [searching, setSearching] = useState(false);
  const [searchError, setSearchError] = useState<string | null>(null);
  const [results, setResults] = useState<SearchResponse | null>(null);

  async function runSearch(event: FormEvent) {
    event.preventDefault();
    const trimmed = query.trim();
    if (!trimmed || searching) return;
    setSearching(true);
    setSearchError(null);
    try {
      setResults(await onSearch(trimmed));
    } catch (error) {
      setResults(null);
      setSearchError(error instanceof Error ? error.message : String(error));
    } finally {
      setSearching(false);
    }
  }

  return (
    <details className="compacted-summary">
      <summary>
        <FileText size={13} />
        compacted summary
        <span className="compacted-summary-hint">the conversation before this fork was summarized by the auxiliary model</span>
        <ChevronDown size={13} />
      </summary>
      {doc ? (
        <div className="compacted-summary-body">
          {goal && <p className="compacted-summary-goal">{goal}</p>}
          {sections.map((section) => (
            <div key={section.label} className="compacted-summary-section">
              <span>{section.label}</span>
              <ul>
                {section.items.map((item, index) => (
                  <li key={index}>{item}</li>
                ))}
              </ul>
            </div>
          ))}
          {latestRequest && (
            <div className="compacted-summary-section">
              <span>latest user request</span>
              <p>{latestRequest}</p>
            </div>
          )}
          {anchors.length > 0 && (
            <div className="compacted-summary-section">
              <span>anchors, verbatim from source</span>
              <ul>
                {anchors.map((item, index) => (
                  <li key={index}>{item}</li>
                ))}
              </ul>
            </div>
          )}
          <form className="compacted-summary-search" onSubmit={runSearch}>
            <Search size={13} />
            <input
              value={query}
              onChange={(event) => setQuery(event.target.value)}
              placeholder="search the original conversation"
              aria-label="search the original conversation"
            />
            <button type="submit" disabled={searching || !query.trim()}>
              {searching ? "searching…" : "search"}
            </button>
          </form>
          {searchError && (
            <p className="compacted-summary-search-error" role="alert">{searchError}</p>
          )}
          {results && (
            <div className="compacted-summary-results">
              <span>
                {results.matches.length === 0
                  ? `no matches for “${results.query}” in the original conversation`
                  : `${results.matches.length} match${results.matches.length === 1 ? "" : "es"} in the ${
                      results.scope === "source" ? "original conversation" : "session"
                    }`}
              </span>
              <ul>
                {results.matches.map((match) => (
                  <li key={match.messageId}>
                    <span className="compacted-summary-match-meta">
                      {match.role} · message {match.messageId}
                    </span>
                    <span className="compacted-summary-match-snippet">{match.snippet}</span>
                  </li>
                ))}
              </ul>
            </div>
          )}
        </div>
      ) : (
        <pre className="compacted-summary-raw">{summary}</pre>
      )}
    </details>
  );
}

function assistantSiblings(messages: HarnessMessage[], message: HarnessMessage): HarnessMessage[] {
  return messages.filter(
    (m) => m.role === "assistant" && m.parentId === message.parentId,
  );
}

function userSiblings(messages: HarnessMessage[], message: HarnessMessage): HarnessMessage[] {
  return messages.filter(
    (m) => m.role === "user" && m.parentId === message.parentId,
  );
}

// Numbered branch chips + prev/next arrows over a set of sibling messages
// (assistant iterations or user-side edit branches).
function BranchNav({
  siblings,
  activeId,
  onSelect,
  label,
}: {
  siblings: HarnessMessage[];
  activeId: number;
  onSelect: (id: number) => void;
  label: string;
}) {
  const index = Math.max(0, siblings.findIndex((s) => s.id === activeId));
  return (
    <>
      <div className="branch-points">
        {siblings.map((sibling, siblingIndex) => (
          <button
            key={sibling.id}
            className={`branch-chip ${siblingIndex === index ? "branch-chip-active" : ""}`}
            onClick={() => onSelect(sibling.id)}
            title={`${label} ${siblingIndex + 1}`}
            aria-label={`${label} iteration ${siblingIndex + 1}`}
          >
            {siblingIndex + 1}
          </button>
        ))}
      </div>
      <div className="iteration-nav">
        <button
          className="iteration-arrow"
          disabled={index === 0}
          onClick={() => onSelect(siblings[index - 1].id)}
          aria-label="Previous iteration"
        >
          <ChevronLeft size={13} />
        </button>
        <span className="iteration-count">{index + 1}/{siblings.length}</span>
        <button
          className="iteration-arrow"
          disabled={index === siblings.length - 1}
          onClick={() => onSelect(siblings[index + 1].id)}
          aria-label="Next iteration"
        >
          <ChevronRight size={13} />
        </button>
      </div>
    </>
  );
}

function ForkMenu({
  canFork,
  onFork,
  label = "fork path",
}: {
  canFork: boolean;
  onFork: (mode: ForkMode) => void;
  label?: string;
}) {
  const [open, setOpen] = useState(false);
  return (
    <div className="fork-menu">
      <button
        type="button"
        className="small-button"
        onClick={() => setOpen((previous) => !previous)}
        disabled={!canFork}
        aria-expanded={open}
        aria-haspopup="menu"
        title="choose how to fork the current visible path"
      >
        <GitFork size={13} /> {label} <ChevronDown size={12} className="fork-menu-chevron" />
      </button>
      {open && (
        <div className="fork-menu-panel" role="menu">
          <button type="button" role="menuitem" onClick={() => { setOpen(false); onFork("selected"); }}>
            <Copy size={13} />
            <span><strong>basic copy</strong><small>clone the visible path immediately</small></span>
          </button>
          <button type="button" role="menuitem" onClick={() => { setOpen(false); onFork("summary"); }}>
            <BrainCircuit size={13} />
            <span><strong>compact summary</strong><small>prepare a summarized context first</small></span>
          </button>
        </div>
      )}
    </div>
  );
}

// Conversation export: the ledger is the source of truth, so
// the exports are pure serializers over a fresh snapshot - JSON for the full
// ledger, Markdown for a readable transcript, CSV for spreadsheet analysis.
function ExportMenu({ onExport, disabled }: {
  onExport: (format: "json" | "markdown" | "csv") => Promise<void>;
  disabled: boolean;
}) {
  const [open, setOpen] = useState(false);
  const [busy, setBusy] = useState(false);
  const pick = (format: "json" | "markdown" | "csv") => {
    if (busy) return;
    setOpen(false);
    setBusy(true);
    void onExport(format).catch(() => undefined).finally(() => setBusy(false));
  };
  return (
    <div className="export-menu">
      <button
        type="button"
        className="small-button"
        onClick={() => setOpen((previous) => !previous)}
        disabled={disabled}
        aria-expanded={open}
        aria-haspopup="menu"
        title="export this conversation"
      >
        <Download size={13} /> export <ChevronDown size={12} className="export-menu-chevron" />
      </button>
      {open && (
        <div className="export-menu-panel" role="menu">
          <button type="button" role="menuitem" onClick={() => pick("json")} disabled={busy}>
            <FileJson size={13} />
            <span><strong>json</strong><small>full ledger snapshot: session, messages, events, artifacts</small></span>
          </button>
          <button type="button" role="menuitem" onClick={() => pick("markdown")} disabled={busy}>
            <FileText size={13} />
            <span><strong>markdown</strong><small>readable transcript of the active path with per-turn metadata</small></span>
          </button>
          <button type="button" role="menuitem" onClick={() => pick("csv")} disabled={busy}>
            <Table2 size={13} />
            <span><strong>csv</strong><small>event ledger table: sequence, type, run, time, payload</small></span>
          </button>
        </div>
      )}
    </div>
  );
}

// The visible task checklist: tasks.create/tasks.update project to the
// ledger, the card renders the latest replayed state, so it survives
// reloads like the rest of the transcript. While the list is live, ChatView
// renders exactly one, pinned as the bottom-most chat element; when the list
// completes or is discarded, the card lingers inline in the transcript (the
// completed card at the last tick, the discarded card at the discard marker,
// with red Xs on the items that never ran).
function TaskListCard({ tasks, discarded = false }: { tasks: TaskItem[]; discarded?: boolean }) {
  if (tasks.length === 0) return null;
  const done = tasks.filter((task) => task.done).length;
  return (
    <div className="task-card">
      <div className="task-card-head">
        <span className="eyebrow">checklist</span>
        <span className="task-card-count">{done}/{tasks.length}</span>
      </div>
      <ul className="task-card-list">
        {tasks.map((task, index) => (
          <li key={index} className={`task-item ${task.done ? "done" : discarded ? "discarded" : ""}`}>
            <span className="task-item-box">{task.done ? <Check size={11} /> : discarded ? <X size={11} /> : null}</span>
            <span className="task-item-text">{task.text}</span>
          </li>
        ))}
      </ul>
    </div>
  );
}

// Per-message run details (info indicator under the message): model, profile,
// token usage, duration, and status for the specific run that produced the
// turn, resolved from the session's event ledger.
function MessageInfoPanel({ run }: { run: HarnessRun }) {
  const started = timestampMs(run.startedAt);
  const finished = timestampMs(run.finishedAt);
  const durationSec = Number.isFinite(started) && Number.isFinite(finished) && finished > started
    ? (finished - started) / 1000
    : undefined;
  const rows: [string, string][] = [
    ["model", run.model || "unknown"],
    ["profile", run.profile + (run.profileRevision ? ` (rev ${run.profileRevision})` : "")],
    ["status", run.status],
  ];
  if (run.metrics.inputTokens > 0) rows.push(["prompt tokens", String(run.metrics.inputTokens)]);
  if (run.metrics.outputTokens > 0) rows.push(["completion tokens", String(run.metrics.outputTokens)]);
  if (run.metrics.providerCalls && run.metrics.providerCalls > 1) {
    rows.push(["provider calls", String(run.metrics.providerCalls)]);
    if (run.metrics.totalInputTokens) rows.push(["total prompt tokens", String(run.metrics.totalInputTokens)]);
    if (run.metrics.totalOutputTokens) rows.push(["total completion tokens", String(run.metrics.totalOutputTokens)]);
  }
  if (durationSec !== undefined) rows.push(["duration", `${durationSec.toFixed(1)} s`]);
  if (durationSec && run.metrics.outputTokens > 0) {
    rows.push(["output speed", `${(run.metrics.outputTokens / durationSec).toFixed(1)} tok/s`]);
  }
  if (run.error) rows.push(["error", run.error]);
  return (
    <div className="message-info" role="status">
      {rows.map(([label, value]) => (
        <div key={label} className="message-info-row"><span>{label}</span><strong>{value}</strong></div>
      ))}
    </div>
  );
}

function referenceDomain(url: string): string {
  try {
    return new URL(url).hostname.replace(/^www\./, "");
  } catch {
    return "";
  }
}

// Compact token counts for the per-message strip (1.2k / 32k, not 1,234).
function formatCompact(value: number): string {
  if (value >= 1e6) return `${(value / 1e6).toFixed(1)}m`;
  if (value >= 1e3) return `${(value / 1e3).toFixed(1)}k`;
  return String(value);
}

// One-line per-message metrics: context pressure, in/out
// tokens, elapsed, throughput, and time-to-first-token. Empty string when
// the run carried no usage data; the strip then stays out of the meta row.
function metricsStripText(run: HarnessRun): string {
  const parts: string[] = [];
  const m = run.metrics;
  if (m.contextWindow > 0 && m.contextUsed > 0) {
    parts.push(`ctx ${formatCompact(m.contextUsed)}/${formatCompact(m.contextWindow)}`);
  }
  const input = m.totalInputTokens || m.inputTokens;
  const output = m.totalOutputTokens || m.outputTokens;
  if (input > 0) parts.push(`${formatCompact(input)} in`);
  if (output > 0) parts.push(`${formatCompact(output)} out`);
  const started = timestampMs(run.startedAt);
  const finished = timestampMs(run.finishedAt);
  if (Number.isFinite(started) && Number.isFinite(finished) && finished > started) {
    const sec = (finished - started) / 1000;
    parts.push(`${sec.toFixed(1)} s`);
    if (output > 0) parts.push(`${(output / sec).toFixed(0)} t/s`);
  }
  if (m.ttftMs !== undefined) parts.push(`ttft ${(m.ttftMs / 1000).toFixed(1)} s`);
  return parts.join(" · ");
}

// Trigger a client-side download of a text document (blob + object URL).
function downloadText(filename: string, content: string, mime: string): void {
  const blob = new Blob([content], { type: mime });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = filename;
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
  window.setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function sessionFileName(session: HarnessSnapshot["session"], ext: string): string {
  const stamp = new Date().toISOString().slice(0, 19).replace(/[:T]/g, "-");
  const slug = (session.title || "conversation").toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 40) || "conversation";
  return `${slug}-${stamp}.${ext}`;
}

// Markdown transcript of the active path: per-turn headings with timestamp,
// model, and output tokens. The full event ledger stays in the JSON export.
function exportMarkdown(snapshot: HarnessSnapshot): string {
  const runs = runsFromEvents(snapshot.events, snapshot.session.id);
  const lines: string[] = [
    `# ${snapshot.session.title || "conversation"}`,
    "",
    `- session: ${snapshot.session.id}`,
    `- profile: ${snapshot.session.profile}`,
    `- model: ${snapshot.session.model}`,
    `- exported: ${new Date().toISOString()}`,
    "",
  ];
  for (const message of getActivePath(snapshot.messages)) {
    lines.push(`## ${message.role}`);
    lines.push("");
    const meta: string[] = [new Date(timestampMs(message.createdAt)).toISOString()];
    if (message.runId) {
      const run = runs.get(message.runId);
      if (run) {
        meta.push(run.model || "unknown model");
        if (run.metrics.outputTokens > 0) meta.push(`${run.metrics.outputTokens} out tokens`);
        if (run.status !== "completed") meta.push(`(${run.status})`);
      }
    }
    lines.push(`> ${meta.join(" ")}`);
    lines.push("");
    lines.push(message.content || "");
    lines.push("");
  }
  return lines.join("\n");
}

// CSV of the event ledger: one row per event, payload as an escaped JSON
// column (RFC 4180 quoting).
function exportEventsCsv(events: HarnessEvent[]): string {
  const escape = (value: string) => `"${value.replace(/"/g, '""')}"`;
  const rows = ["sequence,type,run_id,session_id,emitted_at,payload"];
  for (const event of events) {
    rows.push([
      String(event.sequence),
      escape(event.type),
      escape(event.runId),
      escape(event.sessionId),
      escape(event.emittedAt),
      escape(JSON.stringify(event.payload)),
    ].join(","));
  }
  return rows.join("\n");
}

// Hash of the event window's non-delta event ids. The window's array identity
// changes on every streamed delta, but several derivations (interrupted runs,
// run details, tasks, pending actions) only read non-delta events; gating
// their memo on this hash keeps them - and every TurnBlock prop they feed -
// identity-stable across a reasoning-delta burst.
function ledgerSignature(events: HarnessEvent[]): number {
  let hash = 0;
  for (const event of events) {
    if (event.type.endsWith(".delta")) continue;
    for (let i = 0; i < event.eventId.length; i++) hash = ((hash * 31) + event.eventId.charCodeAt(i)) | 0;
  }
  return hash;
}

// Batch window for stream events. Prod React is cheap enough for ~30 fps
// updates; the dev build allocates several times more per render and balloons
// the renderer's off-heap high-water mark, so it keeps the conservative
// window. Deltas arrive slower than a frame, so neither window waits on rAF.
const FLUSH_MS = import.meta.env.PROD ? 32 : 200;

function useStableDerived<T>(sig: number, compute: () => T): T {
  // eslint-disable-next-line react-hooks/exhaustive-deps -- compute is gated by sig
  return useMemo(() => compute(), [sig]);
}

function hashString(text: string): number {
  let hash = 5381;
  for (let i = 0; i < text.length; i++) {
    hash = ((hash << 5) + hash + text.charCodeAt(i)) | 0;
  }
  return Math.abs(hash);
}

// Stable-identity wrapper for handlers that App defines as plain functions:
// the wrapper keeps one identity for the component's lifetime while always
// calling the latest closure (the memoized children's props stay equal).
function useEventCallback<A extends unknown[], R>(fn: (...args: A) => R): (...args: A) => R {
  const ref = useRef(fn);
  useLayoutEffect(() => {
    ref.current = fn;
  });
  return useCallback((...args: A) => ref.current(...args), []);
}

// Content-addressed derivation cache, shared module-level like the markdown
// component tables. Keyed by session + message + a signature of (run id, text
// lengths, that run's event count), so a delta re-derives only the streaming
// turn; every other turn keeps its cached object identity, which the
// TurnBlock memo keys on. Cleared wholesale when it grows stale.
const messageDeriveCache = new Map<string, { blocks?: RunBlock[]; segments?: string[] }>();

function derivedForMessage(
  sessionId: string,
  message: HarnessMessage,
  events: HarnessEvent[],
  runEventCounts: Map<string, number>,
): { blocks?: RunBlock[]; segments?: string[] } {
  const runId = message.runId || "";
  const isAssistant = message.role === "assistant" && !!runId;
  const key = `${sessionId}|${message.id}|${runId}|${(message.reasoning || "").length}|${(message.content || "").length}|${isAssistant ? runEventCounts.get(runId) || 0 : 0}`;
  const cached = messageDeriveCache.get(key);
  if (cached) return cached;
  const entry = {
    blocks: isAssistant ? runBlocksFromEvents(events, runId, message.reasoning || "", message.content || "") : undefined,
    segments: isAssistant ? reasoningSegmentsForRun(events, runId, message.reasoning || "") : undefined,
  };
  if (messageDeriveCache.size > 256) messageDeriveCache.clear();
  messageDeriveCache.set(key, entry);
  return entry;
}

const TurnBlock = memo(function TurnBlock({
  message,
  allMessages,
  profileName,
  speakerName,
  onSelectIteration,
  onSelectMessage,
  onRegenerate,
  onEdit,
  onDelete,
  onRewindHere,
  canRewind,
  canRegenerate,
  interruptedRuns,
  runInfo,
  reasoningSegments,
  blocks,
  activityRun,
  onApproval,
  onAnswerInput,
  onAnswerSudo,
  sources,
  tasks,
  taskRecords,
  continued,
  subagent,
}: {
  message: HarnessMessage;
  allMessages: HarnessMessage[];
  profileName?: string;
  // Group chat: the speaker display name for a group turn's message (the
  // session's nominal profile name is meaningless for a speaker).
  speakerName?: string;
  // Resume-after-error: this turn continues the run that failed before it
  // (run.continued - renders without a repeated profile marker/header,
  // inside the interrupted turn's visual continuity).
  continued?: boolean;
  onSelectIteration: (runId: string) => void;
  onSelectMessage: (messageId: number) => void;
  onRegenerate?: (runId: string) => void;
  onEdit?: (message: HarnessMessage, content: string) => void;
  onDelete?: (message: HarnessMessage) => void;
  onRewindHere?: (message: HarnessMessage) => void;
  canRewind: boolean;
  canRegenerate: boolean;
  interruptedRuns: Map<string, { kind: "failed" | "cancelled"; error: string; errorKind?: string; errorHint?: string }>;
  runInfo?: Map<string, HarnessRun>;
  reasoningSegments?: string[];
  blocks?: RunBlock[];
  activityRun?: HarnessRun;
  onApproval: (decision: "allow_once" | "allow_turn" | "allow_session" | "allow_always" | "deny") => void;
  onAnswerInput?: (choice: number) => void;
  onAnswerSudo?: (password: string, scope: string) => void;
  sources?: SourceInfo[];
  tasks?: TaskItem[];
  taskRecords?: TaskChecklistRecord[];
  subagent?: {
    events: HarnessEvent[];
    client: HarnessClient;
    onOpenSession?: (sessionId: string) => void;
  };
}) {
  const [confirmRewind, setConfirmRewind] = useState(false);
  const [infoOpen, setInfoOpen] = useState(false);
  const [overlayIndex, setOverlayIndex] = useState<number | null>(null);
  const [copied, setCopied] = useState(false);
  const rewindTimerRef = useRef<number | undefined>(undefined);
  const copyTimerRef = useRef<number | undefined>(undefined);
  const requestRewind = () => {
    if (!canRewind) return;
    if (confirmRewind) {
      window.clearTimeout(rewindTimerRef.current);
      setConfirmRewind(false);
      onRewindHere?.(message);
      return;
    }
    setConfirmRewind(true);
    window.clearTimeout(rewindTimerRef.current);
    rewindTimerRef.current = window.setTimeout(() => setConfirmRewind(false), 2500);
  };
  if (message.role === "user" || message.role === "system") {
    const siblings = message.role === "user" ? userSiblings(allMessages, message) : [];
    // A user message with no assistant reply (its run failed, or the reply was
    // trimmed) has nothing to regenerate on the model side: offer to resend the
    // message itself. The daemon re-runs it against the same message id.
    const hasReply = allMessages.some((m) => m.role === "assistant" && m.parentId === message.id);
    return (
      <MessageBlock
        message={message}
        onEdit={onEdit}
        onDelete={onDelete}
        onRegenerate={hasReply ? undefined : onRegenerate}
        canRegenerate={canRegenerate}
        branchNav={
          siblings.length > 1 ? (
            <BranchNav siblings={siblings} activeId={message.id} onSelect={onSelectMessage} label="prompt" />
          ) : undefined
        }
      />
    );
  }
  const siblings = assistantSiblings(allMessages, message);
  const count = siblings.length;
  const active = siblings[Math.max(0, siblings.findIndex((s) => s.id === message.id))];
  const interruption = active.runId ? interruptedRuns.get(active.runId) : undefined;
  // Inline error grammar in the failed turn: the decode (kind + one hint)
  // is attached where the work is, so the reader sees why here - the
  // ErrorBar below the chat keeps the action buttons.
  const failureGrammar = interruption && interruption.error
    ? errorGrammar(interruption.error, interruption.errorKind, interruption.errorHint)
    : null;
  const hasResponse = active.content.length > 0 || !!interruption;
  const segments = reasoningSegments ?? (active.reasoning ? [active.reasoning] : []);
  const hasBlocks = !!blocks && blocks.length > 0;
  const hasTextBlock = blocks?.some((block) => block.kind === "text") ?? false;
  // Collapsed by default: the trace is for inspection, not the reading path.
  const renderReasoning = (segment: string, index: number) => segment ? (
    <details className="message-reasoning" key={`reasoning-${index}`}>
      <summary>
        <BrainCircuit size={13} />
        reasoning trace
        <ChevronDown size={13} />
      </summary>
      <p>{segment}</p>
    </details>
  ) : null;
  const runDetails = active.runId ? runInfo?.get(active.runId) : undefined;
  const metricsStrip = runDetails ? metricsStripText(runDetails) : "";
  // References section: only the sources this reply actually cited are
  // listed. src:N ids are per-session sequential and stable, so the mapping
  // holds for older turns and iterations alike. Cheap regex over one message,
  // computed inline (not a hook) to avoid the early-return ordering rule.
  let citedSources: SourceInfo[] = [];
  if (sources?.length) {
    const ids = new Set<number>();
    const re = /\[src:(\d+)\]/g;
    let match: RegExpExecArray | null;
    while ((match = re.exec(active.content)) !== null) ids.add(Number(match[1]));
    citedSources = sources.filter((source) => ids.has(source.id)).sort((a, b) => a.id - b.id);
  }
  const citationTarget = citedSources.length > 0 ? `references-${active.id}` : undefined;
  const copyResponse = async () => {
    try {
      await copyText(active.content);
      setCopied(true);
      window.clearTimeout(copyTimerRef.current);
      copyTimerRef.current = window.setTimeout(() => setCopied(false), 1600);
    } catch {
      // Clipboard unavailable (non-secure context); manual selection still works.
    }
  };
  // The closed-checklist card anchored at a given task event, if any: a
  // completed list at its last tick, a discarded list at the discard marker.
  const taskCardAt = (eventId: string) => taskRecords?.find((record) => record.anchor === eventId);
  const speakerLabel = speakerName || profileName || "nyaxus";
  // Group transcripts need per-speaker identity: a hash-hue accent on the
  // marker only (initials stay), so mid-round attribution is instant.
  // Ordinary sessions keep the shared look.
  const markerAccent = speakerName
    ? (() => {
        let hash = 5381;
        for (let i = 0; i < speakerName.length; i++) {
          hash = ((hash << 5) + hash + speakerName.charCodeAt(i)) | 0;
        }
        const hue = Math.abs(hash) % 360;
        return {
          background: `hsl(${hue} 30% 91%)`,
          borderColor: `hsl(${hue} 38% 60%)`,
          color: `hsl(${hue} 45% 32%)`,
        };
      })()
    : undefined;
  return (
      <article className={`message-block message-agent ${continued ? "message-continued" : ""}`}>
      {/* The marker stays rendered for continued turns (CSS ghosts it with
          visibility:hidden) so the continuation blocks keep the same left
          gutter as every other turn - omitting it entirely misaligned the
          resumed subtree against the rest of the transcript. */}
      <div className={`message-marker ${continued ? " message-continued-marker" : ""}`} style={continued ? undefined : markerAccent}>{speakerLabel.slice(0, 2).toLowerCase()}</div>
      <div className="message-main">
        <div className="message-meta">
          {continued ? <span className="message-continued-tag">resumed after error</span> : (
            <>
          <strong>{speakerLabel}</strong>
          <span>{formatTime(active.createdAt)}</span>
            </>
          )}
          {active.runId && <span className="message-run">{active.runId.slice(-8)}</span>}
          {metricsStrip && <span className="message-metrics" title="run metrics: context, tokens, duration, throughput">{metricsStrip}</span>}
          {runDetails && (
            <button type="button" className="message-info-toggle" onClick={() => setInfoOpen((open) => !open)} aria-expanded={infoOpen} title="run details">
              <Info size={12} />
            </button>
          )}
        </div>
        {runDetails && infoOpen && <MessageInfoPanel run={runDetails} />}
        {active.attachments && <AttachmentPills attachments={active.attachments} variant="message" onOpen={setOverlayIndex} />}
        {hasBlocks && blocks ? (
          <>
            {blocks.map((block, index) =>
              block.kind === "reasoning" ? (
                renderReasoning(block.text, index)
              ) : block.kind === "tools" ? (
                // The live run only anchors the newest message; earlier runs'
                // tool blocks must render from their reconstructed event
                // state, or they vanish the moment a new run starts.
                <ActivityGroup key={`tools-${index}`} run={activityRun ?? runDetails ?? null} toolIds={block.toolIds} onApproval={onApproval} onAnswerInput={onAnswerInput} onAnswerSudo={onAnswerSudo} subagent={subagent} />
              ) : block.kind === "task_created" ? (
                <Fragment key={`task-${index}`}>
                  <div className="task-indicator">
                    <ListChecks size={11} />
                    <span>{block.count === 0 ? "checklist discarded" : block.count === 1 ? "checklist · 1 item" : `checklist · ${block.count} items`}</span>
                  </div>
                  {/* A discard anchors its card here (at the discard marker),
                      with red Xs on the items that never ran. */}
                  {(() => { const record = taskCardAt(block.eventId); return record ? <TaskListCard tasks={record.tasks} discarded={record.kind === "discarded"} /> : null; })()}
                </Fragment>
              ) : block.kind === "task_updated" ? (
                <Fragment key={`task-${index}`}>
                  <div className={`task-indicator ${block.status === "done" ? "task-indicator-done" : ""}`}>
                    {block.status === "done" ? <Check size={11} /> : <RotateCcw size={11} />}
                    {/* Text resolves against the current replayed list; if the
                        list was later replaced, the marker falls back to the
                        index (the historical text is not in the event). */}
                    <span>{tasks && block.index >= 1 && block.index <= tasks.length ? tasks[block.index - 1].text : `task ${block.index}`}</span>
                  </div>
                  {/* The completed checklist lingers where the last tick
                      landed, not pinned at the bottom: the work's location
                      is the record's location. */}
                  {(() => { const record = taskCardAt(block.eventId); return record ? <TaskListCard tasks={record.tasks} discarded={record.kind === "discarded"} /> : null; })()}
                </Fragment>
              ) : (
                <div key={`text-${index}`} className="message-body markdown-body">
                  <MarkdownContent content={block.text} citationTarget={citationTarget} />
                </div>
              ),
            )}
            {hasTextBlock && interruption && (
              <p className="message-interrupted">
                {interruption.kind === "failed" ? "[response failed]" : "[response interrupted]"}
                {failureGrammar?.title && <span className="message-interrupted-why"> {failureGrammar.title}</span>}
                {failureGrammar?.hint && <span className="message-interrupted-hint"> · {failureGrammar.hint}</span>}
              </p>
            )}
          </>
        ) : activityRun ? (
          <>
            {renderReasoning(segments[0] || "", 0)}
            <ActivityGroup run={activityRun} onApproval={onApproval} onAnswerInput={onAnswerInput} onAnswerSudo={onAnswerSudo} subagent={subagent} />
            {segments.slice(1).map((segment, index) => renderReasoning(segment, index + 1))}
          </>
        ) : segments.map((segment, index) => renderReasoning(segment, index))}
        {hasResponse && !hasTextBlock && (
          <div className="message-body markdown-body">
            <MarkdownContent content={active.content} citationTarget={citationTarget} />
            {interruption && (
              <span className="message-interrupted message-interrupted-inline">
                {interruption.kind === "failed" ? "[response failed]" : "[response interrupted]"}
                {failureGrammar?.title && <span className="message-interrupted-why"> {failureGrammar.title}</span>}
                {failureGrammar?.hint && <span className="message-interrupted-hint"> · {failureGrammar.hint}</span>}
              </span>
            )}
          </div>
        )}
        {hasResponse && (
          <div className="message-actions">
            {count > 1 && (
              <BranchNav
                siblings={siblings}
                activeId={active.id}
                onSelect={(id) => {
                  const target = siblings.find((s) => s.id === id);
                  if (target?.runId) onSelectIteration(target.runId);
                }}
                label="answer"
              />
            )}
            {active.content && (
              <button className="regenerate-button" onClick={() => void copyResponse()} title="copy the raw markdown source">
                {copied ? <Check size={12} /> : <Copy size={12} />}
                {copied ? "copied" : "copy"}
              </button>
            )}
            {active.runId && onRegenerate && (
              <button
                className="regenerate-button"
                onClick={() => onRegenerate(active.runId!)}
                disabled={!canRegenerate}
                title={canRegenerate ? "generate a fresh answer for this turn" : "stop the active run before regenerating"}
              >
                <RefreshCw size={12} />
                regenerate
              </button>
            )}
            {onRewindHere && (
              <button
                className={`regenerate-button ${confirmRewind ? "branch-chip-active" : ""}`}
                onClick={requestRewind}
                disabled={!canRewind}
                title={canRewind ? "permanently delete every message and branch after this point" : "stop the active run before trimming the conversation"}
              >
                <History size={12} />
                {confirmRewind ? "trim after this?" : "rewind here"}
              </button>
            )}
          </div>
        )}
        {citedSources.length > 0 && (
          <div className="references-section" id={citationTarget}>
            <div className="references-heading">
              references <span>{citedSources.length} total</span>
            </div>
            {citedSources.map((source) => {
              const domain = referenceDomain(source.url);
              return (
                <div className="reference-row" key={source.id} id={`citation-${active.id}-${source.id}`}>
                  <span className="reference-badge">src:{source.id}</span>
                  {source.url ? (
                    <a className="reference-title" href={source.url} target="_blank" rel="noreferrer">
                      {source.title || source.url}
                    </a>
                  ) : (
                    <span className="reference-title">{source.title || "untitled"}</span>
                  )}
                  {domain && <span className="reference-domain">{domain}</span>}
                </div>
              );
            })}
          </div>
        )}
      </div>
      <AttachmentOverlay
        attachments={active.attachments || []}
        index={overlayIndex}
        onClose={() => setOverlayIndex(null)}
        onSelect={setOverlayIndex}
      />
    </article>
  );
}, (previous, next) =>
  previous.message === next.message
  && previous.allMessages.length === next.allMessages.length
  && previous.profileName === next.profileName
  && previous.speakerName === next.speakerName
  && previous.canRewind === next.canRewind
  && previous.canRegenerate === next.canRegenerate
  && previous.interruptedRuns === next.interruptedRuns
  && previous.runInfo === next.runInfo
  && previous.reasoningSegments === next.reasoningSegments
  && previous.blocks === next.blocks
  && previous.activityRun === next.activityRun
  && previous.onApproval === next.onApproval
  && previous.onAnswerInput === next.onAnswerInput
  && previous.onAnswerSudo === next.onAnswerSudo
  && previous.onSelectIteration === next.onSelectIteration
  && previous.onSelectMessage === next.onSelectMessage
  && previous.onRegenerate === next.onRegenerate
  && previous.onEdit === next.onEdit
  && previous.onDelete === next.onDelete
  && previous.onRewindHere === next.onRewindHere
  && previous.sources === next.sources
  && previous.tasks === next.tasks
  && previous.taskRecords === next.taskRecords
  && previous.subagent === next.subagent
);

// Line diff for the prompt.edit approval: the daemon ships the module's
// current text in the approval payload, so the diff is computed locally
// (old -> new). Modules are small, so a plain LCS pass is fine.
function diffLines(oldText: string, newText: string): { sign: " " | "-" | "+"; text: string }[] {
  const a = oldText.split("\n");
  const b = newText.split("\n");
  const n = a.length;
  const m = b.length;
  const dp: number[][] = Array.from({ length: n + 1 }, () => new Array<number>(m + 1).fill(0));
  for (let i = n - 1; i >= 0; i--) {
    for (let j = m - 1; j >= 0; j--) {
      dp[i][j] = a[i] === b[j] ? dp[i + 1][j + 1] + 1 : Math.max(dp[i + 1][j], dp[i][j + 1]);
    }
  }
  const out: { sign: " " | "-" | "+"; text: string }[] = [];
  let i = 0;
  let j = 0;
  while (i < n && j < m) {
    if (a[i] === b[j]) {
      out.push({ sign: " ", text: a[i] });
      i++;
      j++;
    } else if (dp[i + 1][j] >= dp[i][j + 1]) {
      out.push({ sign: "-", text: a[i] });
      i++;
    } else {
      out.push({ sign: "+", text: b[j] });
      j++;
    }
  }
  while (i < n) out.push({ sign: "-", text: a[i++] });
  while (j < m) out.push({ sign: "+", text: b[j++] });
  return out;
}

function PromptEditApproval({ tool, approval }: {
  tool: HarnessTool;
  approval: HarnessRun["approval"];
}) {
  let path = "";
  let reason = "";
  let next = "";
  try {
    const args = JSON.parse(tool.arguments || "{}");
    path = String(args.path || "");
    reason = String(args.reason || "");
    next = String(args.content || "");
  } catch {
    // Malformed arguments: fall back to the raw input shown above.
  }
  const oldText = approval?.oldContent || "";
  const lines = oldText ? diffLines(oldText, next) : [];
  return (
    <div className="prompt-edit-approval">
      {path && <div className="pe-path">{path}</div>}
      {reason && (
        <div className="pe-reason">
          <label>reason</label>
          <span>{reason}</span>
        </div>
      )}
      {lines.length > 0 ? (
        <pre className="pe-diff">
          {lines.map((line, index) => (
            <span
              key={index}
              className={"pe-line " + (line.sign === "+" ? "pe-add" : line.sign === "-" ? "pe-del" : "")}
            >
              {(line.sign === " " ? " " : line.sign) + " " + line.text}
            </span>
          ))}
        </pre>
      ) : (
        <div className="pe-nodiff">no current text available - review the proposed content in the input above</div>
      )}
    </div>
  );
}

function InputCard({ input, onAnswer, onAnswerSudo }: {
  input: NonNullable<HarnessRun["input"]>;
  onAnswer: (choice: number) => void;
  onAnswerSudo?: (password: string, scope: string) => void;
}) {
  // The sudo password gate (docs/sudo-gate.md): a password field (masked)
  // plus the scope the grant lives for. The password leaves the browser
  // only through the authorized run-command channel and is never rendered,
  // logged, or ledgered.
  if (input.kind === "sudo") {
    return <SudoInputCard input={input} onAnswer={onAnswerSudo} />;
  }
  return (
    <div className="tool-input">
      <span className="tool-input-eyebrow">{input.kind === "quiz" ? "quiz" : "question"}</span>
      <p className="tool-input-question">{input.question}</p>
      <div className="tool-input-options">
        {input.options.map((option, index) => (
          <button type="button" key={index} onClick={() => onAnswer(index)}>{option}</button>
        ))}
        {input.hasDontKnow && (
          <button type="button" className="tool-input-dontknow" onClick={() => onAnswer(-1)}>I don't know</button>
        )}
      </div>
    </div>
  );
}

function SudoInputCard({ input, onAnswer }: {
  input: NonNullable<HarnessRun["input"]>;
  onAnswer?: (password: string, scope: string) => void;
}) {
  const [password, setPassword] = useState("");
  const scopes: { id: string; label: string }[] = [
    { id: "once", label: "this command" },
    { id: "ttl", label: "for 15 minutes" },
    { id: "session", label: "this session" },
  ];
  const [scope, setScope] = useState("once");
  const submit = () => {
    if (password.trim() && onAnswer) onAnswer(password, scope);
  };
  return (
    <div className="tool-input">
      <span className="tool-input-eyebrow">sudo password</span>
      <p className="tool-input-question">{input.question}</p>
      {input.command && <pre className="tool-input-cmd">{input.command}</pre>}
      <div className="tool-input-inline">
        <input
          type="password"
          className="tool-input-password"
          placeholder="sudo password"
          value={password}
          disabled={!onAnswer}
          onChange={(event) => setPassword(event.target.value)}
          onKeyDown={(event) => {
            if (event.key === "Enter") {
              event.preventDefault();
              submit();
            }
          }}
          autoFocus
        />
      </div>
      <div className="tool-input-options">
        {scopes.map((option) => (
          <button type="button" key={option.id} className={scope === option.id ? "tool-input-scope-on" : ""} onClick={() => setScope(option.id)}>{option.label}</button>
        ))}
        <button type="button" className="tool-input-scope-submit" disabled={!password.trim()} onClick={submit}>run with sudo</button>
      </div>
    </div>
  );
}

// Inline subagent view (agent.spawn): the worker's conversation
// renders inside the spawn tool row, so worker sessions stay visible only
// where they happened (the sidebar hides them). While the spawn is running,
// the block loads the child session and streams its OWN event stream (a
// second EventSource), rendering the worker live; after completion it shows
// the harvested narrative. "Open full session" jumps to the worker session,
// which is fully live there.
function SubagentBlock({ tool, runId, spawnIndex, events, client, onOpenSession }: {
  tool: HarnessTool;
  runId: string;
  spawnIndex: number;
  events: HarnessEvent[];
  client: HarnessClient;
  onOpenSession?: (sessionId: string) => void;
}) {
  let profile = "";
  let objective = "";
  try {
    const args = JSON.parse(tool.arguments || "{}") as Record<string, unknown>;
    profile = String(args.profile || "");
    objective = String(args.objective || "");
  } catch {
    // Non-JSON arguments: the block still renders with what it has.
  }
  const parsed = parseSpawnResult(tool.result || "");
  const running = tool.status === "running";
  // While running, the tool row carries no child id: the Nth agent.spawn row
  // in this run maps to the Nth worker.started event (the payload carries
  // child_session_id). After completion the tool result carries the exact id.
  const liveChildId = useMemo(() => {
    if (!running) return "";
    const started = events.filter((event) => event.type === "worker.started" && event.runId === runId);
    const event = started[spawnIndex];
    return event ? String(event.payload.child_session_id || "") : "";
  }, [running, events, runId, spawnIndex]);
  const childSessionId = parsed?.childSessionId || liveChildId;

  const [childEvents, setChildEvents] = useState<HarnessEvent[]>([]);
  useEffect(() => {
    if (!running || !childSessionId) return;
    let stop: (() => void) | null = null;
    let disposed = false;
    void client.loadSession(childSessionId).then((snapshot) => {
      if (disposed) return;
      setChildEvents(snapshot.events);
      stop = client.streamSession(childSessionId, (event) => {
        setChildEvents((previous) => previous.some((existing) => existing.eventId === event.eventId)
          ? previous
          : [...previous, event]);
      }, () => {
        // Reconnects replay from the loadSession cursor; the eventId dedupe
        // above absorbs the overlap.
      });
    });
    return () => {
      disposed = true;
      if (stop) stop();
      setChildEvents([]);
    };
  }, [running, childSessionId, client]);

  const childRun = useMemo(() => {
    if (!running || childEvents.length === 0) return null;
    const runs = runsFromEvents(childEvents, childSessionId);
    return runs.size > 0 ? Array.from(runs.values())[0] : null;
  }, [running, childEvents, childSessionId]);
  const childBlocks = useMemo(
    () => (childRun ? runBlocksFromEvents(childEvents, childRun.id) : []),
    [childRun, childEvents],
  );

  const status = running ? "working" : parsed?.status || (tool.status === "failed" ? "failed" : "completed");
  return (
    <div className={`subagent-block subagent-${status}`}>
      <div className="subagent-head">
        <span className="subagent-title"><Bot size={12} /> worker{profile ? ` · ${profile}` : ""}</span>
        <span className="subagent-status">
          {status === "working" ? <><span className="pulse-ring" /> working</>
            : status === "completed" ? <><Check size={11} /> completed</>
            : <><X size={11} /> {status}</>}
        </span>
        {childSessionId && onOpenSession && (
          <button type="button" className="subagent-open" onClick={() => onOpenSession(childSessionId)}>
            open full session <ArrowUpRight size={11} />
          </button>
        )}
      </div>
      {objective && (
        <div className="subagent-section">
          <span className="subagent-label">task</span>
          <p className="subagent-text">{objective}</p>
        </div>
      )}
      <div className="subagent-divider"><span>subagent output</span></div>
      {running ? (
        childRun ? (
          <div className="subagent-live">
            {childBlocks.map((block, index) =>
              block.kind === "reasoning" ? (
                <details key={`r-${index}`} className="message-reasoning">
                  <summary><BrainCircuit size={13} /> reasoning trace <ChevronDown size={13} /></summary>
                  <p>{block.text}</p>
                </details>
              ) : block.kind === "text" ? (
                <div key={`t-${index}`} className="subagent-live-text markdown-body"><MarkdownContent content={block.text} /></div>
              ) : block.kind === "tools" ? (
                <div key={`tools-${index}`} className="subagent-tools">
                  {block.toolIds.map((id) => {
                    const childTool = childRun.tools.find((entry) => entry.id === id);
                    if (!childTool) return null;
                    return (
                      <div key={id} className={`subagent-tool subagent-tool-${childTool.status}`}>
                        <span className="subagent-tool-state">
                          {childTool.status === "completed" ? <Check size={11} /> : childTool.status === "failed" ? <X size={11} /> : <LoaderCircle size={11} className="spin" />}
                        </span>
                        <span>{childTool.name}</span>
                        <span className="subagent-tool-target">{childTool.target}</span>
                      </div>
                    );
                  })}
                </div>
              ) : null,
            )}
            {childRun.approval && (
              <p className="subagent-hint">worker is waiting for approval: {childRun.approval.name} - open the full session to approve.</p>
            )}
            {childRun.input && (
              <p className="subagent-hint">worker is waiting for an answer - open the full session to respond.</p>
            )}
          </div>
        ) : (
          <p className="subagent-text subagent-muted">{childSessionId ? "connecting to the worker…" : "starting the worker…"}</p>
        )
      ) : parsed && parsed.narrative ? (
        <div className="subagent-narrative markdown-body"><MarkdownContent content={parsed.narrative} /></div>
      ) : (
        <p className="subagent-text subagent-muted">(no output)</p>
      )}
    </div>
  );
}

function formatToolDuration(ms: number): string {
  if (ms < 1000) return `${ms} ms`;
  const seconds = ms / 1000;
  if (seconds < 60) return `${seconds.toFixed(1)} s`;
  const minutes = Math.floor(seconds / 60);
  const rest = Math.round(seconds % 60);
  return `${minutes}m ${rest}s`;
}

// Live elapsed timer for a running tool row: ticks once a second inside
// this subtree only (ToolRow is memoized on the tool object, so the
// interval re-renders the row, not the conversation).
function ToolTimer({ startedAt }: { startedAt: string }) {
  const started = startedAt ? Date.parse(startedAt) : 0;
  const [elapsed, setElapsed] = useState(() =>
    started ? Math.max(0, Date.now() - started) : 0);
  useEffect(() => {
    if (!started) return;
    const timer = window.setInterval(
      () => setElapsed(Math.max(0, Date.now() - started)),
      1000);
    return () => window.clearInterval(timer);
  }, [started]);
  if (!started) return <>running</>;
  return <>running · {formatToolDuration(elapsed)}</>;
}
const ToolRow = memo(function ToolRow({
  tool,
  approval,
  input,
  onApproval,
  onAnswerInput,
  onAnswerSudo,
  subagent,
}: {
  tool: HarnessTool;
  approval?: HarnessRun["approval"];
  input?: HarnessRun["input"];
  onApproval?: (decision: "allow_once" | "allow_turn" | "allow_session" | "allow_always" | "deny") => void;
  onAnswerInput?: (choice: number) => void;
  onAnswerSudo?: (password: string, scope: string) => void;
  subagent?: {
    runId: string;
    spawnIndex: number;
    events: HarnessEvent[];
    client: HarnessClient;
    onOpenSession?: (sessionId: string) => void;
  };
}) {
  const output = tool.result || tool.summary || "";
  // Auto-open while THIS row is live (running, awaiting approval, or owing
  // an input answer): a new call reveals itself without expanding its
  // siblings, and the row folds back the moment it completes. Manual
  // toggles stick until the open prop itself changes (React leaves the
  // untouched attribute alone), so completed rows can be inspected.
  const pending = tool.status === "running" || tool.status === "waiting_approval";
  return (
    <details className={`tool-row ${pending ? "tool-row-pending" : ""}`} open={pending}>
      <summary className="tool-row-head">
        <span className={`tool-state ${tool.status}`}>
          {tool.status === "completed" ? <Check size={11} /> : tool.status === "failed" ? <X size={11} /> : tool.status === "waiting_approval" ? <ShieldCheck size={11} /> : tool.status === "running" && input ? <HelpCircle size={11} /> : <span />}
        </span>
        <div className="tool-copy">
          <strong>{tool.name}</strong>
          <span>{tool.target}</span>
        </div>
        <span className="tool-duration">
          {tool.durationMs
            ? formatToolDuration(tool.durationMs)
            : tool.status === "running"
            ? (input ? "waiting for answer" : <ToolTimer startedAt={tool.startedAt || ""} />)
            : tool.status === "waiting_approval" ? "approval" : "--"}
        </span>
      </summary>
      <div className="tool-row-body">
        {tool.name === "agent.spawn" && subagent ? (
          <SubagentBlock
            tool={tool}
            runId={subagent.runId}
            spawnIndex={subagent.spawnIndex}
            events={subagent.events}
            client={subagent.client}
            onOpenSession={subagent.onOpenSession}
          />
        ) : (
          <>
            {tool.arguments && (
              <div className="tool-detail">
                <label>input</label>
                <pre>{tool.arguments}</pre>
              </div>
            )}
            {output && (
              <div className="tool-detail">
                <label>output</label>
                <pre>{output}</pre>
              </div>
            )}
          </>
        )}
        {tool.status === "waiting_approval" && approval && onApproval && (
          <div className="tool-approval">
            <span>approval required{approval.riskLevel
              ? ` · ${approval.riskLevel} risk${approval.sideEffectScope && approval.sideEffectScope !== "none" ? ` · ${approval.sideEffectScope}` : ""}`
              : ""}</span>
            {tool.name === "prompt.edit" && <PromptEditApproval tool={tool} approval={approval} />}
            <div className="approval-actions">
              <button type="button" onClick={() => onApproval("deny")} className="approval-deny">deny</button>
              <button type="button" onClick={() => onApproval("allow_once")} className="approval-allow">allow once</button>
              <button type="button" onClick={() => onApproval("allow_turn")} className="approval-turn" title={`allow every ${tool.name} call for the rest of this turn, with any arguments`}>allow for turn</button>
              <button type="button" onClick={() => onApproval("allow_session")} className="approval-session" title={`allow every ${tool.name} call for the rest of this session, with any arguments`}>allow for session</button>
              <button type="button" onClick={() => onApproval("allow_always")} className="approval-always" title={approval.patternAllowable && approval.fingerprint
                ? `always allow ${tool.name} matching: ${approval.fingerprint} - in every session, until removed in settings`
                : `allow every ${tool.name} call in every session, with any arguments, until removed in settings`}>always allow</button>
            </div>
          </div>
        )}
        {tool.status === "running" && input && onAnswerInput && (
          <InputCard input={input} onAnswer={onAnswerInput} onAnswerSudo={onAnswerSudo} />
        )}
      </div>
    </details>
  );
});

const ActivityGroup = memo(function ActivityGroup({
  run,
  toolIds,
  onApproval,
  onAnswerInput,
  onAnswerSudo,
  subagent,
}: {
  run: HarnessRun | null;
  toolIds?: string[];
  onApproval: (decision: "allow_once" | "allow_turn" | "allow_session" | "allow_always" | "deny") => void;
  onAnswerInput?: (choice: number) => void;
  onAnswerSudo?: (password: string, scope: string) => void;
  subagent?: {
    events: HarnessEvent[];
    client: HarnessClient;
    onOpenSession?: (sessionId: string) => void;
  };
}) {
  // Auto-open while any row is live; a user's manual collapse/expand wins
  // over the auto state (React only re-applies the open attribute when the
  // prop CHANGES, so without an override a new call's 0→pending transition
  // would re-open the group over a user collapse). The override resets when
  // the batch goes quiet, so the next batch re-enters the auto behavior. The
  // hooks
  // run unconditionally (before any early return), because a run can go
  // null ↔ non-null between renders.
  const tools = run
    ? toolIds
      ? run.tools.filter((tool) => toolIds.includes(tool.id))
      : run.tools
    : [];
  const active = tools.some((tool) => tool.status === "running" || tool.status === "waiting_approval");
  const [override, setOverride] = useState<boolean | null>(null);
  const wasActive = useRef(active);
  useEffect(() => {
    if (wasActive.current && !active) setOverride(null);
    wasActive.current = active;
  });
  if (!run || tools.length === 0) return null;
  const completed = tools.filter((tool) => tool.status === "completed").length;
  const failed = tools.filter((tool) => tool.status === "failed").length;
  const pending = tools.filter((tool) => tool.status === "running" || tool.status === "waiting_approval").length;
  const open = override !== null ? override : active;
  // Deduped name preview in the header; a group-level badge when any call
  // failed, so a failed member is visible without expanding.
  const names = [...new Set(tools.map((tool) => tool.name))];
  const namePreview = names.slice(0, 2).join(" · ") + (names.length > 2 ? ` +${names.length - 2}` : "");
  return (
    <details className="activity-group" open={open} onToggle={(event) => {
      // User clicks win over the auto state: record the resulting open as
      // the override; the quiet-batch effect above clears it.
      setOverride((event.target as HTMLDetailsElement).open);
    }}>
      <summary>
        <span className="summary-left">
          <Zap size={13} />
          {tools.length} tool {tools.length === 1 ? "call" : "calls"}
          <span className="summary-names">{namePreview}</span>
        </span>
        <span className="summary-right">
          {completed} complete
          {failed ? <span className="summary-failed"> · {failed} failed</span> : ""}
          {pending ? ` · ${pending} pending` : ""}
        </span>
      </summary>
      <div className="tool-list">
        {tools.map((tool) => (
          <ToolRow
            key={tool.id}
            tool={tool}
            approval={run.approval?.id === tool.id ? run.approval : undefined}
            input={run.input?.id === tool.id ? run.input : undefined}
            onApproval={onApproval}
            onAnswerInput={onAnswerInput}
            onAnswerSudo={onAnswerSudo}
            subagent={subagent && tool.name === "agent.spawn" ? {
              runId: run.id,
              // Nth spawn row in the run ↔ Nth worker.started event.
              spawnIndex: run.tools.filter((entry) => entry.name === "agent.spawn" && entry.id !== tool.id).length,
              events: subagent.events,
              client: subagent.client,
              onOpenSession: subagent.onOpenSession,
            } : undefined}
          />
        ))}
      </div>
    </details>
  );
});

function QueuedPrompts({ items, active, steeringIds, onEdit, onRemove, onSteer, onDeliverNow, onSendNow }: {
  items: QueuedPrompt[];
  active: boolean;
  steeringIds: ReadonlySet<string>;
  onEdit: (item: QueuedPrompt) => void;
  onRemove: (item: QueuedPrompt) => void;
  onSteer: (item: QueuedPrompt) => void;
  onDeliverNow: (item: QueuedPrompt) => void;
  onSendNow: () => void;
}) {
  if (items.length === 0) return null;
  return (
    <section className="queued-prompts" aria-label="Queued messages">
      <div className="queued-prompts-heading">
        <span className="queued-prompts-dot" />
        <span className="queued-prompts-label">queued for execution</span>
        <span className="queued-prompts-count">{items.length}</span>
      </div>
      {items.map((item, index) => {
        const steering = steeringIds.has(item.id);
        return (
        <div className={`queued-prompt${steering ? " steering" : ""}`} key={item.id}>
          <span className="queued-prompt-index">{index + 1}</span>
          <div className="queued-prompt-main">
            <span className="queued-prompt-content">{item.content || "attachment-only prompt"}</span>
            {steering && <small className="queued-prompt-hint">the current turn ends at its next pause - this message takes the slot</small>}
          </div>
          <span className={steering ? "queued-prompt-state steering" : "queued-prompt-state"}>{steering ? "steering" : "waiting"}</span>
          {active && (
            <>
              <button type="button" className="queued-prompt-action" title="end the current turn at its next pause, then send this" onClick={() => onSteer(item)} aria-label="steer with queued message">
                <Route size={12} />
              </button>
              <button type="button" className="queued-prompt-action" title="deliver this to the run's model right now - the turn continues with it in context" onClick={() => onDeliverNow(item)} aria-label="deliver queued message now">
                <Zap size={12} />
              </button>
              <button type="button" className="queued-prompt-action" title="stop the current turn and send the queue now" onClick={() => onSendNow()} aria-label="send queued message now">
                <Play size={12} />
              </button>
            </>
          )}
          <button type="button" className="queued-prompt-action" title="pull back into the composer" onClick={() => onEdit(item)} aria-label="edit queued message">
            <Pencil size={12} />
          </button>
          <button type="button" className="queued-prompt-action" title="remove from queue" onClick={() => onRemove(item)} aria-label="remove queued message">
            <X size={12} />
          </button>
        </div>
        );
      })}
    </section>
  );
}

// Transient amber notice for degraded-but-alive states: the provider retrying
// after a busy/unavailable response, or an active run going quiet.
function WarningBar({ text }: { text: string }) {
  return (
    <div className="warning-bar" role="status">
      <TriangleAlert size={15} className="warning-bar-icon" />
      <p className="warning-bar-text">{text}</p>
    </div>
  );
}

function ErrorBar({ error, errorKind, errorHint, onRetry, onDismiss }: {
  error: string;
  errorKind?: string;
  errorHint?: string;
  onRetry: () => void;
  onDismiss: () => void;
}) {
  const [copied, setCopied] = useState(false);
  const copy = async () => {
    try {
      if (navigator.clipboard?.writeText) {
        await navigator.clipboard.writeText(error);
      } else {
        // Plain http origins off localhost lack the async clipboard API.
        const el = document.createElement("textarea");
        el.value = error;
        el.style.position = "fixed";
        el.style.opacity = "0";
        document.body.appendChild(el);
        el.select();
        document.execCommand("copy");
        document.body.removeChild(el);
      }
      setCopied(true);
      setTimeout(() => setCopied(false), 1500);
    } catch {
      // Clipboard unavailable; leave the icon as-is.
    }
  };
  const grammar = errorGrammar(error, errorKind, errorHint);
  return (
    <div className="error-bar" role="alert">
      <div className="error-bar-head">
        <CircleAlert size={15} className="error-bar-icon" />
        <span className="error-bar-title">an error happened</span>
        <span className="error-bar-kind">{grammar.title}</span>
        <span className="error-bar-actions">
          <button type="button" className="error-bar-action" title="retry" onClick={onRetry} aria-label="retry">
            <RefreshCw size={13} />
          </button>
          <button type="button" className="error-bar-action" title="copy error" onClick={() => void copy()} aria-label="copy error">
            {copied ? <Check size={13} /> : <Copy size={13} />}
          </button>
          <button type="button" className="error-bar-action" title="dismiss" onClick={onDismiss} aria-label="dismiss">
            <X size={13} />
          </button>
        </span>
      </div>
      {grammar.hint && <p className="error-bar-hint">{grammar.hint}</p>}
      {grammar.detail && (
        <details className="error-bar-detail">
          <summary>full detail</summary>
          <pre>{grammar.detail}</pre>
        </details>
      )}
    </div>
  );
}

function RunPill({ status }: { status: RunStatus }) {
  return (
    <span className={`run-pill ${statusClass(status)}`}>
      <span className="run-pill-dot" />
      {statusLabel(status)}
    </span>
  );
}

// The character's latest recorded state (status.set tool), surfaced in the
// toolbar. The daemon stores the label verbatim; empty = none recorded.
function StatusChip({ state, recordedAt }: { state: string; recordedAt?: string }) {
  if (!state) return null;
  return (
    <span className="status-chip" title={recordedAt ? `state recorded ${recordedAt}` : "character state"}>
      <span />
      {state}
    </span>
  );
}

function Brand({ onHome }: { onHome: () => void }) {
  return (
    <button type="button" className="brand-lockup" onClick={onHome} aria-label="Go to overview" title="overview">
      <span className="brand-mark">
        <span className="brand-core">n</span>
        <span className="brand-orbit orbit-one" />
        <span className="brand-orbit orbit-two" />
      </span>
      <span className="brand-text">
        <span className="brand-name">nyaxus</span>
        <span className="brand-subtitle">agent control plane</span>
      </span>
    </button>
  );
}

function Sidebar({
  view,
  onView,
  onHome,
  sessions,
  currentSessionId,
  onSession,
  onNew,
  open,
  onClose,
  session,
  activeRuns,
  onDelete,
  onRename,
  onRetitle,
}: {
  view: View;
  onView: (view: View) => void;
  onHome: () => void;
  sessions: HarnessSession[];
  currentSessionId: string;
  onSession: (id: string) => void;
  onNew: () => void;
  open: boolean;
  onClose: () => void;
  session: HarnessSession | undefined;
  activeRuns: number;
  onDelete: (id: string) => void;
  onRename: (id: string, title: string) => void;
  onRetitle: (id: string) => void;
}) {
  const [filter, setFilter] = useState("");
  const [renamingId, setRenamingId] = useState("");
  const [renameValue, setRenameValue] = useState("");
  const commitRename = (id: string) => {
    if (renamingId !== id) return;
    const title = renameValue.trim();
    setRenamingId("");
    if (title) onRename(id, title);
  };
  const [confirmDeleteId, setConfirmDeleteId] = useState("");
  const confirmTimerRef = useRef<number | undefined>(undefined);
  const requestDelete = (id: string) => {
    if (confirmDeleteId === id) {
      window.clearTimeout(confirmTimerRef.current);
      setConfirmDeleteId("");
      onDelete(id);
      return;
    }
    setConfirmDeleteId(id);
    window.clearTimeout(confirmTimerRef.current);
    confirmTimerRef.current = window.setTimeout(() => setConfirmDeleteId(""), 2500);
  };
  const visibleSessions = useMemo(
    () => sessions
      // Worker sessions (agent.spawn children) stay out of the list: they
      // render inline in the conversation that spawned them.
      .filter((session) => !session.parentSessionId)
      .filter((session) => session.title.toLowerCase().includes(filter.toLowerCase()))
      .sort((a, b) => {
        const delta = timestampMs(b.updatedAt) - timestampMs(a.updatedAt);
        return Number.isFinite(delta) && delta !== 0 ? delta : b.id.localeCompare(a.id);
      }),
    [sessions, filter],
  );
  return (
    <aside className={`sidebar ${open ? "sidebar-open" : ""}`}>
      <div className="sidebar-topline">
        <button className="icon-button mobile-only" onClick={onClose} aria-label="Close navigation">
          <X size={17} />
        </button>
      </div>
      <Brand onHome={onHome} />
      <button className="new-session-button" onClick={onNew}>
        <Plus size={16} />
        <span>new conversation</span>
        <kbd>n</kbd>
      </button>

      <nav className="main-nav" aria-label="Primary navigation">
        {navGroups.map((group) => (
          <div className="nav-group" key={group.label}>
            <div className="nav-label">{group.label}</div>
            {group.items.map((item) => {
              const Icon = item.icon;
              return (
                <button className={`nav-link ${view === item.id ? "nav-link-active" : ""}`} key={item.id} onClick={() => onView(item.id)}>
                  <Icon size={16} strokeWidth={1.8} />
                  <span>{item.label}</span>
                  {item.id === "runs" && activeRuns > 0 && <span className="nav-count">{activeRuns.toString().padStart(2, "0")}</span>}
                </button>
              );
            })}
          </div>
        ))}
      </nav>

      <div className="session-library">
        <div className="library-heading">
          <span className="nav-label">recent threads</span>
          <span className="library-count">{visibleSessions.length.toString().padStart(2, "0")}</span>
        </div>
        <label className="search-box">
          <Search size={14} />
          <input value={filter} onChange={(event) => setFilter(event.target.value)} placeholder="filter sessions" />
          <kbd>/</kbd>
        </label>
        <div className="session-list">
          {visibleSessions.map((session) => (
            <div
              className={`session-item ${currentSessionId === session.id ? "session-item-active" : ""}`}
              key={session.id}
              role="button"
              tabIndex={0}
              onClick={() => { if (renamingId !== session.id) onSession(session.id); }}
              onKeyDown={(event) => {
                if (renamingId === session.id) return;
                if (event.key === "Enter" || event.key === " ") {
                  event.preventDefault();
                  onSession(session.id);
                }
              }}
            >
              <span className={`session-status session-${session.status}`} />
              <span className="session-copy">
                {renamingId === session.id ? (
                  <input
                    className="session-rename-input"
                    value={renameValue}
                    maxLength={200}
                    autoFocus
                    aria-label="rename session"
                    onClick={(event) => event.stopPropagation()}
                    onChange={(event) => setRenameValue(event.target.value)}
                    onBlur={() => commitRename(session.id)}
                    onKeyDown={(event) => {
                      event.stopPropagation();
                      if (event.key === "Enter") {
                        event.preventDefault();
                        commitRename(session.id);
                      }
                      if (event.key === "Escape") setRenameValue("");
                    }}
                  />
                ) : (
                  <strong>{session.title}{session.parentSessionId && <span className="session-child-chip" title={`spawned as a worker from ${session.parentSessionId}`}>worker</span>}</strong>
                )}
                <span>{session.messageCount} messages · {formatTime(session.updatedAt)}</span>
              </span>
              {session.unread && <span className="unread-dot" />}
              {renamingId !== session.id && (
                <button
                  className="session-rename"
                  aria-label="rename session"
                  title="rename"
                  onClick={(event) => {
                    event.stopPropagation();
                    setRenamingId(session.id);
                    setRenameValue(session.title);
                  }}
                >
                  <Pencil size={12} />
                </button>
              )}
              {renamingId !== session.id && (
                <button
                  className="session-rename"
                  aria-label="retitle session with the model"
                  title="retitle with the model (next run)"
                  onClick={(event) => {
                    event.stopPropagation();
                    onRetitle(session.id);
                  }}
                >
                  <Sparkles size={12} />
                </button>
              )}
              <button
                className={`session-delete ${confirmDeleteId === session.id ? "session-delete-confirm" : ""}`}
                aria-label={confirmDeleteId === session.id ? "confirm delete session" : "delete session"}
                onClick={(event) => {
                  event.stopPropagation();
                  requestDelete(session.id);
                }}
              >
                {confirmDeleteId === session.id ? "sure?" : <Trash2 size={13} />}
              </button>
            </div>
          ))}
        </div>
      </div>

      <div className="profile-card">
        <div className="profile-avatar">{(session?.profile || "orchestrator").charAt(0).toUpperCase()}</div>
        <div className="profile-copy">
          <strong>{session?.profile || "orchestrator"}</strong>
          <span>{session?.model || "no active session"}</span>
        </div>
      </div>
    </aside>
  );
}

// Top-bar breadcrumb per view, matching each page's own eyebrow so the bar
// never shows a stale "sessions / new conversation" on a non-chat page.
const CRUMB_SECTIONS: Record<View, [string, string]> = {
  home: ["nyaxus", "overview"],
  chat: ["sessions", "new conversation"],
  runs: ["control", "execution"],
  graph: ["control", "lineage"],
  memory: ["systems", "recall"],
  workspace: ["systems", "local files"],
  observatory: ["systems", "telemetry"],
  providers: ["systems", "connections"],
  profiles: ["systems", "personas"],
  knowledge: ["systems", "retrieval"],
  prompts: ["systems", "templates"],
  skills: ["systems", "capabilities"],
  tools: ["systems", "actions"],
  settings: ["systems", "runtime"],
};

// Global pending-actions center: one always-visible
// queue for the decisions the operator owes - approval gates and structured
// questions - with a live waiting countdown, plus the provenance trail of
// decisions the daemon made without the operator (auto-approvals, loop
// guards). Answers route by run id, so items from any session are actionable.
function PendingActionsCenter({
  open,
  pending,
  provenance,
  sessions,
  onApprove,
  onAnswer,
  onPick,
  onClose,
}: {
  open: boolean;
  pending: PendingAction[];
  provenance: ProvenanceEntry[];
  sessions: HarnessSession[];
  onApprove: (runId: string, approvalId: string, decision: "allow_once" | "allow_turn" | "allow_session" | "allow_always" | "deny") => void;
  onAnswer: (runId: string, inputId: string, choice: number) => void;
  onPick: (sessionId: string) => void;
  onClose: () => void;
}) {
  // Live countdown: tick once a second while the panel is open with items.
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    if (!open || pending.length === 0) return;
    const timer = window.setInterval(() => setNow(Date.now()), 1000);
    return () => window.clearInterval(timer);
  }, [open, pending.length]);
  if (!open) return null;
  const sessionTitle = (sessionId: string) => sessions.find((item) => item.id === sessionId)?.title || "another session";
  const elapsed = (requestedAt: string) => {
    const started = timestampMs(requestedAt);
    if (!Number.isFinite(started)) return "";
    const sec = Math.max(0, Math.floor((now - started) / 1000));
    if (sec < 60) return `${sec}s`;
    if (sec < 3600) return `${Math.floor(sec / 60)}m ${sec % 60}s`;
    return `${Math.floor(sec / 3600)}h ${Math.floor((sec % 3600) / 60)}m`;
  };
  return (
    <div className="actions-panel" role="dialog" aria-label="Pending actions">
      <div className="actions-head"><span>pending actions</span><button onClick={onClose}>close</button></div>
      {pending.length === 0 && provenance.length === 0 && (
        <div className="actions-empty">nothing owed. approval gates and questions land here while a run waits on you; auto-approvals and loop guards leave their provenance below.</div>
      )}
      {pending.length > 0 && pending.map((action) => (
        <div key={action.id} className="actions-item">
          <button type="button" className="actions-item-jump" onClick={() => onPick(action.sessionId)} title="open the session">
            {action.kind === "approval" ? <ShieldCheck size={13} /> : <HelpCircle size={13} />}
            <span className="actions-item-copy">
              <strong>{action.kind === "approval" ? action.name : "question"}</strong>
              <small>{sessionTitle(action.sessionId)} · waiting {elapsed(action.requestedAt)}</small>
            </span>
          </button>
          {action.kind === "input" && action.question && <p className="actions-question">{action.question}</p>}
          {action.kind === "approval" ? (
            <div className="actions-decisions">
              <button type="button" onClick={() => onApprove(action.runId, action.id, "deny")}>deny</button>
              <button type="button" onClick={() => onApprove(action.runId, action.id, "allow_always")} title={action.fingerprint
                ? `always allow matching: ${action.fingerprint} - in every session, until removed in settings`
                : "allow every call of this tool in every session, until removed in settings"}>always</button>
              <button type="button" onClick={() => onApprove(action.runId, action.id, "allow_session")} title="allow every call of this tool for the rest of the session">session</button>
              <button type="button" onClick={() => onApprove(action.runId, action.id, "allow_turn")} title="allow every call of this tool for the rest of this turn">turn</button>
              <button type="button" className="actions-allow" onClick={() => onApprove(action.runId, action.id, "allow_once")}>allow</button>
            </div>
          ) : (
            <div className="actions-decisions">
              {(action.options || []).map((option, index) => (
                <button type="button" key={index} onClick={() => onAnswer(action.runId, action.id, index)}>{option}</button>
              ))}
              {action.hasDontKnow && <button type="button" onClick={() => onAnswer(action.runId, action.id, -1)}>don't know</button>}
            </div>
          )}
        </div>
      ))}
      {provenance.length > 0 && (
        <div className="actions-provenance">
          <div className="actions-section-label">provenance - decided without you</div>
          {provenance.map((entry) => (
            <div key={entry.id} className="actions-prov-row">
              <span className={`actions-prov-kind ${entry.kind === "approval.auto" ? "auto" : "guard"}`}>{entry.kind === "approval.auto" ? "auto" : "guard"}</span>
              <span className="actions-prov-copy">
                <strong>{entry.label}</strong>
                {entry.detail && <small>{entry.detail}</small>}
              </span>
              <small className="actions-prov-time">{formatTime(entry.at)}</small>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}

// ⌘K full-text message search: a global palette over the
// daemon's ledger. 180ms debounce, ↑↓/Enter/Esc keyboard navigation; Enter
// jumps to the hit's session + message (the App's pendingJump handoff).
function SearchPalette({
  open,
  onSearch,
  onPick,
  onClose,
}: {
  open: boolean;
  onSearch: (query: string) => Promise<GlobalSearchResponse>;
  onPick: (match: GlobalSearchMatch) => void;
  onClose: () => void;
}) {
  const [query, setQuery] = useState("");
  const [results, setResults] = useState<GlobalSearchMatch[] | null>(null);
  const [truncated, setTruncated] = useState(false);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(false);
  const [active, setActive] = useState(0);
  const inputRef = useRef<HTMLInputElement>(null);
  const seqRef = useRef(0);

  // Reset and autofocus on each open.
  useEffect(() => {
    if (!open) return;
    setQuery("");
    setResults(null);
    setTruncated(false);
    setError("");
    setLoading(false);
    setActive(0);
    requestAnimationFrame(() => inputRef.current?.focus());
  }, [open]);

  // Debounced search; out-of-order responses are dropped by seq, so a fast
  // typing burst only lands the latest query.
  useEffect(() => {
    if (!open) return;
    const trimmed = query.trim();
    if (!trimmed) {
      setResults(null);
      setError("");
      setLoading(false);
      return;
    }
    setLoading(true);
    setError("");
    const seq = ++seqRef.current;
    const timer = window.setTimeout(() => {
      void onSearch(trimmed).then((response) => {
        if (seqRef.current !== seq) return;
        setResults(response.matches);
        setTruncated(response.truncated);
        setLoading(false);
        setActive(0);
      }).catch((err) => {
        if (seqRef.current !== seq) return;
        setError(err instanceof Error ? err.message : String(err));
        setLoading(false);
      });
    }, 180);
    return () => window.clearTimeout(timer);
  }, [open, query, onSearch]);

  // Keep the highlighted row in range when results shrink.
  useEffect(() => {
    if (results && active >= results.length) setActive(0);
  }, [results, active]);

  if (!open) return null;
  const pick = (index: number) => {
    const match = results?.[index];
    if (match) onPick(match);
  };
  const onKeyDown = (event: KeyboardEvent<HTMLInputElement>) => {
    if (event.key === "ArrowDown") {
      event.preventDefault();
      if (results && results.length > 0) setActive((previous) => (previous + 1) % results.length);
    } else if (event.key === "ArrowUp") {
      event.preventDefault();
      if (results && results.length > 0) setActive((previous) => (previous - 1 + results.length) % results.length);
    } else if (event.key === "Enter") {
      event.preventDefault();
      pick(active);
    } else if (event.key === "Escape") {
      event.preventDefault();
      onClose();
    }
  };
  return createPortal(
    <div className="overlay-backdrop search-backdrop" onMouseDown={onClose}>
      <div className="search-palette" role="dialog" aria-modal="true" aria-label="Search messages" onMouseDown={(event) => event.stopPropagation()}>
        <div className="search-input-row">
          <Search size={15} />
          <input
            ref={inputRef}
            value={query}
            onChange={(event) => setQuery(event.target.value)}
            onKeyDown={onKeyDown}
            placeholder="Search every session…"
            spellCheck={false}
          />
          <kbd>esc</kbd>
        </div>
        <div className="search-results">
          {error && <div className="search-note search-note-error">{error}</div>}
          {!error && loading && <div className="search-note">searching…</div>}
          {!error && !loading && results === null && <div className="search-note">type to search messages across all sessions (newest first).</div>}
          {!error && !loading && results !== null && results.length === 0 && <div className="search-note">no messages matched “{query.trim()}”.</div>}
          {results?.map((match, index) => (
            <button
              key={`${match.sessionId}-${match.messageId}`}
              type="button"
              className={`search-result ${index === active ? "search-result-active" : ""}`}
              onMouseEnter={() => setActive(index)}
              onClick={() => pick(index)}
            >
              <span className="search-result-meta">
                <span className={`search-result-role ${match.role}`}>{match.role}</span>
                <strong>{match.sessionTitle || "session"}</strong>
                <small>{formatTime(match.createdAt)}</small>
              </span>
              <span className="search-result-snippet">{match.snippet}</span>
            </button>
          ))}
          {!error && truncated && <div className="search-note">results truncated - refine the query.</div>}
        </div>
      </div>
    </div>,
    document.body,
  );
}

function Header({
  session,
  sessions,
  view,
  connected,
  connectionDetail,
  connectionTarget,
  lastEventType,
  theme,
  onTheme,
  onMenu,
  inspectorOpen,
  onInspector,
  onGraph,
  notifications,
  bellOpen,
  onBell,
  onPick,
  onClear,
  pendingActions,
  provenance,
  actionsOpen,
  onActions,
  searchOpen,
  onSearch,
  onApproveAction,
  onAnswerAction,
}: {
  session: HarnessSession | undefined;
  view: View;
  connected: boolean;
  connectionDetail: string | null;
  connectionTarget: string;
  lastEventType: string | null;
  theme: "aero" | "night";
  onTheme: () => void;
  onMenu: () => void;
  inspectorOpen: boolean;
  onInspector: () => void;
  onGraph: () => void;
  notifications: AppNotification[];
  bellOpen: boolean;
  onBell: () => void;
  onPick: (sessionId: string) => void;
  onClear: () => void;
  sessions: HarnessSession[];
  pendingActions: PendingAction[];
  provenance: ProvenanceEntry[];
  actionsOpen: boolean;
  onActions: () => void;
  searchOpen: boolean;
  onSearch: () => void;
  onApproveAction: (runId: string, approvalId: string, decision: "allow_once" | "allow_turn" | "allow_session" | "allow_always" | "deny") => void;
  onAnswerAction: (runId: string, inputId: string, choice: number) => void;
}) {
  const unread = notifications.filter((notification) => !notification.read).length;
  const [connectionPanelOpen, setConnectionPanelOpen] = useState(false);
  const phase = connectionDetail || (connected ? "event stream connected" : "waiting for event stream");
  const streamTarget = session?.id
    ? `${connectionTarget}/sessions/${encodeURIComponent(session.id)}/events`
    : `${connectionTarget}/sessions/{session_id}/events`;
  const [crumbSection, crumbItem] = CRUMB_SECTIONS[view];
  const crumbLabel = view === "chat" ? (session?.title || crumbItem) : crumbItem;
  return (
    <header className="topbar">
      <div className="topbar-context">
        <button className="icon-button mobile-only" onClick={onMenu} aria-label="Open navigation">
          <Menu size={18} />
        </button>
        <div className="crumb"><span className="crumb-muted">{crumbSection}</span><span>/</span><strong>{crumbLabel}</strong></div>
      </div>
      <div className="topbar-actions">
        <div className="connection-wrap">
          <button
            type="button"
            className={`connection-state ${connected ? "connection-on" : "connection-off"}`}
            onClick={() => setConnectionPanelOpen((previous) => !previous)}
            aria-expanded={connectionPanelOpen}
            aria-haspopup="dialog"
            title={phase}
          >
            <span /> {connected ? "" : "reconnecting"}
          </button>
          {connectionPanelOpen && (
            <div className="connection-panel" role="dialog" aria-label="Harness connection details">
              <div className="connection-panel-head">
                <span>connection details</span>
                <span className={connected ? "connection-panel-online" : "connection-panel-offline"}>
                  {connected ? "online" : "waiting"}
                </span>
              </div>
              <div className="connection-detail-row"><span>phase</span><strong>{phase}</strong></div>
              <div className="connection-detail-row"><span>session</span><strong>{session?.title || "none selected"}</strong></div>
              <div className="connection-detail-row connection-detail-target"><span>event stream</span><code>{streamTarget}</code></div>
              <div className="connection-detail-row"><span>last event</span><strong>{lastEventType || "none received"}</strong></div>
              <p>{connected ? "the event stream is open and delivering ledger events." : "the browser is waiting for the harness event stream. check the daemon address and port if this persists."}</p>
            </div>
          )}
        </div>
        <button className="topbar-button" onClick={onTheme} title="Toggle theme">
          {theme === "aero" ? <Moon size={15} /> : <Sun size={15} />}
          <span>{theme === "aero" ? "night" : "aero"}</span>
        </button>
        <button className={`topbar-button inspector-toggle ${inspectorOpen ? "button-selected" : ""}`} onClick={onInspector}>
          <PanelRight size={15} />
          <span>inspector</span>
        </button>
        <button className={`topbar-button ${view === "graph" ? "button-selected" : ""}`} onClick={onGraph} title="session graph: branch lineage for this conversation">
          <GitFork size={15} />
          <span>graph</span>
        </button>
        <button className={`icon-button ${searchOpen ? "button-selected" : ""}`} onClick={onSearch} aria-label="Search messages" title="Search messages (Ctrl+K)">
          <Search size={16} />
        </button>
        <div className="actions-wrap">
          <button className={`icon-button ${actionsOpen ? "button-selected" : ""}`} onClick={onActions} aria-label="Pending actions">
            <Inbox size={16} />
          </button>
          {pendingActions.length > 0 && <span className="bell-badge actions-badge">{pendingActions.length > 9 ? "9+" : pendingActions.length.toString()}</span>}
          <PendingActionsCenter
            open={actionsOpen}
            pending={pendingActions}
            provenance={provenance}
            sessions={sessions}
            onApprove={onApproveAction}
            onAnswer={onAnswerAction}
            onPick={(sessionId) => { onPick(sessionId); onActions(); }}
            onClose={onActions}
          />
        </div>
        <div className="bell-wrap">
          <button className={`icon-button ${bellOpen ? "button-selected" : ""}`} onClick={onBell} aria-label="Notifications">
            <Bell size={16} />
          </button>
          {unread > 0 && <span className="bell-badge">{unread > 9 ? "9+" : unread.toString()}</span>}
          {bellOpen && (
            <div className="bell-panel">
              <div className="bell-head">
                <span>notifications</span>
                <button onClick={onClear}>mark all read</button>
              </div>
              {notifications.length ? notifications.map((notification) => {
                const Icon = notificationIcon[notification.kind];
                return (
                  <button key={notification.id} className="bell-item" onClick={() => notification.sessionId && onPick(notification.sessionId)}>
                    <Icon size={14} />
                    <span className="bell-copy">
                      <strong>{notification.text}</strong>
                      <small>{formatTime(notification.time)}</small>
                    </span>
                  </button>
                );
              }) : <div className="bell-empty">nothing yet. run completions, approvals, and connection changes land here.</div>}
            </div>
          )}
        </div>
      </div>
    </header>
  );
}

function Composer({
  value,
  onChange,
  onSend,
  onStop,
  disabled,
  streaming,
  profile,
  model,
  profiles,
  providers,
  defaultProvider,
  onSelectProfile,
  run,
  seed,
  onListFs,
  onListSnippets,
  sessionKey,
  groupSpeakers,
  onGroupSpeakers,
  fixedSpeakers,
}: {
  value: string;
  onChange: (value: string) => void;
  onSend: (content: string, attachments: HarnessAttachment[], mentions: string[], speakers: string[]) => void;
  onStop: () => void;
  disabled: boolean;
  streaming: boolean;
  profile: string;
  model: string;
  profiles: HarnessProfile[];
  providers: HarnessProvider[];
  defaultProvider: string;
  onSelectProfile: (profileId: string) => void;
  run: HarnessRun | null;
  seed: ComposerSeed | null;
  onListFs?: (path: string) => Promise<FsListing>;
  onListSnippets: () => Promise<PromptSnippet[]>;
  sessionKey: string;
  // Group chat (docs/group-chat.md): the speaker roster picked for the next
  // send (2+ creates a group session); fixedSpeakers (an existing group
  // session) shows the immutable composition.
  groupSpeakers: string[];
  onGroupSpeakers: (ids: string[]) => void;
  fixedSpeakers?: string[];
}) {
  const [attachments, setAttachments] = useState<HarnessAttachment[]>([]);
  const [attachmentError, setAttachmentError] = useState("");
  const [processingAttachments, setProcessingAttachments] = useState(false);
  const [overlayIndex, setOverlayIndex] = useState<number | null>(null);
  const [dragOver, setDragOver] = useState(false);
  const [mentionSuggestions, setMentionSuggestions] = useState<FsEntry[] | null>(null);
  const [mentionHighlight, setMentionHighlight] = useState(0);
  const mentionTimer = useRef<number | undefined>(undefined);
  const mentionRequest = useRef(0);
  const mentionsRef = useRef<string[]>([]);
  // @-mention: typing @ opens a path picker for the typed parent directory
  // (the same fs route as the vault picker). Selected spans are recorded and
  // shipped with the send as explicit mentions, so paths with spaces reach
  // the daemon intact; a hand-typed @token without spaces is recovered by
  // the daemon's text scan instead.
  const mentionSegmentStart = (text: string, cursor: number): number => {
    for (let i = cursor - 1; i >= 0; i--) {
      const ch = text[i];
      if (ch === "@") return i;
      if (ch === " " || ch === "\t" || ch === "\n" || ch === "\r") return -1;
    }
    return -1;
  };
  const scheduleMentionSuggestions = (text: string, cursor: number) => {
    window.clearTimeout(mentionTimer.current);
    const start = mentionSegmentStart(text, cursor);
    if (start < 0 || !onListFs) {
      setMentionSuggestions(null);
      return;
    }
    const segment = text.slice(start + 1, cursor);
    const lastSep = Math.max(segment.lastIndexOf("/"), segment.lastIndexOf("\\"));
    const dir = lastSep >= 0 ? segment.slice(0, lastSep + 1) : "";
    const prefix = (lastSep >= 0 ? segment.slice(lastSep + 1) : segment).toLowerCase();
    const absolute = dir.length > 0 &&
      (dir[0] === "/" || (dir[1] === ":" && (dir[2] === "\\" || dir[2] === "/")));
    if (!absolute) {
      setMentionSuggestions(null);
      return;
    }
    mentionTimer.current = window.setTimeout(() => {
      const requestId = ++mentionRequest.current;
      onListFs(dir).then((listing) => {
        if (requestId !== mentionRequest.current) return;
        const matches = listing.entries.filter((entry) => entry.name.toLowerCase().startsWith(prefix));
        setMentionSuggestions(matches.length ? matches : null);
        setMentionHighlight(0);
      }).catch(() => {
        if (requestId === mentionRequest.current) setMentionSuggestions(null);
      });
    }, 200);
  };
  const applyMention = (entry: FsEntry) => {
    const el = textareaRef.current;
    const cursor = el?.selectionStart ?? value.length;
    const start = mentionSegmentStart(value, cursor);
    if (start < 0) return;
    const segment = value.slice(start + 1, cursor);
    const lastSep = Math.max(segment.lastIndexOf("/"), segment.lastIndexOf("\\"));
    const dir = lastSep >= 0 ? segment.slice(0, lastSep + 1) : "";
    const sep = dir.includes("\\") ? "\\" : "/";
    const inserted = entry.name + (entry.dir ? sep : "");
    onChange(value.slice(0, start + 1) + dir + inserted + value.slice(cursor));
    mentionsRef.current = [...mentionsRef.current, dir + inserted];
    setMentionSuggestions(null);
    setMentionHighlight(0);
    window.requestAnimationFrame(() => {
      const node = textareaRef.current;
      if (node) {
        const pos = start + 1 + dir.length + inserted.length;
        node.focus();
        node.setSelectionRange(pos, pos);
      }
    });
  };
  const textInputRef = useRef<HTMLInputElement>(null);
  const imageInputRef = useRef<HTMLInputElement>(null);
  const textareaRef = useRef<HTMLTextAreaElement>(null);
  const attachmentIdRef = useRef(0);
  const dragDepthRef = useRef(0);
  const selectedProfile = profiles.find((item) => item.id === profile);
  const profileSupportsVision = profileVisionEnabled(selectedProfile, providers, defaultProvider);
  // An edited queued prompt lands here as a draft. The seed object changes on
  // every edit, so re-seeding the attachment strip is a prop-change reset,
  // done during render (the documented pattern) instead of an effect.
  const [seedSnapshot, setSeedSnapshot] = useState<ComposerSeed | null>(null);
  if (seed !== seedSnapshot) {
    setSeedSnapshot(seed);
    if (seed) setAttachments(seed.attachments);
  }
  // Prompt snippets (docs/prompt-snippets.md): per-message toggles, reset to
  // all-off at session start (the key changes) and after each send.
  const [snippetPickerOpen, setSnippetPickerOpen] = useState(false);
  const [snippets, setSnippets] = useState<PromptSnippet[] | null>(null);
  const [selectedSnippets, setSelectedSnippets] = useState<ReadonlySet<string>>(new Set());
  const [sessionKeySnapshot, setSessionKeySnapshot] = useState(sessionKey);
  if (sessionKey !== sessionKeySnapshot) {
    setSessionKeySnapshot(sessionKey);
    setSelectedSnippets(new Set());
    setSnippetPickerOpen(false);
  }
  const toggleSnippetPicker = () => {
    const next = !snippetPickerOpen;
    setSnippetPickerOpen(next);
    if (next) {
      // Re-scanned on every open: the files are the source of truth, no
      // client cache to go stale.
      onListSnippets().then(setSnippets).catch(() => setSnippets([]));
    }
  };
  const toggleSnippet = (name: string) => {
    setSelectedSnippets((previous) => {
      const next = new Set(previous);
      if (next.has(name)) next.delete(name);
      else next.add(name);
      return next;
    });
  };
  // Group chat (docs/group-chat.md): the speaker picker. An existing group
  // session shows its immutable composition (fixedSpeakers); a draft or a
  // single-profile session picks the roster for the next send.
  const [groupPickerOpen, setGroupPickerOpen] = useState(false);
  const [groupSessionKeySnapshot, setGroupSessionKeySnapshot] = useState(sessionKey);
  if (sessionKey !== groupSessionKeySnapshot) {
    setGroupSessionKeySnapshot(sessionKey);
    setGroupPickerOpen(false);
  }
  const groupRoster = fixedSpeakers && fixedSpeakers.length > 0 ? fixedSpeakers : groupSpeakers;
  // The participants chip's hover summary resolves roster ids to display
  // names (the profile row may have been renamed after the group opened).
  const groupParticipantName = (id: string) => profiles.find((item) => item.id === id)?.name || id;
  const toggleGroupSpeaker = (id: string) => {
    if (fixedSpeakers) return;  // a group's composition is fixed at creation
    const next = groupSpeakers.includes(id)
      ? groupSpeakers.filter((entry) => entry !== id)
      : [...groupSpeakers, id];
    onGroupSpeakers(next);
  };

  useEffect(() => {
    const el = textareaRef.current;
    if (!el) return;
    if (seed) el.focus();
    // Grow the textarea with long drafts instead of pinning it to two lines;
    // past the cap it scrolls internally.
    el.style.height = "auto";
    el.style.height = `${Math.min(el.scrollHeight, COMPOSER_MAX_HEIGHT_PX)}px`;
  }, [seed, value]);


  const nextAttachmentId = () => {
    attachmentIdRef.current += 1;
    return `browser-attachment-${attachmentIdRef.current}`;
  };

  const send = () => {
    // Selected prompt snippets merge into the sent message (prepend group
    // -> text -> append group); the toggles reset after the send.
    const base = mergePromptSnippets(
      value.trim(),
      snippets || [],
      selectedSnippets,
    ).trim();
    if (processingAttachments || (!base && attachments.length === 0)) return;
    if (selectedSnippets.size > 0) setSelectedSnippets(new Set());
    if (attachments.some((attachment) => attachment.kind === "image") && !profileSupportsVision) {
      setAttachmentError(VISION_UNAVAILABLE_ERROR);
      return;
    }
    // Long composer text ships as a .txt attachment so the transcript stays
    // lean; the daemon accepts an attachment-only message.
    let content = base;
    let outgoing = attachments;
    if (base.length > LONG_TEXT_ATTACHMENT_BYTES) {
      if (base.length > MAX_TEXT_ATTACHMENT_BYTES) {
        setAttachmentError(`text is too long to send (max ${formatBytes(MAX_TEXT_ATTACHMENT_BYTES)})`);
        return;
      }
      content = "";
      outgoing = [
        ...attachments,
        {
          id: nextAttachmentId(),
          name: "pasted-text.txt",
          kind: "text" as const,
          mimeType: "text/plain",
          size: base.length,
          content: base,
        },
      ];
    }
    const encodedPayloadBytes = outgoing.reduce(
      (total, attachment) => total + new Blob([attachment.content]).size,
      0,
    );
    if (encodedPayloadBytes > MAX_ATTACHMENT_PAYLOAD_BYTES) {
      setAttachmentError(`attachments exceed the ${formatBytes(MAX_ATTACHMENT_PAYLOAD_BYTES)} total limit`);
      return;
    }
    setAttachments([]);
    setOverlayIndex(null);
    setAttachmentError("");
    // Only ship mentions that survived in the final text (the user may have
    // deleted one after selecting it).
    const outgoingMentions = mentionsRef.current.filter((path) => base.includes("@" + path));
    mentionsRef.current = [];
    // Group chat: ship the picked roster; the App creates a group session
    // when it has 2+ speakers and the send opens a fresh conversation.
    onSend(content, outgoing, outgoingMentions, groupSpeakers);
  };

  // Shared intake for the file buttons, paste, and drag-and-drop: images go
  // through the normalizer, everything else through the text path.
  const addFiles = async (files: File[]) => {
    if (!files.length || processingAttachments) return;
    const room = MAX_ATTACHMENTS - attachments.length;
    if (room <= 0) {
      setAttachmentError(`maximum ${MAX_ATTACHMENTS} attachments`);
      return;
    }
    const candidates = files.slice(0, room);
    const errors: string[] = files.length > candidates.length ? [`maximum ${MAX_ATTACHMENTS} attachments`] : [];
    const accepted: HarnessAttachment[] = [];
    setAttachmentError("");
    setProcessingAttachments(true);
    for (const file of candidates) {
      if (isImageFile(file)) {
        if (!profileSupportsVision) {
          errors.push(`${file.name}: ${VISION_UNAVAILABLE_ERROR}`);
          continue;
        }
        try {
          accepted.push(await normalizeImageFile(file, nextAttachmentId()));
        } catch (error) {
          errors.push(`${file.name}: ${error instanceof Error ? error.message : "could not normalize image"}`);
        }
        continue;
      }
      // Document intake (pdf/docx/xlsx/pptx by extension or mime): binary
      // files ride as base64 data URLs (kind document) - the daemon stores
      // them and serializes a workspace copy the terminal tool can use;
      // the bytes never enter the prompt context.
      if (isDocumentFile(file)) {
        if (file.size > MAX_DOCUMENT_ATTACHMENT_BYTES) {
          errors.push(`${file.name}: documents must be 8 mb or smaller`);
          continue;
        }
        try {
          const content = await readAsDataURL(file);
          accepted.push({
            id: nextAttachmentId(),
            name: file.name,
            kind: "document" as const,
            mimeType: file.type || "application/octet-stream",
            size: file.size,
            content,
          });
        } catch {
          errors.push(`${file.name}: could not read document`);
        }
        continue;
      }
      if (file.size > MAX_TEXT_ATTACHMENT_BYTES) {
        errors.push(`${file.name}: text files must be 200 kb or smaller`);
        continue;
      }
      try {
        const content = await file.text();
        if (content.includes("\u0000")) {
          errors.push(`${file.name}: binary content is not supported`);
          continue;
        }
        accepted.push({
          id: nextAttachmentId(),
          name: file.name,
          kind: "text",
          mimeType: file.type || "text/plain",
          size: file.size,
          content,
        });
      } catch {
        errors.push(`${file.name}: could not read text file`);
      }
    }
    setAttachments((previous) => [...previous, ...accepted].slice(0, MAX_ATTACHMENTS));
    setProcessingAttachments(false);
    setAttachmentError(errors.join(" "));
  };

  const onPaste = (event: ClipboardEvent<HTMLTextAreaElement>) => {
    const files = Array.from(event.clipboardData?.files || []);
    if (files.length > 0) {
      event.preventDefault();
      void addFiles(files);
      return;
    }
    // Large pasted text becomes an attachment instead of a transcript wall
    // (the reader gets a named text file through the same addFiles intake).
    // A short paste that carries text AND files would have taken the files
    // branch above.
    const text = event.clipboardData?.getData("text/plain") || "";
    if (text.length > 1500) {
      event.preventDefault();
      void addFiles([new File([text], `Pasted_Text_${Date.now()}.txt`, { type: "text/plain" })]);
    }
  };

  const onDragEnter = (event: DragEvent<HTMLDivElement>) => {
    event.preventDefault();
    dragDepthRef.current += 1;
    setDragOver(true);
  };

  const onDragLeave = (event: DragEvent<HTMLDivElement>) => {
    event.preventDefault();
    dragDepthRef.current = Math.max(0, dragDepthRef.current - 1);
    if (dragDepthRef.current === 0) setDragOver(false);
  };

  const onDrop = (event: DragEvent<HTMLDivElement>) => {
    event.preventDefault();
    dragDepthRef.current = 0;
    setDragOver(false);
    const files = Array.from(event.dataTransfer?.files || []);
    if (files.length) void addFiles(files);
  };

  const onKeyDown = (event: KeyboardEvent<HTMLTextAreaElement>) => {
    if (mentionSuggestions && mentionSuggestions.length > 0) {
      if (event.key === "ArrowDown") {
        event.preventDefault();
        setMentionHighlight((h) => (h + 1) % mentionSuggestions.length);
        return;
      }
      if (event.key === "ArrowUp") {
        event.preventDefault();
        setMentionHighlight((h) => (h - 1 + mentionSuggestions.length) % mentionSuggestions.length);
        return;
      }
      if (event.key === "Enter" && !event.shiftKey) {
        event.preventDefault();
        applyMention(mentionSuggestions[mentionHighlight]);
        return;
      }
      if (event.key === "Escape") {
        setMentionSuggestions(null);
        return;
      }
    }
    if (event.key === "Enter" && !event.shiftKey) {
      event.preventDefault();
      send();
    }
  };

  const pickTextFiles = (event: ChangeEvent<HTMLInputElement>) => {
    const files = Array.from(event.target.files || []);
    event.target.value = "";
    void addFiles(files);
  };

  const pickImageFiles = (event: ChangeEvent<HTMLInputElement>) => {
    const files = Array.from(event.target.files || []);
    event.target.value = "";
    void addFiles(files);
  };

  return (
    <div className="composer-wrap">
      <div
        className={`composer ${dragOver ? "composer-drop-target" : ""}`}
        onDragEnter={onDragEnter}
        onDragOver={(event) => event.preventDefault()}
        onDragLeave={onDragLeave}
        onDrop={onDrop}
      >
        <div className="composer-topline">
          <div className="composer-profile">
            {/* A group session talks to a roster, not one profile: the
                participants chip replaces the profile selector and opens the
                same (fixed) roster panel. Title carries the hover summary. */}
            {fixedSpeakers && fixedSpeakers.length >= 2 ? (
              <button
                type="button"
                className="composer-profile-participants"
                title={"participants in speaking order: " + fixedSpeakers.map((id) => groupParticipantName(id)).join(", ")}
                onClick={() => setGroupPickerOpen((open) => !open)}
                disabled={disabled}
                aria-label="group participants"
              >
                <Users size={12} /> {fixedSpeakers.length} participants
              </button>
            ) : (
              <>
                <Command size={12} />
                {/* The daemon pins the profile when a run starts; changing this
                    while a run is active is safe and applies to the next turn. */}
                <select
                  value={profile}
                  disabled={disabled}
                  onChange={(event) => {
                    setAttachmentError("");
                    onSelectProfile(event.target.value);
                  }}
                  aria-label="session profile"
                >
                  {!profiles.some((item) => item.id === profile) && <option value={profile}>{profile}</option>}
                  {profiles.map((item) => <option key={item.id} value={item.id}>{item.name}</option>)}
                </select>
                <span className={`composer-capability ${profileSupportsVision ? "composer-capability-on" : ""}`}>
                  <ImageIcon size={11} /> {profileSupportsVision ? "vision" : "text only"}
                </span>
                <button
                  type="button"
                  className={`composer-icon ${groupRoster.length >= 2 ? "group-button-on" : ""}`}
                  title={fixedSpeakers ? "group session - the speaker roster is fixed" : "group chat - pick 2+ speakers for a shared roundtable"}
                  onClick={() => setGroupPickerOpen((open) => !open)}
                  disabled={disabled}
                  aria-label="Group speakers"
                >
                  <Users size={16} />
                  {groupRoster.length >= 2 && <span className="group-count">{groupRoster.length}</span>}
                </button>
              </>
            )}
          </div>
          <span className="composer-model"><span className="model-dot" /> local / {model}</span>
        </div>
        {groupPickerOpen && (
          <div className="group-picker">
            <div className="group-picker-head">
              group speakers - order is the speaking order
              {fixedSpeakers && <span className="group-picker-fixed">roster fixed for this group</span>}
              {!fixedSpeakers && groupRoster.length > 0 && (
                <button type="button" className="group-picker-clear" onClick={() => onGroupSpeakers([])}>clear</button>
              )}
            </div>
            {/* A fixed roster renders only its own speakers, in speaking
                order - the global profile list is a draft-mode tool. */}
            {(fixedSpeakers && fixedSpeakers.length > 0
              ? fixedSpeakers.map((id) => profiles.find((item) => item.id === id) || { id, name: id } as HarnessProfile)
              : profiles
            ).map((item) => (
              <label key={item.id} className="group-picker-row" title={item.description || item.id}>
                <input
                  type="checkbox"
                  checked={groupRoster.includes(item.id)}
                  disabled={!!fixedSpeakers}
                  onChange={() => toggleGroupSpeaker(item.id)}
                />
                <span className="group-picker-order">{groupRoster.indexOf(item.id) + 1}</span>
                <span className="group-picker-name">{item.name}</span>
              </label>
            ))}
            {!fixedSpeakers && groupRoster.length === 1 && <div className="group-picker-hint">pick at least one more speaker to open a group</div>}
          </div>
        )}
        <AttachmentPills
          attachments={attachments}
          variant="composer"
          onOpen={setOverlayIndex}
          onRemove={(index) => {
            setAttachments((previous) => previous.filter((_, item) => item !== index));
            setOverlayIndex(null);
          }}
        />
        {processingAttachments && (
          <div className="attachment-processing" role="status" aria-live="polite">
            <LoaderCircle size={13} className="attachment-spinner" /> compressing attachments...
          </div>
        )}
        {attachmentError && (
          <div className="attachment-error" role="alert">
            <CircleAlert size={13} /> {attachmentError}
          </div>
        )}
        {snippetPickerOpen && (
          <div className="snippet-picker">
            <div className="snippet-picker-head">prompt snippets - merged into this send (prepend → text → append), toggles reset after</div>
            {snippets === null && <div className="snippet-picker-row">loading snippets…</div>}
            {snippets !== null && snippets.length === 0 && (
              <div className="snippet-picker-row">no snippets. place .md files in the workspace's .agents/prompt-snippets/ (frontmatter: placement prepend|append, order).</div>
            )}
            {snippets !== null && snippets.map((snippet) => (
              <label key={snippet.name} className="snippet-picker-row">
                <input
                  type="checkbox"
                  checked={selectedSnippets.has(snippet.name)}
                  onChange={() => toggleSnippet(snippet.name)}
                />
                <span className="snippet-picker-name">{snippet.name}</span>
                <span className="snippet-picker-placement">{snippet.placement}</span>
              </label>
            ))}
          </div>
        )}
        {mentionSuggestions && mentionSuggestions.length > 0 ? (
          <div className="mention-suggestions">
            {mentionSuggestions.map((entry, index) => (
              <button
                key={entry.name}
                type="button"
                className={index === mentionHighlight ? "mention-suggestion mention-suggestion-active" : "mention-suggestion"}
                onMouseDown={(event) => { event.preventDefault(); applyMention(entry); }}
              >
                <span className="mention-suggestion-name">{entry.name}</span>
                <span className="mention-suggestion-kind">{entry.dir ? "dir" : "file"}</span>
              </button>
            ))}
          </div>
        ) : null}
        <textarea
          ref={textareaRef}
          value={value}
          onChange={(event) => {
            onChange(event.target.value);
            scheduleMentionSuggestions(event.target.value, event.target.selectionStart ?? event.target.value.length);
          }}
          onKeyDown={onKeyDown}
          onPaste={onPaste}
          placeholder={composerPlaceholder(run?.status)}
          rows={2}
          disabled={disabled}
          aria-label="Message the orchestrator"
        />
        <div className="composer-actions">
          <div className="composer-tools">
            <button
              type="button"
              className="composer-icon"
              title="attach a text file (max 200 kb)"
              onClick={() => textInputRef.current?.click()}
              disabled={disabled || processingAttachments || attachments.length >= MAX_ATTACHMENTS}
              aria-label="Attach text file"
            >
              <Paperclip size={16} />
            </button>
            <input ref={textInputRef} type="file" hidden multiple accept={TEXT_FILE_ACCEPT} onChange={(event) => void pickTextFiles(event)} />
            <button
              type="button"
              className="composer-icon"
              title={profileSupportsVision ? "attach images (jpeg, png, webp, or gif)" : VISION_UNAVAILABLE_ERROR}
              onClick={() => imageInputRef.current?.click()}
              disabled={disabled || processingAttachments || attachments.length >= MAX_ATTACHMENTS || !profileSupportsVision}
              aria-label={profileSupportsVision ? "Attach image files" : VISION_UNAVAILABLE_ERROR}
            >
              <ImageIcon size={16} />
            </button>
            <input ref={imageInputRef} type="file" hidden multiple accept={IMAGE_FILE_ACCEPT} onChange={(event) => void pickImageFiles(event)} />
            <button
              type="button"
              className="composer-icon"
              title="prompt snippets - per-message rule files merged into this send (toggles reset after send)"
              onClick={toggleSnippetPicker}
              disabled={disabled}
              aria-label="Toggle prompt snippets"
            >
              <FileText size={16} />
            </button>
            <span className="composer-hint">paste or drop images/files · text ≤ 200 kb · images ≤ 50 mb source · enter to send · shift + enter for newline</span>
          </div>
          {streaming ? (
            <button type="button" className="stop-button" onClick={onStop}>
              <CircleStop size={15} /> stop run
            </button>
          ) : (
            <button type="button" className="send-button" onClick={send} disabled={disabled || processingAttachments || (!value.trim() && attachments.length === 0)}>
              <Send size={15} /> send
            </button>
          )}
        </div>
      </div>
      <div className="composer-footer">
        <span><ShieldCheck size={12} /> local execution policy active</span>
      </div>
      <AttachmentOverlay
        attachments={attachments}
        index={overlayIndex}
        onClose={() => setOverlayIndex(null)}
        onSelect={setOverlayIndex}
      />
    </div>
  );
}

// Inspector body section with a collapsible heading: the chevron on the
// right toggles the body, and the badge (count / status) stays visible when
// collapsed, so the operator can scan every section's state at a glance.
function InspectorSection({ title, badge, collapsed, onToggle, className = "", children }: {
  title: string;
  badge?: ReactNode;
  collapsed: boolean;
  onToggle: () => void;
  className?: string;
  children: ReactNode;
}) {
  return (
    <section className={`inspector-section${className ? ` ${className}` : ""}${collapsed ? " section-collapsed" : ""}`}>
      <div
        className="section-heading section-heading-toggle"
        role="button"
        tabIndex={0}
        aria-expanded={!collapsed}
        onClick={onToggle}
        onKeyDown={(event) => { if (event.key === "Enter" || event.key === " ") { event.preventDefault(); onToggle(); } }}
      >
        <span>{title}</span>
        {badge !== undefined && <span className="section-badge">{badge}</span>}
        {collapsed ? <ChevronRight size={12} className="section-chevron" /> : <ChevronDown size={12} className="section-chevron" />}
      </div>
      {!collapsed && children}
    </section>
  );
}

function RunInspector({
  run,
  events,
  artifacts,
  onApproval,
  onAnswerInput,
  onAnswerSudo,
  sessionPackRevision,
  session,
  onLinkVault,
  onUnlinkVault,
  onListFs,
  profiles,
  sources,
  onSelectProfile,
  onCompactContext,
  onRenameSession,
  onSetSessionTags,
  onRetitleSession,
  onGetJourney,
  onRequestJourneyUpdate,
}: {
  run: HarnessRun | null;
  events: HarnessEvent[];
  artifacts: HarnessSnapshot["artifacts"];
  onApproval: (decision: "allow_once" | "allow_turn" | "allow_session" | "allow_always" | "deny") => void;
  onAnswerInput?: (choice: number) => void;
  onAnswerSudo?: (password: string, scope: string) => void;
  // The pack revision this session is pinned to (0/undefined = legacy
  // identity prompt or never pinned). Pack edits apply to new sessions.
  sessionPackRevision?: number;
  session?: HarnessSession;
  onLinkVault?: (path: string) => Promise<void> | void;
  onUnlinkVault?: () => Promise<void> | void;
  onListFs?: (path: string) => Promise<FsListing>;
  profiles: HarnessProfile[];
  sources: SourceInfo[];
  onSelectProfile: (profileId: string) => void;
  onCompactContext: () => void;
  onRenameSession?: (title: string) => Promise<void> | void;
  onSetSessionTags?: (tags: string[]) => Promise<void> | void;
  onRetitleSession?: () => Promise<void> | void;
  onGetJourney?: (sessionId: string) => Promise<JourneyInfo | null>;
  onRequestJourneyUpdate?: (sessionId: string) => Promise<void>;
}) {
  const contextPercent = run && run.metrics.contextWindow > 0 ? Math.min(100, Math.round((run.metrics.contextUsed / run.metrics.contextWindow) * 100)) : 0;
  // Collapsible sections: the operator folds what they are not acting on;
  // the state is intentionally not persisted (a fresh inspector opens
  // everything expanded).
  const [collapsedSections, setCollapsedSections] = useState<Record<string, boolean>>({});
  const toggleSection = (key: string) => setCollapsedSections((previous) => ({ ...previous, [key]: !previous[key] }));
  const [vaultPath, setVaultPath] = useState("");
  // Session details: the title draft syncs with the session (after saves and
  // AI retitles) but never fights the user while they type - the prop only
  // changes when the daemon confirms a new title.
  const [titleDraft, setTitleDraft] = useState(session?.title ?? "");
  const [titleSaving, setTitleSaving] = useState(false);
  const [retiling, setRetiling] = useState(false);
  useEffect(() => {
    setTitleDraft(session?.title ?? "");
  }, [session?.title]);
  const saveTitle = () => {
    const value = titleDraft.trim();
    if (!value || value === session?.title) return;
    setTitleSaving(true);
    void Promise.resolve(onRenameSession?.(value)).finally(() => setTitleSaving(false));
  };
  const retitle = () => {
    setRetiling(true);
    void Promise.resolve(onRetitleSession?.()).finally(() => setRetiling(false));
  };
  // Tags: the draft is a comma-separated string (the daemon accepts up to 12
  // labels of 32 chars); the sync effect mirrors the title draft behavior.
  const [tagsDraft, setTagsDraft] = useState((session?.tags ?? []).join(","));
  const [tagsSaving, setTagsSaving] = useState(false);
  useEffect(() => {
    setTagsDraft((session?.tags ?? []).join(","));
  }, [session?.tags]);
  const saveTags = () => {
    const value = tagsDraft.split(",").map((tag) => tag.trim()).filter(Boolean);
    if (value.join(",") === (session?.tags ?? []).join(",")) return;
    setTagsSaving(true);
    void Promise.resolve(onSetSessionTags?.(value)).finally(() => setTagsSaving(false));
  };
  // Journey excerpt (docs/memory.md): the historian's narrative for this
  // session. Loads on open; "refresh" enqueues a historian job and polls
  // for the rewritten document (updated_at flip) for up to ~18s.
  const [journey, setJourney] = useState<JourneyInfo | null | undefined>(undefined);
  const [journeyRefreshing, setJourneyRefreshing] = useState(false);
  useEffect(() => {
    const sid = session?.id;
    if (!sid || !onGetJourney) return;
    let cancelled = false;
    setJourney(undefined);
    onGetJourney(sid)
      .then((info) => { if (!cancelled) setJourney(info); })
      .catch(() => { if (!cancelled) setJourney(null); });
    return () => { cancelled = true; };
  }, [session?.id, onGetJourney]);
  const refreshJourney = () => {
    const sid = session?.id;
    if (!sid || !onGetJourney || !onRequestJourneyUpdate || journeyRefreshing) return;
    setJourneyRefreshing(true);
    const startedAt = Date.now();
    const previousUpdated = journey?.updatedAt ?? "";
    const poll = () => {
      onGetJourney(sid)
        .then((info) => {
          if (info) setJourney(info);
          if (info && info.updatedAt !== previousUpdated || Date.now() - startedAt > 18000) {
            setJourneyRefreshing(false);
          } else {
            window.setTimeout(poll, 3000);
          }
        })
        .catch(() => setJourneyRefreshing(false));
    };
    void onRequestJourneyUpdate(sid)
      .then(() => window.setTimeout(poll, 3000))
      .catch(() => setJourneyRefreshing(false));
  };
  const [fsSuggestions, setFsSuggestions] = useState<FsEntry[] | null>(null);
  const [fsHighlight, setFsHighlight] = useState(0);
  const fsRequest = useRef(0);
  const fsTimer = useRef<number | undefined>(undefined);
  const handleLinkVault = () => {
    const path = vaultPath.trim();
    if (!path) return;
    void Promise.resolve(onLinkVault?.(path)).then(() => setVaultPath(""));
  };
  // Path-picker: list the typed parent directory and offer children matching
  // the current segment prefix. Debounced; stale responses are dropped via
  // the request id.
  const scheduleSuggestions = (value: string) => {
    window.clearTimeout(fsTimer.current);
    const lastSep = Math.max(value.lastIndexOf("/"), value.lastIndexOf("\\"));
    if (lastSep <= 0 || !onListFs) {
      setFsSuggestions(null);
      return;
    }
    const dir = value.slice(0, lastSep + 1);
    const prefix = value.slice(lastSep + 1).toLowerCase();
    fsTimer.current = window.setTimeout(() => {
      const requestId = ++fsRequest.current;
      onListFs(dir).then((listing) => {
        if (requestId !== fsRequest.current) return;
        const matches = listing.entries.filter((entry) => entry.name.toLowerCase().startsWith(prefix));
        setFsSuggestions(matches.length ? matches : null);
        setFsHighlight(0);
      }).catch(() => {
        if (requestId === fsRequest.current) setFsSuggestions(null);
      });
    }, 200);
  };
  const applySuggestion = (entry: FsEntry) => {
    const lastSep = Math.max(vaultPath.lastIndexOf("/"), vaultPath.lastIndexOf("\\"));
    const dir = vaultPath.slice(0, lastSep + 1);
    const sep = dir.includes("\\") ? "\\" : "/";
    setVaultPath(dir + entry.name + (entry.dir ? sep : ""));
    setFsSuggestions(null);
    setFsHighlight(0);
  };
  const closeSuggestions = () => {
    setFsSuggestions(null);
    setFsHighlight(0);
  };
  return (
    <aside className="inspector-panel">
      <div className="inspector-head">
        <div>
          <span className="eyebrow">run inspector</span>
          <h2>{run ? run.id : "no active run"}</h2>
        </div>
      </div>
      {run ? (
        <>
          <div className="run-summary-card">
            <div className="run-summary-line">
              <RunPill status={run.status} />
              <span className="run-time"><Clock3 size={13} /> {run.startedAt ? formatTime(run.startedAt) : "queued"}</span>
            </div>
            <div className="route-line"><span>route</span><strong>{run.model || "unresolved"}</strong></div>
            <div className="route-line"><span>provider</span><strong>{run.provider}</strong></div>
            <div className="route-line"><span>profile</span><strong>{run.profile}{run.profileRevision ? ` · v${run.profileRevision}` : ""}</strong></div>
            {run.profileAdopted && run.profileDrift && run.profileDrift.length > 0 && (
              <div className="route-line route-line-warn"><span>drift</span><strong>adopted v{run.profileRevision}: {run.profileDrift.join(", ")}</strong></div>
            )}
            {(() => {
              if (!sessionPackRevision) return null;
              const lastEdit = [...events].reverse().find((event) => event.type === "prompt.pack_edited");
              const editedRevision = lastEdit ? Number(lastEdit.payload?.revision || 0) : 0;
              if (editedRevision > sessionPackRevision) {
                return (
                  <div className="route-line route-line-warn">
                    <span>pack</span>
                    <strong>v{sessionPackRevision} pinned · updated to v{editedRevision} (applies to new sessions)</strong>
                  </div>
                );
              }
              return <div className="route-line"><span>pack</span><strong>v{sessionPackRevision}</strong></div>;
            })()}
          </div>

          <InspectorSection title="activity" badge={`${run.tools.length} tools`} collapsed={!!collapsedSections.activity} onToggle={() => toggleSection("activity")}>
            {run.tools.length ? <div className="inspector-tools">{run.tools.map((tool) => <ToolRow key={tool.id} tool={tool} approval={run.approval?.id === tool.id ? run.approval : undefined} input={run.input?.id === tool.id ? run.input : undefined} onApproval={onApproval} onAnswerInput={onAnswerInput} onAnswerSudo={onAnswerSudo} />)}</div> : <div className="empty-inspector"><Play size={14} /> no tool calls in this run</div>}
          </InspectorSection>

          <InspectorSection title="context pressure" badge={<span className={contextPercent >= 90 ? "context-high" : contextPercent >= 70 ? "context-warn" : undefined}>{contextPercent}%</span>} collapsed={!!collapsedSections["context-pressure"]} onToggle={() => toggleSection("context-pressure")}>
            <div className="meter"><span style={{ width: `${contextPercent}%` }} /></div>
            <div className="meter-caption"><span>{formatNumber(run.metrics.contextUsed)} in · {formatNumber(run.metrics.outputTokens)} out</span><span>{formatNumber(run.metrics.contextWindow)} max</span></div>
          </InspectorSection>

          <InspectorSection title="artifacts" badge={artifacts.length} collapsed={!!collapsedSections.artifacts} onToggle={() => toggleSection("artifacts")}>
            {artifacts.length ? artifacts.map((artifact) => <div className="artifact-row" key={artifact.name}><FileCode2 size={14} /><span><strong>{artifact.name}</strong><small>{artifact.kind} · {artifact.detail}</small></span><ArrowUpRight size={13} /></div>) : <div className="empty-inspector">artifacts appear here when produced</div>}
          </InspectorSection>

          <InspectorSection title="event ledger" badge="tail" className="event-section" collapsed={!!collapsedSections["event-ledger"]} onToggle={() => toggleSection("event-ledger")}>
            <div className="event-list">
              {events.slice(-7).reverse().map((event) => <div className="event-row" key={event.eventId}><span className="event-sequence">{event.sequence.toString().padStart(3, "0")}</span><span className="event-kind">{event.type}</span><span className="event-text">{eventText(event)}</span></div>)}
            </div>
          </InspectorSection>
        </>
      ) : (
        <>
          <div className="inspector-empty-state"><Activity size={22} /><strong>nothing running</strong><span>start a conversation to inspect its live event stream.</span></div>
        </>
      )}
      {session && (
        <InspectorSection title="session details" badge={`${session.messageCount} messages`} className="details-section" collapsed={!!collapsedSections["session-details"]} onToggle={() => toggleSection("session-details")}>
          <div className="details-row">
            <span>title</span>
            <div className="details-title">
              <input
                value={titleDraft}
                onChange={(event) => setTitleDraft(event.target.value)}
                onKeyDown={(event) => { if (event.key === "Enter") saveTitle(); }}
                disabled={!onRenameSession}
              />
              <button className="secondary-button" onClick={saveTitle} disabled={titleSaving || !titleDraft.trim() || titleDraft === session.title}>{titleSaving ? "saving" : "save"}</button>
              <button className="secondary-button" onClick={retitle} disabled={retiling} title="ask the model for a fresh title (applies on the next run)"><Sparkles size={13} className={retiling ? "spin" : undefined} /></button>
            </div>
          </div>
          <div className="details-row">
            <span>tags</span>
            <div className="details-title">
              <input
                value={tagsDraft}
                onChange={(event) => setTagsDraft(event.target.value)}
                onKeyDown={(event) => { if (event.key === "Enter") saveTags(); }}
                placeholder="comma-separated labels"
                disabled={!onSetSessionTags}
              />
              <button className="secondary-button" onClick={saveTags} disabled={tagsSaving || tagsDraft.split(",").map((tag) => tag.trim()).filter(Boolean).join(",") === (session.tags ?? []).join(",")}>{tagsSaving ? "saving" : "save"}</button>
            </div>
          </div>
          <div className="details-row">
            <span>profile</span>
            <select value={session.profile} onChange={(event) => onSelectProfile(event.target.value)}>
              {profiles.map((profile) => <option key={profile.id} value={profile.id}>{profile.name}</option>)}
            </select>
          </div>
          <div className="details-row">
            <span>context</span>
            <div className="details-context">
              <span>{session.contextMode || "selected"} · {session.contextStatus || "ready"}{session.contextWatermark ? ` · ${session.contextWatermark} folded` : ""}</span>
              <button className="secondary-button" onClick={onCompactContext} title="compress the conversation now">compress</button>
            </div>
          </div>
          {session.sourceSessionId && <div className="details-row"><span>fork</span><span>from {session.sourceSessionId}</span></div>}
          {session.parentSessionId && <div className="details-row"><span>spawned from</span><span>{session.parentSessionId} (worker session - objective only, no transcript inheritance)</span></div>}
          {session.packSnapshotId ? <div className="details-row"><span>pack</span><span>snapshot #{session.packSnapshotId} · v{session.packRevision}</span></div> : null}
        </InspectorSection>
      )}
      {session && (
        <InspectorSection title="web sources" badge={sources.length} className="sources-section" collapsed={!!collapsedSections["web-sources"]} onToggle={() => toggleSection("web-sources")}>
          {sources.length ? sources.map((source) => (
            <div className="source-row" key={source.id}>
              <span className="source-id">[src:{source.id}]</span>
              <a className="source-url" href={source.url} target="_blank" rel="noreferrer" title={source.url}>{source.title || source.url}</a>
              <small>{source.status}{source.used ? " · cited" : ""}</small>
            </div>
          )) : <div className="empty-inspector"><Globe size={14} /> no web fetches in this session</div>}
        </InspectorSection>
      )}
      {session && (
        <InspectorSection title="vault mirror" badge={session.mdLogPath ? "linked" : "unlinked"} className="vault-section" collapsed={!!collapsedSections["vault-mirror"]} onToggle={() => toggleSection("vault-mirror")}>
          {session.mdLogPath ? <a className="vault-path" href={"file://" + session.mdLogPath.replace(/\\/g, "/")} target="_blank" rel="noreferrer" title={"open " + session.mdLogPath}>{session.mdLogPath}</a> : <div className="vault-hint">appends this session's turns to a vault note (Obsidian callouts)</div>}
          <div className="vault-controls">
            <div className="vault-field">
              <input
                type="text"
                className="vault-input"
                placeholder="path to an existing vault note (.md)"
                value={vaultPath}
                onChange={(event) => { setVaultPath(event.target.value); scheduleSuggestions(event.target.value); }}
                onKeyDown={(event) => {
                  if (fsSuggestions && fsSuggestions.length > 0) {
                    if (event.key === "ArrowDown") {
                      event.preventDefault();
                      setFsHighlight((h) => (h + 1) % fsSuggestions.length);
                      return;
                    }
                    if (event.key === "ArrowUp") {
                      event.preventDefault();
                      setFsHighlight((h) => (h - 1 + fsSuggestions.length) % fsSuggestions.length);
                      return;
                    }
                    if (event.key === "Enter") {
                      event.preventDefault();
                      applySuggestion(fsSuggestions[fsHighlight]);
                      return;
                    }
                    if (event.key === "Escape") {
                      closeSuggestions();
                      return;
                    }
                  }
                  if (event.key === "Enter") handleLinkVault();
                }}
                onBlur={() => window.setTimeout(closeSuggestions, 150)}
              />
              {fsSuggestions && fsSuggestions.length > 0 ? (
                <div className="vault-suggestions">
                  {fsSuggestions.map((entry, index) => (
                    <button
                      key={entry.name}
                      type="button"
                      className={index === fsHighlight ? "vault-suggestion vault-suggestion-active" : "vault-suggestion"}
                      onMouseDown={(event) => { event.preventDefault(); applySuggestion(entry); }}
                    >
                      <span className="vault-suggestion-name">{entry.name}</span>
                      <span className="vault-suggestion-kind">{entry.dir ? "dir" : "file"}</span>
                    </button>
                  ))}
                </div>
              ) : null}
            </div>
            <button className="vault-btn" onClick={handleLinkVault} disabled={!vaultPath.trim()}>Link</button>
            {session.mdLogPath ? <button className="vault-btn vault-btn-ghost" onClick={() => void Promise.resolve(onUnlinkVault?.())}>Unlink</button> : null}
          </div>
        </InspectorSection>
      )}
      {session && (
        <InspectorSection title="journey" badge={journey ? "excerpt" : "none"} className="journey-section" collapsed={!!collapsedSections.journey} onToggle={() => toggleSection("journey")}>
          {journey === undefined ? (
            <div className="empty-inspector"><LoaderCircle size={14} className="spin" /> loading journey</div>
          ) : journey ? (
            <>
              <pre className="journey-text">{journey.text}</pre>
              <div className="journey-meta">
                <span>updated {formatTime(journey.updatedAt)} · through message {journey.watermark}</span>
                <button className="secondary-button" onClick={refreshJourney} disabled={journeyRefreshing}>{journeyRefreshing ? "refreshing" : "refresh"}</button>
              </div>
            </>
          ) : (
            <div className="empty-inspector">
              <span><BookOpen size={14} /> no journey yet. the historian writes it as runs complete</span>
              <button className="secondary-button" onClick={refreshJourney} disabled={journeyRefreshing}>{journeyRefreshing ? "refreshing" : "refresh now"}</button>
            </div>
          )}
        </InspectorSection>
      )}
    </aside>
  );
}

function ChatView({
  session,
  messages,
  run,
  events,
  client,
  onOpenSession,
  artifacts,
  status,
  draft,
  onDraft,
  onSend,
  onStop,
  onApproval,
  onAnswerInput,
  onAnswerSudo,
  onSetGroupRounds,
  onContextDecision,
  onRegenerate,
  onContinueFailedRun,
  onSelectIteration,
  onSelectProfile,
  profiles,
  providers,
  defaultProvider,
  draftProfile,
  groupDraft,
  onGroupSpeakers,
  connected,
  disconnectedDetail,
  stalled,
  inspectorOpen,
  dismissedRetryRun,
  dismissFailedRun,
  queuedPrompts,
  onQueueEdit,
  onQueueRemove,
  onQueueSteer,
  onQueueDeliverNow,
  onQueueSendNow,
  onCompactContext,
  steeringQueueIds,
  steerFlash,
  composerSeed,
  onListSnippets,
  onEdit,
  onDelete,
  onRewindHere,
  onFork,
  onSelectMessage,
  onSearch,
  onLinkVault,
  onUnlinkVault,
  onListFs,
  onExport,
  sources,
  pendingJump,
  onJumpHandled,
  onRenameSession,
  onSetSessionTags,
  onRetitleSession,
  onGetJourney,
  onRequestJourneyUpdate,
}: {
  session: HarnessSession | undefined;
  messages: HarnessMessage[];
  client: HarnessClient;
  // Jumps the main view to a worker session (from the inline subagent block).
  onOpenSession?: (sessionId: string) => void;
  run: HarnessRun | null;
  events: HarnessEvent[];
  artifacts: HarnessSnapshot["artifacts"];
  status: HarnessSessionStatus | null;
  draft: string;
  onDraft: (value: string) => void;
  onSend: (content: string, attachments: HarnessAttachment[], mentions: string[], speakers: string[]) => void;
  // Group chat: the draft speaker roster for the next send (App-owned, so a
  // new conversation resets it); the composer shows it as a fixed roster on
  // an existing group session.
  groupDraft: string[];
  onGroupSpeakers: (ids: string[]) => void;
  onStop: () => void;
  onApproval: (decision: "allow_once" | "allow_turn" | "allow_session" | "allow_always" | "deny") => void;
  onAnswerInput: (choice: number) => void;
  onAnswerSudo: (password: string, scope: string) => void;
  onSetGroupRounds: (rounds: number | null) => void;
  onContextDecision: (decision: "primary" | "deterministic" | "retry") => void;
  onRegenerate?: (runId: string) => void;
  onContinueFailedRun: (runId: string) => void;
  onSelectIteration: (runId: string) => void;
  onSelectProfile: (profileId: string) => void;
  onEdit?: (message: HarnessMessage, content: string) => void;
  onDelete?: (message: HarnessMessage) => void;
  onRewindHere?: (message: HarnessMessage) => void;
  onFork: (mode: ForkMode) => void;
  onSelectMessage: (messageId: number) => void;
  onExport: (format: "json" | "markdown" | "csv") => Promise<void>;
  onSearch: (query: string) => Promise<SearchResponse>;
  onLinkVault?: (path: string) => Promise<void> | void;
  onUnlinkVault?: () => Promise<void> | void;
  onListFs?: (path: string) => Promise<FsListing>;
  onRenameSession?: (title: string) => Promise<void> | void;
  onSetSessionTags?: (tags: string[]) => Promise<void> | void;
  onRetitleSession?: () => Promise<void> | void;
  onGetJourney?: (sessionId: string) => Promise<JourneyInfo | null>;
  onRequestJourneyUpdate?: (sessionId: string) => Promise<void>;
  sources: SourceInfo[];
  pendingJump: number | null;
  onJumpHandled: () => void;
  profiles: HarnessProfile[];
  providers: HarnessProvider[];
  defaultProvider: string;
  draftProfile: string;
  connected: boolean;
  disconnectedDetail: string | null;
  stalled: boolean;
  inspectorOpen: boolean;
  dismissedRetryRun: readonly string[];
  dismissFailedRun: (runId: string | null) => void;
  queuedPrompts: QueuedPrompt[];
  onQueueEdit: (item: QueuedPrompt) => void;
  onQueueRemove: (item: QueuedPrompt) => void;
  onQueueSteer: (item: QueuedPrompt) => void;
  onQueueDeliverNow: (item: QueuedPrompt) => void;
  onQueueSendNow: () => void;
  onCompactContext: () => void;
  steeringQueueIds: ReadonlySet<string>;
  steerFlash: { text: string; key: number } | null;
  composerSeed: ComposerSeed | null;
  onListSnippets: () => Promise<PromptSnippet[]>;
}) {
  const streaming = !!run && isRunActive(run.status);
  // Worker sessions are the model's to drive: the user can talk to them, but
  // editing/deleting/regenerating/rewinding the worker's turns from outside
  // forks the transcript the parent's harvest reads back.
  const workerSession = !!session?.parentSessionId;
  const sessionHandoffPending = !connected &&
    (disconnectedDetail === "loading session snapshot" || disconnectedDetail === "opening event stream");
  const path = getDisplayPath(messages);

  // ── viewport ────────────────────────────────────────────────────────────
  // Opening a chat lands on the latest turn. While the reader stays at the
  // bottom, new/streamed content keeps the tail in view; scrolling up releases
  // the pin so back-reading is never fought. Long chats only mount the tail
  // window; scrolling to the top loads earlier turns (anchored).
  const scrollRef = useRef<HTMLDivElement>(null);
  const anchorRef = useRef<{ top: number; height: number } | null>(null);
  const [pinned, setPinned] = useState(true);
  const [frozenStart, setFrozenStart] = useState<number | null>(null);
  // The banner's rounds-control popover (group sessions).
  const [roundsOpen, setRoundsOpen] = useState(false);
  const [roundsDraft, setRoundsDraft] = useState(3);
  // Switching sessions resets the viewport (render-time state adjustment, the
  // documented pattern - avoids a setState-in-effect).
  const [windowSession, setWindowSession] = useState(session?.id);
  if (windowSession !== session?.id) {
    // Per-session reading position: the scroller still holds the OLD
    // session's content at this render-time
    // reset, so capture before switching. Restoration only applies when the
    // reader was NOT at the bottom (mid-transcript); near-bottom returns
    // stay pinned like everything else. LRU-capped.
    const scroller = scrollRef.current;
    if (scroller && windowSession) {
      const top = scroller.scrollTop;
      const nearBottom = scroller.scrollHeight - top - scroller.clientHeight < 80;
      if (!nearBottom) {
        sessionScrollMemory.set(windowSession, top);
        if (sessionScrollMemory.size > 200) {
          const oldest = sessionScrollMemory.keys().next().value;
          if (oldest !== undefined) sessionScrollMemory.delete(oldest);
        }
      } else {
        sessionScrollMemory.delete(windowSession);
      }
    }
    setWindowSession(session?.id);
    setFrozenStart(null);
    setPinned(true);
  }
  const pathLength = path.length;
  const lastMessage = path[pathLength - 1];
  const tailSignal = `${pathLength}:${lastMessage?.id ?? 0}:${lastMessage?.content?.length ?? 0}:${lastMessage?.reasoning?.length ?? 0}`;
  const windowStart = frozenStart === null
    ? Math.max(0, pathLength - CHAT_WINDOW_SIZE)
    : Math.min(frozenStart, pathLength);
  const visiblePath = path.slice(windowStart);
  const activityRun = run && run.tools.length > 0 ? run : null;
  // The anchor scan spans the FULL display path, not the visible window: a
  // run streaming while the viewport is pinned to older turns used to miss
  // its anchor and float its tool rows as a detached bar between turns.
  const activityAnchorId = activityRun
    ? path.find((message) => message.role === "assistant" && message.runId === activityRun.id)?.id
    : undefined;

  // Restore the previous session's reading position after the new content
  // commits (layout effect: before the browser paints).
  useLayoutEffect(() => {
    if (windowSession === undefined) return;
    const top = sessionScrollMemory.get(windowSession);
    if (top === undefined) return;
    sessionScrollMemory.delete(windowSession);
    const el = scrollRef.current;
    if (el && window.length) {
      setPinned(false);
      requestAnimationFrame(() => {
        el.scrollTop = Math.min(Math.max(0, top), Math.max(0, el.scrollHeight - el.clientHeight));
      });
    }
  }, [windowSession]);

  // Keep the tail in view while the reader is pinned to the bottom (opening a
  // chat, sending, streaming). Instant, not smooth: repeated smooth scrolls
  // on every streamed delta would fight each other. The scroll lands in one
  // rAF frame - a per-delta synchronous layout write makes offscreen
  // content-visibility blocks invalidate the whole scroller each time, and
  // their settling shifts the bottom: a second frame re-lands on the true
  // end instead of fighting (a two-frame settle).
  const tailScrollRef = useRef<number | undefined>(undefined);
  useEffect(() => {
    if (!pinned) return;
    const el = scrollRef.current;
    if (!el) return;
    const land = () => {
      el.scrollTop = el.scrollHeight;
    };
    if (tailScrollRef.current !== undefined) {
      window.cancelAnimationFrame(tailScrollRef.current);
      tailScrollRef.current = undefined;
    }
    tailScrollRef.current = window.requestAnimationFrame(() => {
      land();
      tailScrollRef.current = window.requestAnimationFrame(land);
    });
  }, [tailSignal, pinned]);
  useEffect(() => () => {
    if (tailScrollRef.current !== undefined) window.cancelAnimationFrame(tailScrollRef.current);
  }, []);

  // Preserve the reading position when earlier turns are prepended.
  useLayoutEffect(() => {
    const anchor = anchorRef.current;
    if (!anchor) return;
    anchorRef.current = null;
    const el = scrollRef.current;
    if (el) el.scrollTop = el.scrollHeight - anchor.height + anchor.top;
  }, [windowStart]);

  const loadEarlier = () => {
    if (windowStart === 0) return;
    const el = scrollRef.current;
    if (el) anchorRef.current = { top: el.scrollTop, height: el.scrollHeight };
    setFrozenStart(Math.max(0, windowStart - CHAT_WINDOW_SIZE));
  };

  const handleScroll = () => {
    const el = scrollRef.current;
    if (!el) return;
    setPinned(el.scrollHeight - el.scrollTop - el.clientHeight < 80);
    if (el.scrollTop < 60 && windowStart > 0) loadEarlier();
  };

  // Sending always jumps to the newest turn and re-arms the bottom pin.
  const handleSend = (content: string, attachments: HarnessAttachment[], mentions: string[], speakers: string[]) => {
    setPinned(true);
    onSend(content, attachments, mentions, speakers);
  };
  // Forking copies the active path, so it needs a fully persisted path (no
  // live temp ids) and no run in flight. Persisted rows carry daemon
  // snowflake ids (positive); client temp rows stay negative.
  const canFork = path.length > 0 && !streaming && connected && path.every((message) => message.id > 0);

  // ── in-chat jumps ─────────────────────────────────────────────────────
  // A jump to a message outside the mounted tail window first widens the
  // window, then scrolls once the anchor has rendered (rAF retries cover
  // the mount frame). The graph view hands off here via pendingJump; the
  // branch switch behind it is a server round-trip, so if the message is
  // not on the active path yet, the effect re-runs when the path rebuilds.
  const [jumpTarget, setJumpTarget] = useState<number | null>(null);
  useEffect(() => {
    if (pendingJump === null) return;
    const index = path.findIndex((message) => message.id === pendingJump);
    if (index < 0) return;
    const raf = requestAnimationFrame(() => {
      if (index < windowStart) setFrozenStart(Math.max(0, index - 5));
      setPinned(false);
      setJumpTarget(pendingJump);
      onJumpHandled();
    });
    return () => cancelAnimationFrame(raf);
  }, [pendingJump, path, windowStart, onJumpHandled]);
  useEffect(() => {
    if (jumpTarget === null) return;
    let cancelled = false;
    let frames = 0;
    const tryScroll = () => {
      if (cancelled) return;
      const el = document.getElementById(`turn-${jumpTarget}`);
      if (el) {
        el.scrollIntoView({ behavior: "smooth", block: "start" });
        setJumpTarget(null);
        return;
      }
      if (++frames > 40) {
        setJumpTarget(null);
        return;
      }
      requestAnimationFrame(tryScroll);
    };
    requestAnimationFrame(tryScroll);
    return () => { cancelled = true; };
  }, [jumpTarget]);
  // The event window's identity changes on every streamed delta; the
  // derivations below only read non-delta events, so gate them on the
  // window's non-delta hash (see ledgerSignature) to keep their identities -
  // and every memoized TurnBlock prop they feed - stable across a burst.
  const ledgerSig = useMemo(() => ledgerSignature(events), [events]);
  const runEventCounts = useMemo(() => {
    const counts = new Map<string, number>();
    for (const event of events) counts.set(event.runId, (counts.get(event.runId) || 0) + 1);
    return counts;
  }, [events]);
  const interruptedRuns = useStableDerived(ledgerSig, () => {
    const map = new Map<string, { kind: "failed" | "cancelled"; error: string; errorKind?: string; errorHint?: string }>();
    for (const event of events) {
      if (event.type === "run.failed") {
        map.set(event.runId, { kind: "failed", error: String(event.payload.error || ""),
          errorKind: event.payload.kind ? String(event.payload.kind) : undefined,
          errorHint: event.payload.hint ? String(event.payload.hint) : undefined });
      } else if (event.type === "run.cancelled") {
        map.set(event.runId, { kind: "cancelled", error: "" });
      }
    }
    return map;
  });
  // Resume-after-error pairings (run.continued): a continuation turn renders
  // inside the interrupted turn's visual block instead of a second profile
  // header repeating the same speaker.
  const continuedFrom = useMemo(() => {
    const map = new Map<string, string>();
    for (const event of events) {
      if (event.type === "run.continued" && String(event.payload.failed || "")) {
        map.set(event.runId, String(event.payload.failed));
      }
    }
    return map;
  }, [events]);
  // Per-run details for the info indicator on each assistant turn (model,
  // profile, usage, duration), rebuilt from the event ledger like the
  // inspector does, so it survives a page reload.
  const runInfo = useStableDerived(ledgerSig, () => (session ? runsFromEvents(events, session.id) : undefined));
  // Latest task-checklist state, replayed from the ledger (tasks.created
  // replaces, tasks.updated toggles) but scoped to the ACTIVE path's runs:
  // the ledger is append-only, so an unscoped replay resurrects a list
  // whose creating turn was rewound - the ghost did not die with the
  // transcript. The active-run set comes from the displayed path; a rewind
  // reloads the projection, the set shrinks, the replay drops the rewound
  // run's events, and the list dies with its turn. While the list is live
  // the card is pinned at the bottom; once every item is done it lingers
  // in the transcript where the last tick landed (taskCompleteAt), so the
  // record sits at the work's location; a tasks.create [] discard empties
  // the list and keeps the card at the discard marker (red Xs on the items
  // that never ran).
  const activeRunIds = useMemo(
    () => new Set(path.map((message) => message.runId).filter((id): id is string => !!id)),
    [path],
  );
  const tasksSig = (ledgerSig + hashString([...activeRunIds].sort().join("\u0000"))) | 0;
  const tasksNow = useStableDerived(tasksSig, () => tasksFromEvents(events, activeRunIds));
  // Checklists that closed in the ledger: each lingers at its own location -
  // a completed list at its last tick (all green), a discarded list at the
  // discard marker (red Xs on the items that never ran). Independent records,
  // so a later create/discard never removes an earlier card.
  const taskRecords = useStableDerived(ledgerSig, () => taskChecklistRecords(events));
  const tasksComplete = tasksNow.length > 0 && tasksNow.every((task) => task.done);
  // Group chat (docs/group-chat.md): the roundtable state replayed from the
  // ledger (roster, live round, current speaker, termination), plus the
  // speaker display names for attribution. A group session's transcript is
  // rendered from groupTranscriptItems so recorded abstentions land at their
  // turn's position between committed messages.
  const groupSpeakers = session?.speakers && session.speakers.length > 0 ? session.speakers : undefined;
  const groupState = useStableDerived(ledgerSig, () => groupStateFromEvents(events));
  const speakerNames = useMemo(() => {
    const names = new Map<string, string>();
    for (const profile of profiles) names.set(profile.id, profile.name);
    return names;
  }, [profiles]);
  const speakerNameFor = (id?: string) => (id ? speakerNames.get(id) || id : undefined);
  const groupItems: GroupTranscriptItem[] = useMemo(
    () => (groupSpeakers ? groupTranscriptItems(visiblePath, events) : []),
    [groupSpeakers, visiblePath, events],
  );
  // Context pressure: the latest run's prompt tokens against the provider
  // window. The per-run metrics strip shows this per turn; the toolbar chip
  // tracks the session's most recent state and hosts "compress now".
  const contextPressure = useMemo(() => {
    const candidates: HarnessRun[] = runInfo ? [...runInfo.values()] : [];
    if (run && run.metrics.contextUsed > 0 && run.metrics.contextWindow > 0) {
      candidates.push(run);
    }
    const stamp = (value?: string) => Date.parse(value || "") || 0;
    let latest: HarnessRun | null = null;
    for (const candidate of candidates) {
      if (candidate.metrics.contextWindow <= 0 || candidate.metrics.contextUsed <= 0) continue;
      if (!latest) {
        latest = candidate;
        continue;
      }
      if (stamp(candidate.finishedAt || candidate.startedAt) >= stamp(latest.finishedAt || latest.startedAt)) {
        latest = candidate;
      }
    }
    if (!latest) return null;
    return { used: latest.metrics.contextUsed, window: latest.metrics.contextWindow };
  }, [run, runInfo]);
  return (
    <div className={`chat-layout ${inspectorOpen ? "chat-layout-inspector" : ""}`}>
      <section className="conversation-panel">
        <div className="conversation-toolbar">
          <div>
            <span className="eyebrow">primary thread</span>
            <div className="conversation-title-line"><h1>{session?.title || "new conversation"}</h1><span className="thread-state"><span /> persistent</span><StatusChip state={status?.latest || ""} recordedAt={status?.events[0]?.createdAt} /></div>
          </div>
          <div className="conversation-toolbar-actions">
            {contextPressure && (
              <div className="context-meter" title={`context pressure: latest run used ${formatCompact(contextPressure.used)} of ${formatCompact(contextPressure.window)} tokens`}>
                <span className={`context-meter-dot ${contextPressure.used / contextPressure.window > 0.9 ? "high" : contextPressure.used / contextPressure.window > 0.7 ? "warn" : ""}`} />
                <span className="context-meter-text">ctx {formatCompact(contextPressure.used)}/{formatCompact(contextPressure.window)}</span>
                <button
                  type="button"
                  className="context-compress"
                  onClick={onCompactContext}
                  disabled={streaming || session?.contextStatus === "preparing" || !!session?.contextPrompt}
                  title="compress now: fold the oldest turns into the checkpoint before the next turn"
                >
                  <Shrink size={12} />
                  compress
                </button>
              </div>
            )}
            <ExportMenu onExport={onExport} disabled={path.length === 0} />
            <ForkMenu canFork={canFork} onFork={onFork} />
          </div>
        </div>
        <div className="conversation-scroll" ref={scrollRef} onScroll={handleScroll}>
          {session?.contextMode === "summary" && session.contextStatus === "preparing" && (
            <div className="context-preparing-notice" role="status">
              <Clock3 size={14} /> compact context is preparing; new turns remain queued
            </div>
          )}
          {session?.contextPrompt && (
            <CompactionPromptCard prompt={session.contextPrompt} onDecide={onContextDecision} />
          )}
          {session?.contextMode === "summary" && session.contextStatus === "failed" && (
            <div className="context-failed-notice" role="alert">
              <CircleAlert size={14} /> compact context failed: {session.contextError || "preparation error"}
            </div>
          )}
          {session?.contextMode === "summary" && session.contextStatus === "ready" && session.contextSummary && (
            <CompactedSummaryBlock
              summary={session.contextSummary}
              onSearch={onSearch}
            />
          )}
          {path.length === 0 && <div className="conversation-intro"><div className="intro-line"><span className="intro-rule" /><span>new conversation</span><span className="intro-rule" /></div><p>the harness owns the run. the model owns the answer. this stream owns the truth between them.</p></div>}
          {groupSpeakers && (
            <div className={`group-banner ${groupState.active ? "group-banner-active" : ""}`} role="status">
              <Users size={13} />
              <span className="group-banner-roster">
                {groupSpeakers.map((speaker, index) => (
                  <span key={speaker} className={groupState.currentSpeaker === speaker ? "group-speaker group-speaker-live" : "group-speaker"}>
                    {speakerNameFor(speaker) || speaker}{index < groupSpeakers.length - 1 ? " ·" : ""}
                  </span>
                ))}
              </span>
              <span className="group-banner-state">
                {groupState.active
                  ? `round ${Math.max(1, groupState.round)}/${groupState.maxRounds || "?"}${groupState.prepared
                    ? ` · ${speakerNameFor(groupState.prepared) || groupState.prepared} is preparing`
                    : groupState.currentSpeaker ? ` · ${speakerNameFor(groupState.currentSpeaker) || groupState.currentSpeaker} is speaking` : ""}`
                  : `ended · ${groupState.termination || "stopped"} · ${groupState.round} round${groupState.round === 1 ? "" : "s"}`}
              </span>
              {/* The round budget's per-session control: a slider (1-6) that
                  overrides group.max_rounds for THIS group, or "auto" for the
                  global setting. Takes effect from the next group run. */}
              <div className="group-rounds">
                <button
                  type="button"
                  className={`group-rounds-chip ${roundsOpen ? "group-rounds-open" : ""}`}
                  title="rounds per group run: how many full passes the speakers get - 1-2 for quick answers, 5-6 for an extended discussion"
                  onClick={() => {
                    if (!roundsOpen) setRoundsDraft(session?.groupRoundsOverride ?? Math.max(1, groupState.maxRounds || 3));
                    setRoundsOpen((open) => !open);
                  }}
                >
                  rounds {session?.groupRoundsOverride ?? "auto"}
                </button>
                {roundsOpen && (
                  <div className="group-rounds-panel" role="group" aria-label="group rounds">
                    <input
                      type="range"
                      min={1}
                      max={6}
                      step={1}
                      value={roundsDraft}
                      onChange={(event) => setRoundsDraft(Number(event.target.value))}
                    />
                    <span className="group-rounds-value">{roundsDraft}</span>
                    <button type="button" onClick={() => { onSetGroupRounds(roundsDraft); setRoundsOpen(false); }}>apply</button>
                    <button type="button" className="group-rounds-auto" onClick={() => { onSetGroupRounds(null); setRoundsOpen(false); }}>auto</button>
                  </div>
                )}
              </div>
            </div>
          )}
          {groupSpeakers
            ? groupItems.map((item) => {
                if (item.kind === "abstained") {
                  return (
                    <div key={`abstain-${item.turnRun}`} className="abstain-row" role="note">
                      <span className="abstain-dot" />
                      <span>{speakerNameFor(item.speaker) || item.speaker || "a speaker"} abstained - nothing to add</span>
                    </div>
                  );
                }
                const message = item.message;
                const messageActivity = activityRun && activityAnchorId === message.id ? activityRun : undefined;
                const derived = derivedForMessage(session?.id || "", message, events, runEventCounts);
                return (
                <div key={message.id} id={`turn-${message.id}`} className="turn-anchor">
                  <TurnBlock
                    message={message}
                    allMessages={messages}
                    profileName={session?.profile}
                    speakerName={message.role === "assistant" ? speakerNameFor(message.speaker) : undefined}
                    onSelectIteration={onSelectIteration}
                    onSelectMessage={onSelectMessage}
                    onRegenerate={workerSession ? undefined : onRegenerate}
                    onEdit={workerSession ? undefined : onEdit}
                    onDelete={workerSession ? undefined : onDelete}
                    onRewindHere={workerSession ? undefined : onRewindHere}
                    canRewind={!streaming}
                    canRegenerate={!streaming}
                    interruptedRuns={interruptedRuns}
                    runInfo={runInfo}
                    reasoningSegments={derived.segments}
                    blocks={derived.blocks}
                    activityRun={messageActivity}
                    continued={continuedFrom.has(String(message.runId || ""))}
                    onApproval={onApproval}
                    onAnswerInput={onAnswerInput}
                    onAnswerSudo={onAnswerSudo}
                    sources={sources}
                    tasks={tasksNow}
                    taskRecords={taskRecords}
                    subagent={{ events, client, onOpenSession }}
                  />
                </div>
                );
              })
            : visiblePath.map((message) => {
                const messageActivity = activityRun && activityAnchorId === message.id ? activityRun : undefined;
                const derived = derivedForMessage(session?.id || "", message, events, runEventCounts);
                const messageReasoning = derived.segments;
                const messageBlocks = derived.blocks;
                return (
                <div key={message.id} id={`turn-${message.id}`} className="turn-anchor">
                  <TurnBlock
                    message={message}
                    allMessages={messages}
                    profileName={session?.profile}
                    onSelectIteration={onSelectIteration}
                    onSelectMessage={onSelectMessage}
                    onRegenerate={workerSession ? undefined : onRegenerate}
                    onEdit={workerSession ? undefined : onEdit}
                    onDelete={workerSession ? undefined : onDelete}
                    onRewindHere={workerSession ? undefined : onRewindHere}
                    canRewind={!streaming}
                    canRegenerate={!streaming}
                    interruptedRuns={interruptedRuns}
                    runInfo={runInfo}
                    reasoningSegments={messageReasoning}
                    blocks={messageBlocks}
                    activityRun={messageActivity}
                    continued={continuedFrom.has(String(message.runId || ""))}
                    onApproval={onApproval}
                    onAnswerInput={onAnswerInput}
                    onAnswerSudo={onAnswerSudo}
                    sources={sources}
                    tasks={tasksNow}
                    taskRecords={taskRecords}
                    subagent={{ events, client, onOpenSession }}
                  />
                </div>
                );
              })}
          {/* Standalone tool group only while the run is in flight (no
              persisted assistant message to anchor it yet). A completed run
              whose message was trimmed by a rewind has no anchor and must not
              float its tool calls - that is the stale-`run` visual. */}
          {activityRun && activityAnchorId === undefined && isRunActive(activityRun.status) && <ActivityGroup run={activityRun} onApproval={onApproval} onAnswerInput={onAnswerInput} onAnswerSudo={onAnswerSudo} />}
          {/* Pinned only while the checklist is live (0/N .. n/N): once
              every item is done the card lingers in the transcript at the
              last tick, and a tasks.create [] discard keeps the card at the
              discard marker with red Xs on the items that never ran - both
              rendered from taskRecords, which also keeps any earlier
              completed/discarded card in place. */}
          {tasksNow.length > 0 && !tasksComplete && (
            <div className="task-card-anchor">
              <TaskListCard tasks={tasksNow} />
            </div>
          )}
          {streaming && (() => {
            const workerLabel = run?.profile || session?.profile || "orchestrator";
            // While the summary context is being rebuilt, a queued turn shows
            // the compaction rather than the bare slot-wait copy.
            const compacting = session?.contextMode === "summary" && session.contextStatus === "preparing";
            const baseLabel =
              run?.status === "queued" ? (compacting ? "compacting context" : "waiting for a generation slot")
              : run?.status === "waiting_approval" ? `waiting for approval: ${run.approval?.name || "tool"}`
              : run?.status === "waiting_input" ? "waiting for your answer"
              : `${workerLabel} is working`;
            const resumed = run && continuedFrom.has(String(run.id));
            const toolCount = run?.tools.length || 0;
            // The resumed marker rides the pill text itself while the
            // continuation has no row yet (a refresh before the first
            // delta): a separate tag element would be a foreign font size
            // next to the pill copy.
            return (
              <div className="streaming-line">
                <span className="pulse-ring" />
                <span>{resumed ? `resumed · ${baseLabel}` : baseLabel}</span>
                {toolCount > 0 && <span className="streaming-tools">· {toolCount} {toolCount === 1 ? "tool" : "tools"}</span>}
                <LiveElapsed startedAt={run?.startedAt} />
                <span className="streaming-esc">esc to interrupt</span>
                <span className="streaming-dots">...</span>
              </div>
            );
          })()}
          {session && !connected && !sessionHandoffPending && <div className="disconnected-notice"><CircleAlert size={14} /> {disconnectedDetail ? `${disconnectedDetail}, replay will resume from the last event` : "connection interrupted, replay will resume from the last event"}</div>}
        </div>
        {run && run.status === "failed" && run.sessionId === session?.id && !dismissedRetryRun.includes(run.id) && (
          <ErrorBar error={run.error || ""} errorKind={run.errorKind} errorHint={run.errorHint} onRetry={() => onContinueFailedRun(run.id)} onDismiss={() => dismissFailedRun(run.id)} />
        )}
        {run && streaming && run.retry && (
          <WarningBar text={`model endpoint unavailable: ${run.retry.reason}. retrying, ${Math.max(0, run.retry.maxAttempts - run.retry.attempt)} attempt left.`} />
        )}
        {run && streaming && stalled && (
          <WarningBar text="no events for 60s while the run is active. the endpoint may be slow or stuck." />
        )}
        {run && streaming && run.kvCacheMiss && (
          <WarningBar text={run.kvCacheMiss.kind === "regression"
            ? `prompt cache regression: the previous call was served from cache, this one re-prefilled ${run.kvCacheMiss.promptTokens} tokens.`
            : `prompt cache miss: ${run.kvCacheMiss.promptTokens} tokens re-prefilled with no cache hit. check the prompt prefix for drift.`} />
        )}
        <QueuedPrompts items={queuedPrompts} active={streaming} steeringIds={steeringQueueIds} onEdit={onQueueEdit} onRemove={onQueueRemove} onSteer={onQueueSteer} onDeliverNow={onQueueDeliverNow} onSendNow={onQueueSendNow} />
        {steerFlash && (
          <div className="steer-flash" key={steerFlash.key} role="status">
            <Route size={12} /> {steerFlash.text}
          </div>
        )}
        <Composer value={draft} onChange={onDraft} onSend={handleSend} onStop={onStop} disabled={!!session && !connected} streaming={streaming} profile={session?.profile || draftProfile} model={run?.model || session?.model || "local model"} profiles={profiles} providers={providers} defaultProvider={defaultProvider} onSelectProfile={onSelectProfile} run={run} seed={composerSeed && composerSeed.sessionId === session?.id ? composerSeed : null} onListFs={onListFs} onListSnippets={onListSnippets} sessionKey={session?.id || "draft"} groupSpeakers={groupDraft} onGroupSpeakers={onGroupSpeakers} fixedSpeakers={session?.speakers} />
      </section>
      {inspectorOpen && <RunInspector run={run} events={events} artifacts={artifacts} onApproval={onApproval} onAnswerInput={onAnswerInput} onAnswerSudo={onAnswerSudo} sessionPackRevision={session?.packRevision} session={session} onLinkVault={onLinkVault} onUnlinkVault={onUnlinkVault} onListFs={onListFs} profiles={profiles} sources={sources} onSelectProfile={onSelectProfile} onCompactContext={onCompactContext} onRenameSession={onRenameSession} onSetSessionTags={onSetSessionTags} onRetitleSession={onRetitleSession} onGetJourney={onGetJourney} onRequestJourneyUpdate={onRequestJourneyUpdate} />}
    </div>
  );
}

function HomeView({ sessions, run, profilesCount, onNew, onSession, onView }: {
  sessions: HarnessSession[];
  run: HarnessRun | null;
  profilesCount: number;
  onNew: () => void;
  onSession: (id: string) => void;
  onView: (view: View) => void;
}) {
  const inFlight = !!run && isRunActive(run.status);
  // Worker sessions render inline in their parent's chat, not here.
  const recent = sessions.filter((session) => !session.parentSessionId).slice(0, 4);
  const systems: { label: string; note: string; view: View; icon: LucideIcon }[] = [
    { label: "memory", note: "persistent facts and graph", view: "memory", icon: BrainCircuit },
    { label: "workspace", note: "files and artifacts", view: "workspace", icon: FolderOpen },
    { label: "observatory", note: "metrics and traces", view: "observatory", icon: BarChart3 },
    { label: "profiles", note: "personas and models", view: "profiles", icon: UserRound },
    { label: "settings", note: "runtime configuration", view: "settings", icon: SlidersHorizontal },
  ];
  return (
    <div className="standalone-view">
      <div className="standalone-heading">
        <div>
          <span className="eyebrow">nyaxus / overview</span>
          <h1>overview</h1>
          <p>a stable landing point. sessions keep their urls, so a refresh never loses them.</p>
        </div>
        <button className="secondary-button" onClick={onNew}><Plus size={15} /> new conversation</button>
      </div>
      <div className="home-stats">
        <div className="home-stat"><span className="home-stat-value">{String(sessions.length).padStart(2, "0")}</span><span className="home-stat-label">conversations</span></div>
        <div className="home-stat"><span className="home-stat-value">{inFlight ? "01" : "00"}</span><span className="home-stat-label">runs in flight</span></div>
        <div className="home-stat"><span className="home-stat-value">{String(profilesCount).padStart(2, "0")}</span><span className="home-stat-label">profiles</span></div>
      </div>
      <div className="home-grid">
        <section className="home-card">
          <div className="home-card-head"><span>recent conversations</span><span className="panel-count">{String(sessions.length).padStart(2, "0")}</span></div>
          {recent.length === 0
            ? <div className="home-empty"><MessageSquare size={16} /> no conversations yet. start one and it lands here.</div>
            : recent.map((session) => (
              <button key={session.id} className="home-row" onClick={() => onSession(session.id)}>
                <span className="home-row-title">{session.title}</span>
                <span className="home-row-meta"><MessageSquare size={12} /> {session.messageCount} messages · {formatTime(session.updatedAt)}</span>
                <ArrowUpRight size={14} />
              </button>
            ))}
        </section>
        <section className="home-card">
          <div className="home-card-head"><span>systems</span><span className="panel-count">{String(systems.length).padStart(2, "0")}</span></div>
          {systems.map((item) => (
            <button key={item.view} className="home-row" onClick={() => onView(item.view)}>
              <item.icon size={15} />
              <span className="home-row-title">{item.label}</span>
              <span className="home-row-meta">{item.note}</span>
              <ArrowUpRight size={14} />
            </button>
          ))}
        </section>
      </div>
    </div>
  );
}

function RunsView({ sessions, run, onOpen }: { sessions: HarnessSession[]; run: HarnessRun | null; onOpen: (id: string) => void }) {
  const inFlight = !!run && isRunActive(run.status);
  return (
    <div className="standalone-view">
      <div className="standalone-heading"><div><span className="eyebrow">control / execution</span><h1>active runs</h1><p>durable work stays visible even when the browser does not.</p></div></div>
      <div className="run-board">
        <div className="run-board-main">
          <div className="panel-heading"><span>in flight</span><span className="panel-count">{inFlight ? "01" : "00"}</span></div>
          {inFlight && run ? <div className="large-run-card"><div className="large-run-top"><RunPill status={run.status} /><span>{run.id}</span><span className="large-run-elapsed"><Clock3 size={13} /> {formatElapsed(run.startedAt)}</span></div><div className="large-run-route"><span><Gauge size={14} /> {run.model}</span><span><Zap size={14} /> {run.tools.length} tool calls</span><span><BrainCircuit size={14} /> {formatNumber(run.metrics.reasoningTokens)} reasoning tok</span></div></div> : <div className="blank-card"><Check size={18} /><strong>no active generation</strong><span>the harness is idle.</span></div>}
          <div className="panel-heading recent-heading"><span>recent threads</span><span className="panel-count">{sessions.length.toString().padStart(2, "0")}</span></div>
          {sessions.slice(0, 3).map((session, index) => <div className="completion-row" key={session.id} role="button" tabIndex={0} onClick={() => onOpen(session.id)} onKeyDown={(event) => { if (event.key === "Enter" || event.key === " ") { event.preventDefault(); onOpen(session.id); } }}><span className="completion-index">0{index + 1}</span><span className="completion-name"><strong>{session.title}</strong><small>{session.profile} · {session.model}</small></span><span className="completion-result"><Check size={13} /> {session.status}</span><span className="completion-time">{formatTime(session.updatedAt)}</span></div>)}
        </div>
      </div>
    </div>
  );
}

// Session graph: the message tree as a git-graph - lanes are branches, rows
// are depth. Click a row to inspect it in the side panel, then either select
// that branch as the active path (non-destructive) or rewind to it (destroys
// every descendant, same as the chat's rewind-here). Clicking a node ball
// directly swaps the viewed branch (non-active rows) - the git-client UX.

// Graph strip geometry: one SVG column per row, LANE_PX between lane centers.
const LANE_PX = 26;
const ROW_H = 40;
const laneX = (lane: number) => lane * LANE_PX + LANE_PX / 2;

function GraphView({ session, rows, runs, onOpenChat, onSelectBranch, onRewind }: {
  session: HarnessSession | undefined;
  rows: GraphRow[];
  runs: Map<string, HarnessRun>;
  onOpenChat: (messageId: number | null) => void;
  onSelectBranch: (messageId: number) => void;
  onRewind: (message: HarnessMessage) => void;
}) {
  // Preselect the active leaf so the panel is useful on first open; the
  // component remounts on view switch, so no effect is needed to track it.
  const [selectedId, setSelectedId] = useState<number | null>(() => rows.filter((row) => row.active).pop()?.message.id ?? null);
  const selected = rows.find((row) => row.message.id === selectedId) ?? null;
  const laneCount = rows.reduce((max, row) => Math.max(max, row.lane + 1), 0);
  // Rails/curves come from actual parent→child chains (computeGraphEdges),
  // so a lane reused by an unrelated later branch draws no phantom rail
  // across the gap, and the active path reads in the accent color.
  const edges = useMemo(() => computeGraphEdges(rows), [rows]);
  // Sibling positions under the same parent (row order within a depth group
  // is id order), for the "branch N of M" tag on off-path rows.
  const siblingInfo = useMemo(() => {
    const groups = new Map<number, GraphRow[]>();
    for (const row of rows) {
      const list = groups.get(row.message.parentId);
      if (list) list.push(row);
      else groups.set(row.message.parentId, [row]);
    }
    const info = new Map<number, { index: number; count: number }>();
    for (const list of groups.values()) {
      list.forEach((row, index) => info.set(row.message.id, { index, count: list.length }));
    }
    return info;
  }, [rows]);
  const selectedRun = selected?.message.runId ? runs.get(selected.message.runId) : undefined;
  const heading = (
    <div className="standalone-heading">
      <div><span className="eyebrow">control / lineage</span><h1>session graph</h1><p>every branch of this conversation. click a row to inspect it, then select it as the active path or rewind to it. open in chat scrolls the chat to the selected message.</p></div>
      <div className="graph-heading-actions">
        <span className="graph-stats"><GitFork size={13} /> {rows.length} messages · {laneCount} {laneCount === 1 ? "lane" : "lanes"}</span>
        <button type="button" className="secondary-button" onClick={() => onOpenChat(selectedId)} title={selectedId != null ? "open the chat scrolled to the selected message" : "open the chat"}><MessageSquare size={14} /> open in chat</button>
      </div>
    </div>
  );
  if (!session) {
    return (
      <div className="standalone-view">
        {heading}
        <div className="blank-card"><GitFork size={18} /><strong>no session</strong><span>open a conversation to see its lineage.</span></div>
      </div>
    );
  }
  return (
    <div className="standalone-view graph-view">
      {heading}
      {rows.length === 0 ? (
        <div className="blank-card"><GitFork size={18} /><strong>no messages</strong><span>send the first message to grow the graph.</span></div>
      ) : (
        <div className="graph-layout">
          <div className="graph-canvas">
            {rows.map((row, rowIndex) => {
              const edge = edges[rowIndex];
              const edgeClass = (active: boolean) => `graph-edge${active ? " graph-edge-on" : ""}`;
              return (
                <div
                  key={row.message.id}
                  role="button"
                  tabIndex={0}
                  className={`graph-row ${row.active ? "graph-row-active" : ""} ${selectedId === row.message.id ? "graph-row-selected" : ""}`}
                  onClick={() => setSelectedId(row.message.id)}
                  onKeyDown={(event) => {
                    if (event.key === "Enter" || event.key === " ") {
                      event.preventDefault();
                      setSelectedId(row.message.id);
                    }
                  }}
                >
                  <svg className="graph-edges" width={laneCount * LANE_PX} height={ROW_H} aria-hidden="true">
                    {[...edge.through].map(([lane, active]) => (
                      <line key={lane} x1={laneX(lane)} x2={laneX(lane)} y1={0} y2={ROW_H} className={edgeClass(active)} />
                    ))}
                    {edge.inEdge && (edge.inEdge.fromLane === row.lane ? (
                      <line x1={laneX(row.lane)} x2={laneX(row.lane)} y1={0} y2={ROW_H / 2} className={edgeClass(edge.inEdge.active)} />
                    ) : (
                      <path
                        d={`M ${laneX(edge.inEdge.fromLane)} 0 C ${laneX(edge.inEdge.fromLane)} ${ROW_H / 4} ${laneX(row.lane)} ${ROW_H / 4} ${laneX(row.lane)} ${ROW_H / 2}`}
                        className={edgeClass(edge.inEdge.active)}
                      />
                    ))}
                    {edge.outEdges.length > 0 && (
                      <line
                        x1={laneX(edge.outEdges[0].fromLane)}
                        x2={laneX(edge.outEdges[0].fromLane)}
                        y1={ROW_H / 2}
                        y2={ROW_H}
                        className={edgeClass(edge.outEdges.some((out) => out.active))}
                      />
                    )}
                    <circle
                      cx={laneX(row.lane)}
                      cy={ROW_H / 2}
                      r={5}
                      className={`graph-node graph-node-${row.message.role} ${row.active ? "graph-node-active" : ""}`}
                      onClick={(event) => {
                        event.stopPropagation();
                        if (row.active) setSelectedId(row.message.id);
                        else onSelectBranch(row.message.id);
                      }}
                    >
                      <title>{row.active ? "on the active path" : "switch to this branch"}</title>
                    </circle>
                  </svg>
                  <span className="graph-time">{formatTime(row.message.createdAt)}</span>
                  <div className="graph-body">
                    <span className="graph-role-line">
                      <span className={`graph-role-badge graph-role-badge-${row.message.role}`}>{row.message.role}</span>
                      {!row.active && (siblingInfo.get(row.message.id)?.count ?? 1) > 1 && (
                        <span className="graph-branch-tag" title="off the active path - click the node ball to switch to this branch">
                          branch {(siblingInfo.get(row.message.id)!.index) + 1}/{siblingInfo.get(row.message.id)!.count}
                        </span>
                      )}
                      {row.active && <span className="graph-active-tag">active</span>}
                    </span>
                    <span className="graph-preview">{row.message.content}</span>
                  </div>
                </div>
              );
            })}
          </div>
          <aside className="graph-details">
            {selected ? (
              <>
                <div className="graph-details-head">
                  <span className="eyebrow">message {selected.message.id}</span>
                  <span>{selected.message.role} · {formatTime(selected.message.createdAt)}</span>
                </div>
                <div className="graph-details-content"><MarkdownContent content={selected.message.content} /></div>
                {selectedRun && (
                  <div className="graph-run">
                    <div className="graph-run-top"><RunPill status={selectedRun.status} /><span>{selectedRun.model}</span></div>
                    <div className="graph-run-route">
                      <span><Zap size={13} /> {selectedRun.tools.length} tools</span>
                      <span><Gauge size={13} /> {formatNumber(selectedRun.metrics.inputTokens)} in</span>
                      <span><BrainCircuit size={13} /> {formatNumber(selectedRun.metrics.reasoningTokens)} reasoning</span>
                    </div>
                    {selectedRun.error && <div className="graph-run-error">{selectedRun.error}</div>}
                  </div>
                )}
                <div className="graph-details-actions">
                  <button type="button" className="primary-button" onClick={() => onSelectBranch(selected.message.id)}><Check size={14} /> select this branch</button>
                  <button type="button" className="secondary-button" onClick={() => onRewind(selected.message)}><History size={14} /> rewind here</button>
                </div>
              </>
            ) : (
              <div className="blank-card"><GitFork size={18} /><strong>no message selected</strong><span>click a row in the graph to inspect it.</span></div>
            )}
          </aside>
        </div>
      )}
    </div>
  );
}

function NotWired({ eyebrow, title, note }: { eyebrow: string; title: string; note: string }) {
  return (
    <div className="standalone-view">
      <div className="standalone-heading">
        <div>
          <span className="eyebrow">{eyebrow}</span>
          <h1>{title}</h1>
          <p>{note}</p>
        </div>
      </div>
      <div className="blank-card">
        <CircleAlert size={18} />
        <strong>not yet wired to the daemon</strong>
        <span>this surface has no backend endpoint yet. it will read live harness data once the corresponding api lands.</span>
      </div>
    </div>
  );
}

function MemoryView({
  currentSessionId,
  onStatus,
  onRecall,
  onBank,
  onUnitDetail,
  onInvalidateUnit,
  onReactivateUnit,
  onSupersedeUnit,
  onDeleteUnit,
  onSetUnitCore,
  onConsolidate,
  onListRecords,
  onCreateRecord,
  onApproveRecord,
  onRejectRecord,
  onListConsolidations,
  onApproveConsolidation,
  onRejectConsolidation,
}: {
  currentSessionId: string;
  onStatus: () => Promise<MemoryStatus>;
  onRecall: (input: { query: string; session?: string }) => Promise<MemoryRecallResponse>;
  onBank: (input?: {
    text?: string;
    type?: string;
    scope?: string;
    state?: string;
    limit?: number;
    offset?: number;
  }) => Promise<MemoryBankResponse>;
  onUnitDetail: (id: string) => Promise<MemoryUnitDetail>;
  onInvalidateUnit: (id: string, reason?: string) => Promise<void>;
  onReactivateUnit: (id: string, reason?: string) => Promise<void>;
  onSupersedeUnit: (id: string, text: string, reason?: string) => Promise<void>;
  onDeleteUnit: (id: string) => Promise<void>;
  onSetUnitCore: (id: string, on: boolean) => Promise<void>;
  onConsolidate: () => Promise<{ ok: boolean; job_id: number }>;
  onListRecords: (status?: string) => Promise<MemoryRecord[]>;
  onCreateRecord: (input: MemoryRecordInput) => Promise<MemoryRecord>;
  onApproveRecord: (id: number) => Promise<MemoryRecord>;
  onRejectRecord: (id: number) => Promise<MemoryRecord>;
  onListConsolidations: (status?: string) => Promise<MemoryConsolidation[]>;
  onApproveConsolidation: (id: number) => Promise<MemoryConsolidation>;
  onRejectConsolidation: (id: number) => Promise<MemoryConsolidation>;
}) {
  const [status, setStatus] = useState<MemoryStatus | null>(null);
  const [statusBusy, setStatusBusy] = useState(true);
  const [statusError, setStatusError] = useState("");
  const [query, setQuery] = useState("");
  const [scope, setScope] = useState<"session" | "bank">(currentSessionId ? "session" : "bank");
  const [results, setResults] = useState<MemoryRecallItem[] | null>(null);
  const [meta, setMeta] = useState<{ source: string; fallback: boolean } | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  // Approved-memory queue (slice 2): candidates awaiting a decision, plus the
  // decided history. Nothing is auto-promoted; every row is an explicit choice.
  const [records, setRecords] = useState<MemoryRecord[]>([]);
  const [recordsBusy, setRecordsBusy] = useState(true);
  const [recordsError, setRecordsError] = useState("");
  const [draftText, setDraftText] = useState("");
  const [draftType, setDraftType] = useState("fact");
  const [draftScope, setDraftScope] = useState("session");
  const [draftBusy, setDraftBusy] = useState(false);
  const [decidingId, setDecidingId] = useState<number | null>(null);
  // Consolidation candidates (custom provider): near-duplicate pairs and
  // value conflicts the sweep proposes; the operator approves (retires the
  // older unit) or
  // rejects (keeps both, never re-proposed). Nothing is retired until
  // approved.
  const [consolidations, setConsolidations] = useState<MemoryConsolidation[]>([]);
  const [consolidationsBusy, setConsolidationsBusy] = useState(true);
  const [consolidationsError, setConsolidationsError] = useState("");
  const [decidingConsolidationId, setDecidingConsolidationId] = useState<number | null>(null);
  // Decided candidates (approved/rejected) collapse into a history block so
  // the working set stays clean after a review pass.
  const [showConsolidationHistory, setShowConsolidationHistory] = useState(false);
  // Memory bank browser: paginated listing of the stored units with optional
  // full-text and type/state filters, in table or timeline form.
  const [bankText, setBankText] = useState("");
  const [bankType, setBankType] = useState("all");
  const [bankState, setBankState] = useState<"active" | "all">("active");
  const [bankUnits, setBankUnits] = useState<MemoryBankUnit[] | null>(null);
  const [bankTotal, setBankTotal] = useState(0);
  const [bankBusy, setBankBusy] = useState(true);
  const [bankError, setBankError] = useState("");
  const [bankView, setBankView] = useState<"table" | "timeline">("table");
  const BANK_PAGE = 50;
  // Unit curation: the inspected unit's detail (unit + version timeline +
  // links), the in-flight operation, and the supersede draft. Delete is an
  // explicit two-step confirmation.
  const [detailId, setDetailId] = useState<string | null>(null);
  const [detail, setDetail] = useState<MemoryUnitDetail | null>(null);
  const [detailBusy, setDetailBusy] = useState(false);
  const [detailError, setDetailError] = useState("");
  const [unitOp, setUnitOp] = useState<"" | "invalidate" | "reactivate" | "supersede" | "delete" | "pin" | "unpin">("");
  const [unitOpError, setUnitOpError] = useState("");
  const [supersedeText, setSupersedeText] = useState("");
  const [supersedeReason, setSupersedeReason] = useState("");
  const [confirmDelete, setConfirmDelete] = useState(false);

  // Load the status once on mount; every state update lands in a promise
  // continuation, so the effect body itself stays side-effect-free.
  useEffect(() => {
    let active = true;
    onStatus()
      .then((data) => { if (active) setStatus(data); })
      .catch((err) => { if (active) setStatusError(err instanceof Error ? err.message : "status unavailable"); })
      .finally(() => { if (active) setStatusBusy(false); });
    return () => { active = false; };
    // onStatus closes over the stable client; the status is fetched once.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const refreshStatus = useCallback(async () => {
    setStatusBusy(true);
    setStatusError("");
    try {
      setStatus(await onStatus());
    } catch (err) {
      setStatusError(err instanceof Error ? err.message : "status unavailable");
    } finally {
      setStatusBusy(false);
    }
  }, [onStatus]);

  const loadRecords = useCallback(async () => {
    setRecordsBusy(true);
    setRecordsError("");
    try {
      setRecords(await onListRecords());
    } catch (err) {
      setRecordsError(err instanceof Error ? err.message : "queue unavailable");
    } finally {
      setRecordsBusy(false);
    }
  }, [onListRecords]);

  // Load the queue once on mount, alongside the status probe.
  useEffect(() => {
    let active = true;
    onListRecords()
      .then((data) => { if (active) setRecords(data); })
      .catch((err) => { if (active) setRecordsError(err instanceof Error ? err.message : "queue unavailable"); })
      .finally(() => { if (active) setRecordsBusy(false); });
    return () => { active = false; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const proposeRecord = async () => {
    const text = draftText.trim();
    if (!text || draftBusy) return;
    setDraftBusy(true);
    setRecordsError("");
    try {
      await onCreateRecord({
        text,
        type: draftType,
        scope: draftScope,
        ...(currentSessionId ? { session_id: currentSessionId } : {}),
        provenance: JSON.stringify({ origin: "manual", via: "nyaxus-ui" }),
      });
      setDraftText("");
      await loadRecords();
    } catch (err) {
      setRecordsError(err instanceof Error ? err.message : "could not propose a memory");
    } finally {
      setDraftBusy(false);
    }
  };

  const decideRecord = async (id: number, action: "approve" | "reject") => {
    if (decidingId !== null) return;
    setDecidingId(id);
    setRecordsError("");
    try {
      if (action === "approve") await onApproveRecord(id);
      else await onRejectRecord(id);
      await loadRecords();
    } catch (err) {
      setRecordsError(err instanceof Error ? err.message : "could not update the record");
    } finally {
      setDecidingId(null);
    }
  };

  const loadConsolidations = useCallback(async () => {
    setConsolidationsBusy(true);
    setConsolidationsError("");
    try {
      setConsolidations(await onListConsolidations());
    } catch (err) {
      setConsolidationsError(err instanceof Error ? err.message : "candidates unavailable");
    } finally {
      setConsolidationsBusy(false);
    }
  }, [onListConsolidations]);

  // Load the candidates once the status confirms the custom provider (the
  // endpoint only exists there); the status probe resolves on mount.
  const customProviderLoaded = status?.provider === "custom" || !!status?.custom;
  useEffect(() => {
    if (!customProviderLoaded) return;
    let active = true;
    onListConsolidations()
      .then((data) => { if (active) setConsolidations(data); })
      .catch((err) => { if (active) setConsolidationsError(err instanceof Error ? err.message : "candidates unavailable"); })
      .finally(() => { if (active) setConsolidationsBusy(false); });
    return () => { active = false; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [customProviderLoaded]);

  const runRecall = async () => {
    const trimmed = query.trim();
    if (!trimmed || busy) return;
    setBusy(true);
    setError("");
    try {
      const response = await onRecall({
        query: trimmed,
        ...(scope === "session" && currentSessionId ? { session: currentSessionId } : {}),
      });
      setResults(response.results);
      setMeta({ source: response.source, fallback: response.fallback === true });
    } catch (err) {
      setError(err instanceof Error ? err.message : "recall failed");
    } finally {
      setBusy(false);
    }
  };

  type BankOverrides = { type?: string; state?: "active" | "all" };

  const bankParams = useCallback(
    (offset: number, overrides?: BankOverrides) => ({
      text: bankText.trim() || undefined,
      type: (overrides?.type ?? bankType) === "all" ? undefined : (overrides?.type ?? bankType),
      state: (overrides?.state ?? bankState) === "active" ? "active" : "all",
      limit: BANK_PAGE,
      offset,
    }),
    [bankText, bankType, bankState],
  );

  const applyBank = useCallback(async (overrides?: BankOverrides) => {
    setBankBusy(true);
    setBankError("");
    try {
      const response = await onBank(bankParams(0, overrides));
      setBankUnits(response.units);
      setBankTotal(response.total);
    } catch (err) {
      setBankError(err instanceof Error ? err.message : "bank unavailable");
      setBankUnits(null);
      setBankTotal(0);
    } finally {
      setBankBusy(false);
    }
  }, [onBank, bankParams]);

  const loadMoreBank = useCallback(async () => {
    if (bankBusy || !bankUnits || bankUnits.length >= bankTotal) return;
    setBankBusy(true);
    setBankError("");
    try {
      const response = await onBank(bankParams(bankUnits.length));
      setBankUnits((prev) => (prev ? [...prev, ...response.units] : response.units));
      setBankTotal(response.total);
    } catch (err) {
      setBankError(err instanceof Error ? err.message : "bank unavailable");
    } finally {
      setBankBusy(false);
    }
  }, [onBank, bankParams, bankBusy, bankUnits, bankTotal]);

  const openUnit = useCallback(async (id: string) => {
    setDetailId(id);
    setDetail(null);
    setDetailError("");
    setUnitOpError("");
    setSupersedeText("");
    setSupersedeReason("");
    setConfirmDelete(false);
    setDetailBusy(true);
    try {
      setDetail(await onUnitDetail(id));
    } catch (err) {
      setDetailError(err instanceof Error ? err.message : "could not load the unit");
    } finally {
      setDetailBusy(false);
    }
  }, [onUnitDetail]);

  const closeUnit = useCallback(() => {
    setDetailId(null);
    setDetail(null);
    setDetailError("");
    setUnitOpError("");
    setSupersedeText("");
    setSupersedeReason("");
    setConfirmDelete(false);
  }, []);

  // Run a curation operation, then refresh the detail (unless it went away)
  // and the bank so the table reflects the change.
  const runUnitOp = useCallback(
    async (op: "invalidate" | "reactivate" | "supersede" | "delete" | "pin" | "unpin", action: () => Promise<void>) => {
      if (!detailId || unitOp) return;
      setUnitOp(op);
      setUnitOpError("");
      try {
        await action();
        if (op === "delete") {
          closeUnit();
        } else {
          setDetail(await onUnitDetail(detailId));
        }
        await applyBank();
      } catch (err) {
        setUnitOpError(err instanceof Error ? err.message : "operation failed");
      } finally {
        setUnitOp("");
      }
    },
    [detailId, unitOp, onUnitDetail, applyBank, closeUnit],
  );

  // Load the first page once on mount, alongside the status probe.
  useEffect(() => {
    let active = true;
    onBank({ limit: BANK_PAGE, offset: 0 })
      .then((data) => {
        if (active) {
          setBankUnits(data.units);
          setBankTotal(data.total);
        }
      })
      .catch((err) => {
        if (active) setBankError(err instanceof Error ? err.message : "bank unavailable");
      })
      .finally(() => {
        if (active) setBankBusy(false);
      });
    return () => { active = false; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Timeline view: group the (newest-first) page by its created date.
  const timelineGroups = useMemo(() => {
    const groups: Array<{ date: string; units: MemoryBankUnit[] }> = [];
    for (const unit of bankUnits || []) {
      const date = (unit.created_at || "").slice(0, 10) || "undated";
      const last = groups[groups.length - 1];
      if (last && last.date === date) last.units.push(unit);
      else groups.push({ date, units: [unit] });
    }
    return groups;
  }, [bankUnits]);

  const customProvider = status?.provider === "custom" || !!status?.custom;
  const customUnitCount = Object.values(status?.custom?.units ?? {}).reduce((sum, count) => sum + count, 0);
  const lastSweep = status?.custom?.last_sweep;

  // Manual consolidation sweep (two-step confirm). The sweep runs as a
  // durable background job: the button enqueues it, then polls the status
  // until the sweep records a new finished_at stamp and reports the result
  // (units scanned, collapses) right here in the view.
  const [consolidateBusy, setConsolidateBusy] = useState(false);
  const [consolidateConfirm, setConsolidateConfirm] = useState(false);
  const [consolidateMsg, setConsolidateMsg] = useState("");
  // "on" (green) once the sweep reports a result; "warn" (amber) while it is
  // still running or when it failed.
  const [consolidateTone, setConsolidateTone] = useState<"on" | "warn">("warn");
  const runConsolidate = useCallback(async () => {
    if (!consolidateConfirm) {
      setConsolidateConfirm(true);
      window.setTimeout(() => setConsolidateConfirm(false), 5000);
      return;
    }
    setConsolidateConfirm(false);
    setConsolidateBusy(true);
    setConsolidateMsg("");
    setConsolidateTone("warn");
    try {
      // The sweep we are about to trigger must be distinguishable from the
      // last one on record: remember its finished_at before enqueuing.
      const before = (await onStatus()).custom?.last_sweep?.finished_at ?? "";
      const enqueued = await onConsolidate();
      setConsolidateMsg(`sweep enqueued (job ${enqueued.job_id}) - scanning the active bank…`);
      let sweep: MemorySweepInfo | null = null;
      let activeUnits = 0;
      for (let attempt = 0; attempt < 60; attempt += 1) {
        await new Promise((resolve) => { window.setTimeout(resolve, 1000); });
        const data = await onStatus();
        activeUnits = Number(data.custom?.units?.active || 0);
        const last = data.custom?.last_sweep;
        if (last && last.finished_at && last.finished_at !== before) {
          sweep = last;
          break;
        }
      }
      if (sweep) {
        setConsolidateTone("on");
        let msg =
          `sweep complete - ${sweep.selected} unit(s) scanned, ${sweep.changes} candidate(s) proposed for review ` +
          `(trigger ${sweep.trigger}${sweep.fixpoint ? ", fixpoint" : ""})`;
        if (sweep.selected === 0) {
          msg += activeUnits === 0
            ? " - no active units in the bank"
            : " - all active units are within the 24h cooldown";
        }
        setConsolidateMsg(msg);
      } else {
        setConsolidateMsg("sweep still running in the background - refresh status for the result");
      }
      await refreshStatus();
      await applyBank();
      await loadConsolidations();
    } catch (err) {
      setConsolidateTone("warn");
      setConsolidateMsg(err instanceof Error ? err.message : "consolidation failed");
    } finally {
      setConsolidateBusy(false);
    }
  }, [consolidateConfirm, onConsolidate, onStatus, refreshStatus, applyBank, loadConsolidations]);

  // Approve (retire the older unit) or reject (keep both, never re-proposed)
  // a proposed consolidation, then refresh the candidates, the bank, and the
  // status so the chips and table reflect the decision.
  const decideConsolidation = async (id: number, action: "approve" | "reject") => {
    if (decidingConsolidationId !== null) return;
    setDecidingConsolidationId(id);
    setConsolidationsError("");
    try {
      if (action === "approve") await onApproveConsolidation(id);
      else await onRejectConsolidation(id);
      await loadConsolidations();
      await applyBank();
      await refreshStatus();
    } catch (err) {
      setConsolidationsError(err instanceof Error ? err.message : "could not update the consolidation");
    } finally {
      setDecidingConsolidationId(null);
    }
  };

  // The working set is the pending candidates; decided ones fold into the
  // history block below the list.
  const pendingConsolidations = consolidations.filter((candidate) => candidate.status === "pending");
  const decidedConsolidations = consolidations.filter((candidate) => candidate.status !== "pending");

  const renderConsolidationRow = (candidate: MemoryConsolidation, withActions: boolean) => (
    <div className={`memory-record memory-consolidation memory-consolidation-${candidate.status}`}>
      <div className="memory-record-meta">
        <span className={`memory-chip memory-record-status-${candidate.status}`}>{candidate.status}</span>
        {candidate.kind === "conflict" && (
          <span className="memory-chip" title="same-topic memories asserting different values; approving keeps the newer text as the standing fact">conflict</span>
        )}
        <span title="embedding cosine similarity of the pair">cosine {candidate.cosine.toFixed(3)}</span>
        <span>{candidate.scope}</span>
        <span>retire unit {candidate.older_unit_id}</span>
        <span>keep unit {candidate.newer_unit_id}</span>
        {candidate.created_at && <span>proposed {formatTime(candidate.created_at)}</span>}
        {candidate.decided_at && <span>decided {formatTime(candidate.decided_at)}</span>}
      </div>
      <div className="memory-consolidation-pair">
        <div className="memory-consolidation-side">
          <span className="memory-consolidation-label">retire (older)</span>
          <p>{candidate.older_text || "-"}</p>
        </div>
        <div className="memory-consolidation-side">
          <span className="memory-consolidation-label">keep (newer)</span>
          <p>{candidate.newer_text || "-"}</p>
        </div>
      </div>
      {withActions && candidate.status === "pending" && (
        <div className="memory-record-actions">
          <button className="primary-button" onClick={() => void decideConsolidation(candidate.id, "approve")} disabled={decidingConsolidationId !== null}>
            {decidingConsolidationId === candidate.id ? <LoaderCircle size={14} className="spin" /> : <Check size={14} />} approve
          </button>
          <button className="secondary-button" onClick={() => void decideConsolidation(candidate.id, "reject")} disabled={decidingConsolidationId !== null}>
            {decidingConsolidationId === candidate.id ? <LoaderCircle size={14} className="spin" /> : <X size={14} />} reject
          </button>
        </div>
      )}
    </div>
  );

  // Inline unit inspection: rendered directly under the clicked row
  // (table view) or item (timeline view), so the detail stays in context
  // instead of landing at the bottom of the list.
  const renderUnitDetail = () => {
    if (!detailId) return null;
    return (
      <div className="memory-unit-detail">
            <div className="memory-unit-detail-head">
              <div>
                <span className="eyebrow">systems / curation</span>
                <h3>unit {detailId}</h3>
              </div>
              <button className="secondary-button" onClick={closeUnit}>
                <X size={14} /> close
              </button>
            </div>
            {detailBusy && (
              <div className="blank-card"><LoaderCircle size={18} className="spin" /><strong>loading unit</strong><span>fetching versions and provenance.</span></div>
            )}
            {detailError && <div className="persona-error"><CircleAlert size={14} /> {detailError}</div>}
            {detail && (
              <div className="memory-unit-detail-body">
                <div className="memory-unit-meta">
                  <span className={`memory-chip memory-unit-state-${detail.unit.state}`}>{detail.unit.state}</span>
                  <span className="memory-kind">{detail.unit.type}</span>
                  {detail.unit.core && (
                    <span className="memory-chip memory-unit-core" title="pinned: injected verbatim into every prompt">
                      <Pin size={11} /> core
                    </span>
                  )}
                  <span>{detail.unit.scope}</span>
                  {detail.unit.profile && <span>{detail.unit.profile}</span>}
                  {detail.unit.session_id && <span>session {detail.unit.session_id}</span>}
                  {detail.unit.run_id && <span>run {detail.unit.run_id}</span>}
                  {detail.unit.created_at && <span>created {(detail.unit.created_at || "").slice(0, 10)}</span>}
                  {detail.feedback && (
                    <span title="times offered to the model (distinct from 'used')">
                      offered {detail.feedback.offered}
                      {detail.feedback.last_offered_at
                        ? ` (last ${detail.feedback.last_offered_at.slice(0, 10)})`
                        : ""}
                    </span>
                  )}
                  {detail.feedback && detail.feedback.cited > 0 && (
                    <span title="times the model cited this memory in its answer (validated against the offered set)">
                      cited {detail.feedback.cited}
                      {detail.feedback.last_cited_at
                        ? ` (last ${detail.feedback.last_cited_at.slice(0, 10)})`
                        : ""}
                    </span>
                  )}
                  {detail.feedback && detail.feedback.used > 0 && (
                    <span title="times the model marked this memory as used (low-trust signal, validated against the offered set)">
                      used {detail.feedback.used}
                      {detail.feedback.last_used_at
                        ? ` (last ${detail.feedback.last_used_at.slice(0, 10)})`
                        : ""}
                    </span>
                  )}
                  {detail.feedback && detail.feedback.acted_on > 0 && (
                    <span title="times the model acted on this memory (low-trust signal, validated against the offered set)">
                      acted-on {detail.feedback.acted_on}
                      {detail.feedback.last_acted_on_at
                        ? ` (last ${detail.feedback.last_acted_on_at.slice(0, 10)})`
                        : ""}
                    </span>
                  )}
                </div>
                {detail.unit.state_reason && (
                  <div className="memory-unit-state-reason">{detail.unit.state_reason}</div>
                )}
                <div className="memory-unit-versions">
                  {detail.versions.map((version, index) => (
                    <div key={version.id} className={`memory-unit-version${version.current ? " memory-unit-version-current" : ""}`}>
                      <div className="memory-unit-version-head">
                        <span>v{index + 1}</span>
                        {version.current && <span className="memory-chip memory-unit-current">current</span>}
                        {version.created_at && <span>{version.created_at.slice(0, 10)}</span>}
                        {version.model_id && <span>{version.model_id}</span>}
                        {version.store_reason && <span>{version.store_reason}</span>}
                      </div>
                      <p>{version.text}</p>
                    </div>
                  ))}
                </div>
                {detail.links.length > 0 && (
                  <div className="memory-unit-links">
                    {detail.links.map((link) => (
                      <span key={link.target_id} className="memory-unit-link">
                        <ArrowUpRight size={12} /> {link.link_type} → unit {link.target_id}
                      </span>
                    ))}
                  </div>
                )}
                {unitOpError && <div className="persona-error"><CircleAlert size={14} /> {unitOpError}</div>}
                <div className="memory-unit-actions">
                  {detail.unit.state === "active" ? (
                    <button
                      className="secondary-button"
                      onClick={() => void runUnitOp("invalidate", () => onInvalidateUnit(detailId))}
                      disabled={unitOp !== ""}
                    >
                      {unitOp === "invalidate" ? <LoaderCircle size={14} className="spin" /> : <CircleStop size={14} />} invalidate
                    </button>
                  ) : (
                    <button
                      className="secondary-button"
                      onClick={() => void runUnitOp("reactivate", () => onReactivateUnit(detailId))}
                      disabled={unitOp !== ""}
                    >
                      {unitOp === "reactivate" ? <LoaderCircle size={14} className="spin" /> : <Play size={14} />} reactivate
                    </button>
                  )}
                  <button
                    className="secondary-button"
                    title="pinned units are injected verbatim into every prompt (the always-injected core)"
                    onClick={() => void runUnitOp(detail.unit.core ? "unpin" : "pin", () => onSetUnitCore(detailId, !detail.unit.core))}
                    disabled={unitOp !== ""}
                  >
                    {(unitOp === "pin" || unitOp === "unpin") ? <LoaderCircle size={14} className="spin" /> : <Pin size={14} />}
                    {detail.unit.core ? "unpin from core" : "pin to core"}
                  </button>
                  <div className="memory-unit-supersede">
                    <textarea
                      className="memory-propose-text"
                      value={supersedeText}
                      onChange={(event) => setSupersedeText(event.target.value)}
                      placeholder="replace this memory's text (appends a new version, keeps the old one)"
                      rows={2}
                    />
                    <div className="memory-unit-supersede-row">
                      <input
                        className="memory-unit-reason"
                        value={supersedeReason}
                        onChange={(event) => setSupersedeReason(event.target.value)}
                        placeholder="reason (optional)"
                      />
                      <button
                        className="primary-button"
                        onClick={() => {
                          const text = supersedeText.trim();
                          if (!text) return;
                          void runUnitOp("supersede", async () => {
                            await onSupersedeUnit(detailId, text, supersedeReason.trim() || undefined);
                            setSupersedeText("");
                            setSupersedeReason("");
                          });
                        }}
                        disabled={unitOp !== "" || !supersedeText.trim()}
                      >
                        {unitOp === "supersede" ? <LoaderCircle size={14} className="spin" /> : <History size={14} />} supersede
                      </button>
                    </div>
                  </div>
                  {!confirmDelete ? (
                    <button className="secondary-button" onClick={() => setConfirmDelete(true)} disabled={unitOp !== ""}>
                      <Trash2 size={14} /> delete…
                    </button>
                  ) : (
                    <div className="memory-unit-delete-confirm">
                      <span>delete this unit and all of its versions? this cannot be undone.</span>
                      <button
                        className="danger-button"
                        onClick={() => void runUnitOp("delete", () => onDeleteUnit(detailId))}
                        disabled={unitOp !== ""}
                      >
                        {unitOp === "delete" ? <LoaderCircle size={14} className="spin" /> : <Trash2 size={14} />} confirm delete
                      </button>
                      <button className="secondary-button" onClick={() => setConfirmDelete(false)} disabled={unitOp !== ""}>
                        cancel
                      </button>
                    </div>
                  )}
                </div>
              </div>
            )}

      </div>
    );
  };

  return (
    <div className="standalone-view">
      <div className="standalone-heading">
        <div>
          <span className="eyebrow">systems / recall</span>
          <h1>memory</h1>
          <p>recall the memory bank on demand and inspect the retain outbox. recalled items are evidence, not instructions.</p>
        </div>
        <div className="memory-heading-actions">
          {customProvider && (
            <button className="secondary-button" onClick={() => void runConsolidate()} disabled={consolidateBusy} title="collapse near-duplicate memories across the whole active bank (per scope, cosine above memory.sweep_cosine_threshold, default 0.92; 24h per-unit cooldown) - runs in the background and reports the result here">
              {consolidateBusy ? <LoaderCircle size={14} className="spin" /> : <Sparkles size={14} />}
              {consolidateConfirm ? "confirm sweep" : "consolidate"}
            </button>
          )}
          <button className="secondary-button" onClick={() => void refreshStatus()} disabled={statusBusy}>
            {statusBusy ? <LoaderCircle size={14} className="spin" /> : <RefreshCw size={14} />} refresh status
          </button>
        </div>
      </div>
      {consolidateMsg && (
        <div className={`memory-chip ${consolidateTone === "on" ? "memory-chip-on" : "memory-chip-warn"}`} style={{ display: "inline-block", marginBottom: 8 }}>
          {consolidateBusy && <LoaderCircle size={12} className="spin" />}
          {consolidateMsg}
        </div>
      )}
      {statusError && <div className="persona-error"><CircleAlert size={14} /> {statusError}</div>}
      {status && (
        <div className="memory-status">
          <span className={`memory-chip ${status.healthy ? "memory-chip-on" : status.enabled ? "memory-chip-warn" : "memory-chip-off"}`}>
            {status.healthy ? customProvider ? "custom healthy" : "provider healthy" : status.enabled ? "provider unreachable" : "memory disabled"}
          </span>
          {status.provider && <span className="memory-chip">provider {status.provider}</span>}
          {customProvider ? (
            <>
              <span className={`memory-chip ${status.custom?.ready ? "memory-chip-on" : "memory-chip-warn"}`}>
                local models {status.custom?.ready ? "ready" : "not ready"}
              </span>
              {customUnitCount > 0 && <span className="memory-chip">{customUnitCount} active units</span>}
              {typeof status.custom?.core === "number" && status.custom.core > 0 && (
                <span className="memory-chip memory-unit-core" title="pinned units injected verbatim into every prompt">
                  <Pin size={11} /> {status.custom.core} core
                </span>
              )}
              {lastSweep && (
                <span
                  className="memory-chip"
                  title={`consolidation sweep: ${lastSweep.selected} selected, ${lastSweep.changes} proposed (trigger ${lastSweep.trigger})`}
                >
                  sweep {lastSweep.changes} proposed · {lastSweep.trigger}
                  {lastSweep.finished_at ? ` · ${lastSweep.finished_at.slice(0, 10)}` : ""}
                  {lastSweep.fixpoint ? " · fixpoint" : ""}
                </span>
              )}
              {status.custom?.consolidations && (
                <span
                  className="memory-chip"
                  title="consolidation candidates awaiting your decision (approve retires the older unit; reject keeps both)"
                >
                  {status.custom.consolidations.pending || 0} pending · {status.custom.consolidations.approved || 0} approved
                </span>
              )}
            </>
          ) : (
            <>
              {status.endpoint && <span className="memory-chip">{status.endpoint}</span>}
              {Object.entries(status.jobs).map(([state, count]) => (
                <span key={state} className={`memory-chip ${state === "failed" || state === "dead" ? "memory-chip-warn" : ""}`}>
                  {state} {count}
                </span>
              ))}
            </>
          )}
          {status.suppressions && Object.keys(status.suppressions).length > 0 && (
            <span className="memory-chip memory-chip-warn">
              {Object.values(status.suppressions).reduce((a, b) => a + b, 0)} suppressed
            </span>
          )}
          {status.records && (
            <span className="memory-chip">
              {status.records.candidate || 0} pending · {status.records.approved || 0} approved
            </span>
          )}
          {status.error && <span className="memory-chip memory-chip-warn">{status.error}</span>}
        </div>
      )}
      <div className="memory-console">
        <div className="memory-console-row">
          <input
            className="memory-query"
            value={query}
            onChange={(event) => setQuery(event.target.value)}
            onKeyDown={(event) => { if (event.key === "Enter") void runRecall(); }}
            placeholder="what should the harness recall?"
          />
          <div className="memory-scope">
            <label>
              <input type="radio" name="memory-scope" checked={scope === "session"} onChange={() => setScope("session")} disabled={!currentSessionId} />
              <span>current session</span>
            </label>
            <label>
              <input type="radio" name="memory-scope" checked={scope === "bank"} onChange={() => setScope("bank")} />
              <span>entire bank</span>
            </label>
          </div>
          <button className="primary-button" onClick={() => void runRecall()} disabled={busy || !query.trim()}>
            {busy ? <LoaderCircle size={14} className="spin" /> : <Search size={14} />} {busy ? "recalling..." : "recall"}
          </button>
        </div>
        {error && <div className="persona-error"><CircleAlert size={14} /> {error}</div>}
        {meta && results && (
          <div className="memory-results-head">
            <span>{results.length} result{results.length === 1 ? "" : "s"}</span>
            <span className="memory-source">source: {meta.source}</span>
            <span className="memory-source">
              scope: {scope === "session" && currentSessionId ? `session ${currentSessionId.slice(-8)}` : "entire bank"}
            </span>
            {meta.fallback && <span className="memory-chip memory-chip-warn">local fallback - provider disabled or unreachable</span>}
          </div>
        )}
        {results && results.length === 0 && (
          <div className="blank-card">
            <Search size={18} />
            <strong>nothing recalled</strong>
            <span>no items matched that query in the {scope === "session" ? "session" : "bank"} scope.</span>
          </div>
        )}
        {results && results.length > 0 && results.map((item) => (
          <div className="memory-result" key={item.id}>
            <div className="memory-result-meta">
              <span className="memory-kind">{item.type || "memory"}</span>
              <span>{item.id}</span>
              {item.document_id && <span>doc {item.document_id}</span>}
              {item.role && <span>{item.role}</span>}
              {(item.tags || []).map((tag) => <span key={tag} className="memory-chip">{tag}</span>)}
            </div>
            <p>{item.text}</p>
          </div>
        ))}
      </div>
      {customProvider && (
        <div className="memory-queue">
          <div className="memory-queue-head">
            <div>
              <span className="eyebrow">systems / approval</span>
              <h2>consolidation candidates</h2>
              <p>sweeps propose near-duplicate pairs and value conflicts (same topic, different facts); approving retires the older unit in favor of the newer one, rejecting keeps both and the pair is never re-proposed. nothing is retired until you approve.</p>
            </div>
            <button className="secondary-button" onClick={() => void loadConsolidations()} disabled={consolidationsBusy}>
              {consolidationsBusy ? <LoaderCircle size={14} className="spin" /> : <RefreshCw size={14} />} refresh
            </button>
          </div>
          {consolidationsError && <div className="persona-error"><CircleAlert size={14} /> {consolidationsError}</div>}
          {consolidationsBusy && consolidations.length === 0 && (
            <div className="blank-card"><LoaderCircle size={18} className="spin" /><strong>loading candidates</strong><span>fetching proposed consolidations.</span></div>
          )}
          {!consolidationsBusy && pendingConsolidations.length === 0 && (
            <div className="blank-card"><Sparkles size={18} /><strong>no pending candidates</strong><span>run a sweep above; proposed near-duplicate and conflict pairs wait here for your decision.</span></div>
          )}
          {pendingConsolidations.map((candidate) => (
            <Fragment key={candidate.id}>{renderConsolidationRow(candidate, true)}</Fragment>
          ))}
          {decidedConsolidations.length > 0 && (
            <div className="memory-consolidation-history">
              <button className="secondary-button" onClick={() => setShowConsolidationHistory((value) => !value)}>
                {showConsolidationHistory ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
                history · {decidedConsolidations.filter((candidate) => candidate.status === "approved").length} approved · {decidedConsolidations.filter((candidate) => candidate.status === "rejected").length} rejected
              </button>
              {showConsolidationHistory && (
                <div className="memory-consolidation-history-list">
                  {decidedConsolidations.map((candidate) => (
                    <Fragment key={candidate.id}>{renderConsolidationRow(candidate, false)}</Fragment>
                  ))}
                </div>
              )}
            </div>
          )}
        </div>
      )}
      <div className="memory-bank">
        <div className="memory-bank-head">
          <div>
            <span className="eyebrow">systems / bank</span>
            <h2>memory bank</h2>
            <p>browse the stored units. the text filter is a full-text search; type and state are plain filters. newest first by default.</p>
          </div>
          <div className="memory-bank-actions">
            <div className="memory-bank-views">
              <button className={bankView === "table" ? "active" : ""} onClick={() => setBankView("table")}>table</button>
              <button className={bankView === "timeline" ? "active" : ""} onClick={() => setBankView("timeline")}>timeline</button>
            </div>
            <button className="secondary-button" onClick={() => void applyBank()} disabled={bankBusy}>
              {bankBusy ? <LoaderCircle size={14} className="spin" /> : <RefreshCw size={14} />} refresh
            </button>
          </div>
        </div>
        <div className="memory-bank-filters">
          <input
            className="memory-bank-text"
            value={bankText}
            onChange={(event) => setBankText(event.target.value)}
            onKeyDown={(event) => { if (event.key === "Enter") void applyBank(); }}
            placeholder="filter by text..."
          />
          <label>type
            <select
              value={bankType}
              onChange={(event) => {
                setBankType(event.target.value);
                void applyBank({ type: event.target.value });
              }}
            >
              <option value="all">all</option>
              <option value="world">world</option>
              <option value="experience">experience</option>
              <option value="observation">observation</option>
              <option value="mental_model">mental model</option>
              <option value="procedure">procedure</option>
              <option value="lesson">lesson</option>
            </select>
          </label>
          <label>state
            <select
              value={bankState}
              onChange={(event) => {
                const next = event.target.value as "active" | "all";
                setBankState(next);
                void applyBank({ state: next });
              }}
            >
              <option value="active">active</option>
              <option value="all">all states</option>
            </select>
          </label>
          <button className="primary-button" onClick={() => void applyBank()} disabled={bankBusy}>
            {bankBusy ? <LoaderCircle size={14} className="spin" /> : <Search size={14} />} apply
          </button>
        </div>
        {bankError && <div className="persona-error"><CircleAlert size={14} /> {bankError}</div>}
        {bankUnits && (
          <div className="memory-bank-count">
            <span>showing {bankUnits.length} of {bankTotal} unit{bankTotal === 1 ? "" : "s"}</span>
          </div>
        )}
        {bankUnits && bankUnits.length === 0 && (
          <div className="blank-card">
            <Search size={18} />
            <strong>no units matched</strong>
            <span>try a different filter, or clear the text search.</span>
          </div>
        )}
        {bankUnits && bankUnits.length > 0 && bankView === "table" && (
          <div className="memory-bank-table">
            <div className="memory-bank-row memory-bank-row-head">
              <span>memory</span>
              <span>type</span>
              <span>scope</span>
              <span>profile</span>
              <span>created</span>
            </div>
            {bankUnits.map((unit) => (
              <Fragment key={unit.id}>
              <div
                className={`memory-bank-row memory-bank-row-selectable${detailId === unit.id ? " memory-bank-row-selected" : ""}`}
                onClick={() => void openUnit(unit.id)}
              >
                <div className="memory-bank-cell">
                  <div className="memory-bank-cell-top">
                    <p>{unit.text}</p>
                    <button
                      className="memory-unit-inspect"
                      title="inspect unit (versions, provenance, curation)"
                      onClick={(event) => { event.stopPropagation(); void openUnit(unit.id); }}
                    >
                      <Eye size={13} />
                    </button>
                  </div>
                  {unit.store_reason && <span className="memory-bank-reason">{unit.store_reason}</span>}
                  {unit.state !== "active" && (
                    <span className="memory-bank-state">{unit.state}{unit.state_reason ? ` - ${unit.state_reason}` : ""}</span>
                  )}
                </div>
                <span className="memory-kind">{unit.type}</span>
                <span className="memory-bank-scope">{unit.scope}</span>
                <span>{unit.profile || "-"}</span>
                <span>{(unit.created_at || "").slice(0, 10) || "-"}</span>
              </div>
              {detailId === unit.id && renderUnitDetail()}
              </Fragment>
            ))}
          </div>
        )}
        {bankUnits && bankUnits.length > 0 && bankView === "timeline" && (
          <div className="memory-bank-timeline">
            {timelineGroups.map((group) => (
              <div key={group.date} className="memory-bank-date">
                <div className="memory-bank-date-head">
                  <span>{group.date}</span>
                  <em>{group.units.length} item{group.units.length === 1 ? "" : "s"}</em>
                </div>
                {group.units.map((unit) => (
                  <Fragment key={unit.id}>
                  <div
                    className={`memory-bank-item memory-bank-item-selectable${detailId === unit.id ? " memory-bank-row-selected" : ""}`}
                    onClick={() => void openUnit(unit.id)}
                  >
                    <div className="memory-bank-cell-top">
                      <p>{unit.text}</p>
                      <button
                        className="memory-unit-inspect"
                        title="inspect unit (versions, provenance, curation)"
                        onClick={(event) => { event.stopPropagation(); void openUnit(unit.id); }}
                      >
                        <Eye size={13} />
                      </button>
                    </div>
                    <div className="memory-result-meta">
                      <span className="memory-kind">{unit.type}</span>
                      <span>{unit.scope}</span>
                      {unit.profile && <span>{unit.profile}</span>}
                      {unit.created_at && <span>{(unit.created_at || "").slice(11, 16)}</span>}
                      {unit.mentioned_at && <span>mentioned {unit.mentioned_at}</span>}
                      {unit.state !== "active" && <span className="memory-bank-state">{unit.state}</span>}
                      {unit.core && (
                        <span className="memory-chip memory-unit-core" title="pinned: injected verbatim into every prompt">
                          <Pin size={11} /> core
                        </span>
                      )}
                    </div>
                  </div>
                  {detailId === unit.id && renderUnitDetail()}
                  </Fragment>
                ))}
              </div>
            ))}
          </div>
        )}
        {bankUnits && bankUnits.length < bankTotal && (
          <div className="memory-bank-loadmore">
            <button className="secondary-button" onClick={() => void loadMoreBank()} disabled={bankBusy}>
              {bankBusy && <LoaderCircle size={14} className="spin" />} load more
            </button>
          </div>
        )}
      </div>
      <div className="memory-queue">
        <div className="memory-queue-head">
          <div>
            <span className="eyebrow">systems / approval</span>
            <h2>memory queue</h2>
            <p>proposed memories wait here until you decide. approving makes a record durable; rejecting discards it. nothing is promoted automatically.</p>
          </div>
          <button className="secondary-button" onClick={() => void loadRecords()} disabled={recordsBusy}>
            {recordsBusy ? <LoaderCircle size={14} className="spin" /> : <RefreshCw size={14} />} refresh
          </button>
        </div>
        {recordsError && <div className="persona-error"><CircleAlert size={14} /> {recordsError}</div>}
        <div className="memory-propose">
          <textarea
            className="memory-propose-text"
            value={draftText}
            onChange={(event) => setDraftText(event.target.value)}
            placeholder="propose a memory to remember (e.g. a preference, decision, or fact)"
            rows={2}
          />
          <div className="memory-propose-row">
            <label>type
              <select value={draftType} onChange={(event) => setDraftType(event.target.value)}>
                <option value="fact">fact</option>
                <option value="preference">preference</option>
                <option value="decision">decision</option>
              </select>
            </label>
            <label>scope
              <select value={draftScope} onChange={(event) => setDraftScope(event.target.value)}>
                <option value="session">session</option>
                <option value="project">project</option>
                <option value="user">user</option>
              </select>
            </label>
            <button className="primary-button" onClick={() => void proposeRecord()} disabled={draftBusy || !draftText.trim()}>
              {draftBusy ? <LoaderCircle size={14} className="spin" /> : <Plus size={14} />} propose
            </button>
          </div>
        </div>
        {recordsBusy && records.length === 0 && (
          <div className="blank-card"><LoaderCircle size={18} className="spin" /><strong>loading queue</strong><span>fetching proposed memories.</span></div>
        )}
        {!recordsBusy && records.length === 0 && (
          <div className="blank-card"><BrainCircuit size={18} /><strong>no proposed memories</strong><span>propose one above, or let a run surface candidates.</span></div>
        )}
        {records.map((record) => (
          <div key={record.id} className={`memory-record memory-record-${record.status}`}>
            <div className="memory-record-meta">
              <span className={`memory-chip memory-record-status-${record.status}`}>{record.status}</span>
              <span className="memory-kind">{record.type}</span>
              <span>#{record.id}</span>
              <span>{record.scope}</span>
              {record.session_id && <span>session {record.session_id}</span>}
              {record.decided_at && <span>decided {formatTime(record.decided_at)}</span>}
            </div>
            <p>{record.text}</p>
            {record.status === "candidate" && (
              <div className="memory-record-actions">
                <button className="primary-button" onClick={() => void decideRecord(record.id, "approve")} disabled={decidingId !== null}>
                  {decidingId === record.id ? <LoaderCircle size={14} className="spin" /> : <Check size={14} />} approve
                </button>
                <button className="secondary-button" onClick={() => void decideRecord(record.id, "reject")} disabled={decidingId !== null}>
                  {decidingId === record.id ? <LoaderCircle size={14} className="spin" /> : <X size={14} />} reject
                </button>
              </div>
            )}
          </div>
        ))}
      </div>
    </div>
  );
}

function WorkspaceView() {
  return <NotWired eyebrow="systems / local files" title="workspace" note="file tools will let the agent read and write within the workspace root." />;
}

// ── observatory (infrastructure observability) ────────────────────────────
// The daemon's bounded snapshot of the host (docs/infra-observability.md):
// RAM/swap/load, docker containers (restart counts surface the kernel's OOM
// kills), and provider leds. Polls /api/v1/infra; nothing is reconstructed
// from the browser.
function formatMb(mb: number): string {
  if (mb < 0) return "-";
  if (mb < 1024) return `${Math.round(mb)} MiB`;
  const gb = mb / 1024;
  return `${gb.toFixed(gb >= 100 ? 0 : 1)} GiB`;
}

function ObservatoryView({ onInfra }: { onInfra: () => Promise<InfraReport> }) {
  const [report, setReport] = useState<InfraReport | null>(null);
  const [error, setError] = useState("");
  const [refreshing, setRefreshing] = useState(false);
  const [autoRefresh, setAutoRefresh] = useState(true);
  const [updatedAt, setUpdatedAt] = useState<number | null>(null);
  const [now, setNow] = useState(() => Date.now());

  const load = useCallback(async () => {
    setRefreshing(true);
    try {
      setReport(await onInfra());
      setUpdatedAt(Date.now());
      setError("");
    } catch (err) {
      setError(err instanceof Error ? err.message : "infra snapshot failed");
    } finally {
      setRefreshing(false);
    }
  }, [onInfra]);

  useEffect(() => {
    void load();
  }, [load]);

  useEffect(() => {
    if (!autoRefresh) return;
    const interval = setInterval(() => void load(), 5000);
    return () => clearInterval(interval);
  }, [autoRefresh, load]);

  useEffect(() => {
    const interval = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(interval);
  }, []);

  const host = report?.host;
  const memUsedMb = host?.available ? Math.max(0, host.memTotalMb - host.memAvailableMb) : 0;
  const memPct = host?.available && host.memTotalMb > 0 ? (memUsedMb / host.memTotalMb) * 100 : 0;
  const swapUsedMb = host?.available ? Math.max(0, host.swapTotalMb - host.swapFreeMb) : 0;
  const swapPct = host?.available && host.swapTotalMb > 0 ? (swapUsedMb / host.swapTotalMb) * 100 : 0;
  const loadHot = host?.available && host.cores > 0 && host.load1 > host.cores;
  const ageSec = updatedAt !== null ? Math.max(0, Math.round((now - updatedAt) / 1000)) : null;

  return (
    <div className="standalone-view">
      <div className="standalone-heading">
        <div>
          <span className="eyebrow">systems / telemetry</span>
          <h1>observatory</h1>
          <p>
            operational facts from the daemon: host memory and load, docker
            containers (restart counts are where the kernel's OOM kills land),
            and provider reachability. refreshes every 5 s while auto is on.
          </p>
        </div>
        <div className="skill-heading-actions">
          <button className="secondary-button" onClick={() => setAutoRefresh((on) => !on)} title="toggle auto-refresh">
            <RefreshCw size={14} className={autoRefresh ? "spin-slow" : undefined} /> {autoRefresh ? "auto" : "manual"}
          </button>
          <button className="secondary-button" onClick={() => void load()} disabled={refreshing}>
            <RefreshCw size={14} className={refreshing ? "spin" : undefined} /> refresh
          </button>
        </div>
      </div>
      {error && (
        <div className="persona-error">
          <CircleAlert size={14} /> {error}
          <button className="secondary-button" onClick={() => void load()}>retry</button>
        </div>
      )}
      {report === null && !error && (
        <div className="blank-card"><LoaderCircle size={18} className="attachment-spinner" /> taking the first snapshot...</div>
      )}
      {report !== null && (
        <>
          {host?.available ? (
            <div className="infra-host">
              <div className="infra-metric">
                <div className="infra-metric-head">
                  <span>memory</span>
                  <span>{formatMb(memUsedMb)} / {formatMb(host.memTotalMb)} · {memPct.toFixed(0)}%</span>
                </div>
                <div className="infra-bar"><div className={`infra-bar-fill ${memPct >= 90 ? "danger" : memPct >= 75 ? "warn" : ""}`} style={{ width: `${Math.min(100, memPct)}%` }} /></div>
                <small>{formatMb(host.memAvailableMb)} available</small>
              </div>
              <div className="infra-metric">
                <div className="infra-metric-head">
                  <span>swap</span>
                  <span>{host.swapTotalMb > 0 ? `${formatMb(swapUsedMb)} / ${formatMb(host.swapTotalMb)} · ${swapPct.toFixed(0)}%` : "none configured"}</span>
                </div>
                <div className="infra-bar"><div className={`infra-bar-fill ${swapPct >= 90 ? "danger" : swapPct >= 50 ? "warn" : ""}`} style={{ width: `${host.swapTotalMb > 0 ? Math.min(100, swapPct) : 0}%` }} /></div>
                <small>{host.swapTotalMb > 0 ? `${formatMb(host.swapFreeMb)} free` : "a 25 GB box runs hot without it"}</small>
              </div>
              <div className="infra-metric">
                <div className="infra-metric-head">
                  <span>load</span>
                  <span className={loadHot ? "infra-load-hot" : undefined}>{host.load1.toFixed(2)} / {host.load5.toFixed(2)} / {host.load15.toFixed(2)}</span>
                </div>
                <div className="infra-bar"><div className={`infra-bar-fill ${loadHot ? "warn" : ""}`} style={{ width: `${host.cores > 0 ? Math.min(100, (host.load1 / host.cores) * 100) : 0}%` }} /></div>
                <small>{host.cores} cores{loadHot ? " · load above core count" : ""}</small>
              </div>
              {report.gpu?.available && report.gpu.devices.map((device) => {
                const vramPct = device.vramTotalMb > 0 ? (device.vramUsedMb / device.vramTotalMb) * 100 : 0;
                return (
                  <div className="infra-metric" key={device.name}>
                    <div className="infra-metric-head">
                      <span>vram</span>
                      <span>{formatMb(device.vramUsedMb)} / {formatMb(device.vramTotalMb)} · {device.utilPct}% util</span>
                    </div>
                    <div className="infra-bar"><div className={`infra-bar-fill ${vramPct >= 90 ? "danger" : vramPct >= 75 ? "warn" : ""}`} style={{ width: `${Math.min(100, vramPct)}%` }} /></div>
                    <small>{device.name} · {formatMb(Math.max(0, device.vramTotalMb - device.vramUsedMb))} free</small>
                  </div>
                );
              })}
            </div>
          ) : (
            <div className="blank-card">host stats unavailable on this platform</div>
          )}
          <div className="infra-panels">
            <div className="infra-panel">
              <div className="infra-panel-head">
                <span>containers</span>
                {report.dockerAvailable ? (
                  <span className="panel-count">{String(report.containers.length).padStart(2, "0")}</span>
                ) : (
                  <span className="infra-muted">docker not reachable from the daemon</span>
                )}
              </div>
              {report.dockerAvailable && report.containers.map((container) => (
                <div className={`infra-container ${container.state === "running" ? "" : "down"}`} key={container.name}>
                  <span className={`infra-led ${container.state === "running" ? (container.health === "unhealthy" ? "warn" : "on") : "off"}`} />
                  <span className="infra-container-name">{container.name}</span>
                  <span className="infra-container-meta">
                    {container.state}
                    {container.health ? ` · ${container.health}` : ""}
                    {container.memMb >= 0 ? ` · ${formatMb(container.memMb)}` : ""}
                  </span>
                  {container.restarts > 0 && (
                    <span className="infra-restarts" title="restarts since install - the kernel's OOM kills land here">{container.restarts} restarts</span>
                  )}
                </div>
              ))}
            </div>
            <div className="infra-panel">
              <div className="infra-panel-head"><span>providers</span></div>
              {report.providers.length === 0 && <div className="infra-muted">no provider endpoints configured</div>}
              {report.providers.map((provider) => (
                <div className="infra-container" key={provider.name}>
                  <span className={`infra-led ${provider.online ? "on" : "off"}`} />
                  <span className="infra-container-name">{provider.name}</span>
                  <span className="infra-container-meta">{provider.endpoint}</span>
                  {provider.online && <span className="infra-latency">{provider.latencyMs} ms</span>}
                </div>
              ))}
            </div>
          </div>
          {ageSec !== null && (
            <div className="infra-updated">updated {ageSec} s ago</div>
          )}
        </>
      )}
    </div>
  );
}

function KnowledgeView() {
  return <NotWired eyebrow="systems / retrieval" title="knowledge" note="uploaded files, directories, and webpages will become a searchable corpus the model can cite." />;
}

// ── Prompt packs: upload, edit, order, and preview modular prompts ─────────

type PackModuleMeta = {
  id: string;
  path: string;
  category: string;
  required: boolean;
  slot: string;
};

function parsePackModules(pack: PromptPackDetail | null): PackModuleMeta[] {
  if (!pack) return [];
  const manifestFile = pack.files.find((file) => file.path === "manifest.json");
  if (!manifestFile) return [];
  try {
    const manifest = JSON.parse(manifestFile.content) as {
      modules?: { id?: string; path?: string; category?: string; required?: boolean; slot?: string }[];
    };
    if (!Array.isArray(manifest.modules)) return [];
    return manifest.modules
      .filter((module) => typeof module.id === "string" && module.id)
      .map((module) => ({
        id: module.id as string,
        path: module.path || "",
        category: module.category || "",
        required: module.required === true,
        slot: module.slot || "",
      }));
  } catch {
    return [];
  }
}

// The manifest may declare named default stacks (e.g. the characters pack's
// agent / roleplay / debrief presets). They are starting points for a
// profile's prompt order, not separate storage.
function parseDefaultStacks(pack: PromptPackDetail | null): { id: string; label: string; order: string[] }[] {
  if (!pack) return [];
  const manifestFile = pack.files.find((file) => file.path === "manifest.json");
  if (!manifestFile) return [];
  try {
    const manifest = JSON.parse(manifestFile.content) as {
      default_stacks?: Record<string, { label?: string; order?: string[] }>;
    };
    if (!manifest.default_stacks || typeof manifest.default_stacks !== "object") return [];
    return Object.entries(manifest.default_stacks)
      .filter(([, stack]) => stack && Array.isArray(stack.order))
      .map(([id, stack]) => ({
        id,
        label: stack.label || id,
        order: (stack.order as string[]).filter((moduleId) => typeof moduleId === "string"),
      }));
  } catch {
    return [];
  }
}

function PromptsView({
  profiles,
  onListPacks,
  onCreatePack,
  onGetPack,
  onUpsertFile,
  onDeleteFile,
  onDeletePack,
  onPreview,
  onSaveProfilePlan,
}: {
  profiles: HarnessProfile[];
  onListPacks: () => Promise<PromptPackSummary[]>;
  onCreatePack: (input: { id: string; name: string; description?: string }) => Promise<PromptPackDetail>;
  onGetPack: (id: string) => Promise<PromptPackDetail>;
  onUpsertFile: (packId: string, path: string, content: string) => Promise<PackValidation>;
  onDeleteFile: (packId: string, path: string) => Promise<PackValidation>;
  onDeletePack: (id: string) => Promise<void>;
  onPreview: (packId: string, plan: string[], profileId?: string) => Promise<PromptPackPreview>;
  onSaveProfilePlan: (profileId: string, packId: string, plan: string[]) => Promise<void>;
}) {
  const [packs, setPacks] = useState<PromptPackSummary[]>([]);
  const [selectedPackId, setSelectedPackId] = useState("");
  const [pack, setPack] = useState<PromptPackDetail | null>(null);
  const [selectedFile, setSelectedFile] = useState("");
  const [draft, setDraft] = useState("");
  const [dirty, setDirty] = useState(false);
  const [validation, setValidation] = useState<PackValidation | null>(null);
  const [newPackOpen, setNewPackOpen] = useState(false);
  const [newPack, setNewPack] = useState({ id: "", name: "", description: "" });
  const [newFilePath, setNewFilePath] = useState("");
  const [planProfileId, setPlanProfileId] = useState("");
  const [preview, setPreview] = useState<PromptPackPreview | null>(null);
  const [busy, setBusy] = useState(false);
  const [flash, setFlash] = useState<{ kind: "ok" | "error"; text: string } | null>(null);
  const [dragIndex, setDragIndex] = useState<number | null>(null);
  const [copied, setCopied] = useState(false);
  // Which manifest default stack is currently loaded into the order, so the
  // selector shows the active choice instead of snapping back to the
  // placeholder after loading.
  const [loadedStackId, setLoadedStackId] = useState("");
  const fileInputRef = useRef<HTMLInputElement>(null);
  const folderInputRef = useRef<HTMLInputElement>(null);

  const notify = useCallback((kind: "ok" | "error", text: string) => {
    setFlash({ kind, text });
    window.setTimeout(() => setFlash(null), 4000);
  }, []);

  const loadPacks = useCallback(async () => {
    const items = await onListPacks();
    setPacks(items);
    return items;
  }, [onListPacks]);

  const loadPack = useCallback(async (id: string) => {
    const detail = await onGetPack(id);
    setPack(detail);
    setValidation(detail.validation);
    return detail;
  }, [onGetPack]);

  // Initial load: packs + first selection.
  useEffect(() => {
    let cancelled = false;
    (async () => {
      try {
        const items = await loadPacks();
        if (cancelled) return;
        if (items.length > 0) {
          setSelectedPackId((current) => current || items[0].id);
        }
      } catch {
        // offline daemon; the user can still retry from the UI
      }
    })();
    return () => { cancelled = true; };
  }, [loadPacks]);

  // Follow the selected pack into the detail view (async load; the reset
  // for "no pack" happens in the selector handler below).
  useEffect(() => {
    if (!selectedPackId) return;
    let cancelled = false;
    (async () => {
      try {
        const detail = await loadPack(selectedPackId);
        if (cancelled) return;
        const first = detail.files.find((file) => file.path !== "manifest.json") || detail.files[0];
        if (first) {
          setSelectedFile((current) => {
            if (current && detail.files.some((file) => file.path === current)) return current;
            return first.path;
          });
        } else {
          setSelectedFile("");
        }
      } catch {
        // surfaced via the pack selector retry
      }
    })();
    return () => { cancelled = true; };
  }, [selectedPackId, loadPack]);

  // The editor shows the stored file content; local edits live in `draft`
  // while dirty, so a reload or file switch never clobbers saved content.
  const baseContent = useMemo(
    () => pack?.files.find((file) => file.path === selectedFile)?.content ?? "",
    [pack, selectedFile],
  );
  const editorValue = dirty ? draft : baseContent;

  const modules = useMemo(() => parsePackModules(pack), [pack]);
  const defaultStacks = useMemo(() => parseDefaultStacks(pack), [pack]);

  // The plan editor starts from the profile's saved stack (empty when the
  // profile is bound to a different pack); local edits (drag, add, remove)
  // live in planOverride until "save stack" persists them.
  const savedPlan = useMemo(() => {
    if (!planProfileId) return [];
    const profile = profiles.find((item) => item.id === planProfileId);
    if (!profile) return [];
    return profile.promptPackId === selectedPackId ? [...profile.promptPlan] : [];
  }, [planProfileId, profiles, selectedPackId]);
  const [planOverride, setPlanOverride] = useState<string[] | null>(null);
  const plan = planOverride ?? savedPlan;

  async function run(action: () => Promise<void>) {
    setBusy(true);
    try {
      await action();
    } catch (error) {
      notify("error", error instanceof Error ? error.message : String(error));
    } finally {
      setBusy(false);
    }
  }

  async function handleCreatePack() {
    const id = newPack.id.trim();
    const name = newPack.name.trim();
    if (!id || !name) return;
    await run(async () => {
      const created = await onCreatePack({ id, name, description: newPack.description.trim() });
      const items = await loadPacks();
      setSelectedPackId(created.id);
      setNewPackOpen(false);
      setNewPack({ id: "", name: "", description: "" });
      notify("ok", `pack '${id}' created (${items.length} total)`);
    });
  }

  async function handleSaveFile() {
    if (!selectedPackId || !selectedFile) return;
    await run(async () => {
      const result = await onUpsertFile(selectedPackId, selectedFile, draft);
      setValidation(result);
      setDirty(false);
      await loadPack(selectedPackId);
      await loadPacks();
      notify("ok", `saved ${selectedFile}`);
    });
  }

  async function handleNewFile() {
    if (!selectedPackId) return;
    const path = newFilePath.trim().replace(/\\/g, "/").replace(/^\//, "");
    if (!path) return;
    await run(async () => {
      const result = await onUpsertFile(selectedPackId, path, "");
      setValidation(result);
      const detail = await loadPack(selectedPackId);
      await loadPacks();
      setSelectedFile(path);
      setNewFilePath("");
      notify("ok", `created ${path}`);
      void detail;
    });
  }

  async function handleDeleteFile() {
    if (!selectedPackId || !selectedFile) return;
    if (selectedFile === "manifest.json" && !window.confirm("delete manifest.json? the pack will be invalid until it is restored.")) return;
    await run(async () => {
      const result = await onDeleteFile(selectedPackId, selectedFile);
      setValidation(result);
      const detail = await loadPack(selectedPackId);
      await loadPacks();
      const next = detail.files.find((file) => file.path !== selectedFile);
      setSelectedFile(next ? next.path : "");
      notify("ok", `deleted ${selectedFile}`);
    });
  }

  async function handleDeletePack() {
    if (!selectedPackId) return;
    if (!window.confirm(`delete pack '${selectedPackId}' and all its files?`)) return;
    await run(async () => {
      await onDeletePack(selectedPackId);
      const items = await loadPacks();
      setSelectedPackId(items[0] ? items[0].id : "");
      setPack(null);
      setValidation(null);
      setPreview(null);
      setPlanOverride(null);
      setLoadedStackId("");
      notify("ok", "pack deleted");
    });
  }

  // Folder uploads double as pack creation: the top folder name becomes the
  // pack id (e.g. selecting 'characters/' creates pack 'characters').
  function packIdFromFolderName(name: string): string {
    const slug = name.toLowerCase().replace(/[^a-z0-9_-]+/g, "-").replace(/^-+|-+$/g, "");
    return slug || "pack";
  }

  function handleUpload(event: ChangeEvent<HTMLInputElement>, stripTopFolder: boolean) {
    const files = event.target.files;
    if (!files || files.length === 0) return;
    void (async () => {
      setBusy(true);
      try {
        let packId = selectedPackId;
        if (!packId) {
          if (!stripTopFolder) {
            notify("error", "no pack selected - use 'upload folder' (the folder name becomes the pack id) or create a pack first");
            return;
          }
          const first = Array.from(files)[0] as File & { webkitRelativePath?: string };
          const topFolder = (first.webkitRelativePath || "").split("/")[0] || first.name;
          packId = packIdFromFolderName(topFolder);
          try {
            await onCreatePack({ id: packId, name: topFolder });
            await loadPacks();
            notify("ok", `created pack '${packId}' from the folder name`);
          } catch (error) {
            if (!String(error).toLowerCase().includes("already exists")) throw error;
            notify("ok", `using the existing pack '${packId}'`);
          }
          setSelectedPackId(packId);
        }
        let count = 0;
        for (const file of Array.from(files)) {
          const relative = (file as File & { webkitRelativePath?: string }).webkitRelativePath || file.name;
          let path = relative.replace(/\\/g, "/").replace(/^\//, "");
          if (stripTopFolder) {
            const parts = path.split("/");
            if (parts.length > 1) path = parts.slice(1).join("/");
          }
          if (!path) continue;
          const content = await file.text();
          await onUpsertFile(packId, path, content);
          count += 1;
        }
        const detail = await loadPack(packId);
        setValidation(detail.validation);
        await loadPacks();
        notify("ok", `uploaded ${count} file(s) into '${packId}'`);
      } catch (error) {
        notify("error", error instanceof Error ? error.message : String(error));
      } finally {
        setBusy(false);
        if (fileInputRef.current) fileInputRef.current.value = "";
        if (folderInputRef.current) folderInputRef.current.value = "";
      }
    })();
  }

  function handleAddModule(moduleId: string) {
    if (!moduleId || plan.includes(moduleId)) return;
    setPlanOverride([...plan, moduleId]);
    setPreview(null);
    setLoadedStackId("");
  }

  // Load a manifest default stack as the working order. Modules that are not
  // declared in the pack's manifest are dropped so validation stays clean.
  function handleLoadStack(stackId: string) {
    if (!stackId) return;
    const stack = defaultStacks.find((item) => item.id === stackId);
    if (!stack) return;
    setPlanOverride(stack.order.filter((moduleId) => modules.some((module) => module.id === moduleId)));
    setPreview(null);
    setLoadedStackId(stackId);
  }

  function handleRemoveModule(index: number) {
    setPlanOverride(plan.filter((_, i) => i !== index));
    setPreview(null);
    setLoadedStackId("");
  }

  function handleDropModule(targetIndex: number) {
    if (dragIndex === null || dragIndex === targetIndex) {
      setDragIndex(null);
      return;
    }
    const next = [...plan];
    const [moved] = next.splice(dragIndex, 1);
    next.splice(targetIndex, 0, moved);
    setPlanOverride(next);
    setDragIndex(null);
    setPreview(null);
    setLoadedStackId("");
  }

  async function handleSavePlan() {
    if (!planProfileId || !selectedPackId) return;
    await run(async () => {
      await onSaveProfilePlan(planProfileId, selectedPackId, plan);
      // The saved profile now carries this stack; drop the local override so
      // the derived savedPlan (refreshed from the server) is authoritative.
      setPlanOverride(null);
      notify("ok", `saved the prompt order for '${planProfileId}'`);
    });
  }

  async function handleUnbindProfile() {
    const profile = profiles.find((item) => item.id === planProfileId);
    if (!profile?.promptPackId) return;
    await run(async () => {
      // An empty pack id restores the profile's legacy identity_prompt path.
      await onSaveProfilePlan(planProfileId, "", []);
      setPlanOverride(null);
      setPreview(null);
      setLoadedStackId("");
      notify("ok", `unbound prompt pack from '${planProfileId}'`);
    });
  }

  async function handlePreview() {
    if (!selectedPackId) return;
    await run(async () => {
      const result = await onPreview(selectedPackId, plan, planProfileId || undefined);
      setPreview(result);
      if (!result.ok) notify("error", result.error || "preview failed");
    });
  }

  async function handleCopyPreview() {
    if (!preview?.prompt) return;
    try {
      await copyText(preview.prompt);
      setCopied(true);
      window.setTimeout(() => setCopied(false), 1500);
    } catch {
      notify("error", "copy unavailable");
    }
  }

  const selectedProfile = profiles.find((item) => item.id === planProfileId);
  const boundElsewhere = selectedProfile && selectedPackId && selectedProfile.promptPackId !== selectedPackId;
  const addableModules = modules.filter((module) => !plan.includes(module.id));

  return (
    <div className="prompt-packs-view">
      <div className="prompt-packs-header">
        <div>
          <span className="eyebrow">systems / prompt packs</span>
          <h1>prompt packs</h1>
          <p>upload and edit modular prompt files, order them per profile, and preview the exact system prompt.</p>
        </div>
        <div className="prompt-packs-actions">
          <select
            className="prompt-packs-select"
            value={selectedPackId}
            onChange={(event) => {
              setSelectedPackId(event.target.value);
              setPack(null);
              setValidation(null);
              setPreview(null);
              setPlanOverride(null);
              setLoadedStackId("");
              setDirty(false);
            }}
            aria-label="prompt pack"
          >
            <option value="">no pack</option>
            {packs.map((item) => (
              <option key={item.id} value={item.id}>
                {item.name} ({item.id}){item.validationStatus === "invalid" ? " · invalid" : ""}
              </option>
            ))}
          </select>
          <button type="button" onClick={() => setNewPackOpen((open) => !open)} disabled={busy}>
            <Plus size={13} /> new pack
          </button>
          <button type="button" onClick={() => fileInputRef.current?.click()} disabled={busy || !selectedPackId} title="add loose files to the selected pack">
            <Upload size={13} /> upload files
          </button>
          <button type="button" onClick={() => folderInputRef.current?.click()} disabled={busy} title="import a tier folder as a pack - the folder name becomes the pack id (e.g. 'characters')">
            <FolderOpen size={13} /> upload folder
          </button>
          <button type="button" onClick={() => void run(async () => { const detail = await loadPack(selectedPackId); setValidation(detail.validation); notify("ok", "validated"); })} disabled={busy || !selectedPackId}>
            <ShieldCheck size={13} /> validate
          </button>
          <button type="button" className="danger" onClick={() => void handleDeletePack()} disabled={busy || !selectedPackId}>
            <Trash2 size={13} /> delete
          </button>
          <input ref={fileInputRef} type="file" multiple hidden onChange={(event) => handleUpload(event, false)} />
          <input ref={folderInputRef} type="file" multiple hidden {...({ webkitdirectory: "" } as Record<string, string>)} onChange={(event) => handleUpload(event, true)} />
        </div>
      </div>

      {newPackOpen && (
        <div className="prompt-packs-newpack">
          <label className="form-field">
            <span>id</span>
            <input value={newPack.id} onChange={(event) => setNewPack((current) => ({ ...current, id: event.target.value }))} placeholder="characters" />
          </label>
          <label className="form-field">
            <span>name</span>
            <input value={newPack.name} onChange={(event) => setNewPack((current) => ({ ...current, name: event.target.value }))} placeholder="characters" />
          </label>
          <label className="form-field">
            <span>description</span>
            <input value={newPack.description} onChange={(event) => setNewPack((current) => ({ ...current, description: event.target.value }))} placeholder="what this pack contains" />
          </label>
          <button type="button" onClick={() => void handleCreatePack()} disabled={busy || !newPack.id.trim() || !newPack.name.trim()}>
            create
          </button>
        </div>
      )}

      {flash && <div className={`prompt-packs-flash ${flash.kind}`}>{flash.text}</div>}

      <div className="prompt-packs-grid">
        <section className="prompt-packs-files" aria-label="pack files">
          <div className="prompt-packs-section-head">
            <h2>files</h2>
            <span className="prompt-packs-count">{pack ? pack.files.length : 0}</span>
          </div>
          <ul className="prompt-packs-filelist">
            {(pack?.files || []).map((file) => (
              <li key={file.path}>
                <button
                  type="button"
                  className={file.path === selectedFile ? "prompt-packs-file prompt-packs-file-active" : "prompt-packs-file"}
                  onClick={() => { setSelectedFile(file.path); setDirty(false); }}
                >
                  <span className="prompt-packs-file-path">{file.path}</span>
                  <span className="prompt-packs-file-meta">{file.content.length} B · r{file.revision}</span>
                </button>
              </li>
            ))}
            {pack && pack.files.length === 0 && <li className="prompt-packs-empty">no files yet - upload or create one</li>}
          </ul>
          <div className="prompt-packs-newfile">
            <input
              value={newFilePath}
              onChange={(event) => setNewFilePath(event.target.value)}
              placeholder="new file path, e.g. shared/policy.md"
              onKeyDown={(event) => { if (event.key === "Enter") void handleNewFile(); }}
            />
            <button type="button" onClick={() => void handleNewFile()} disabled={busy || !selectedPackId || !newFilePath.trim()}>
              <Plus size={12} /> add
            </button>
          </div>
          <div className={`prompt-packs-validation ${validation ? (validation.valid ? "valid" : "invalid") : ""}`}>
            <div className="prompt-packs-validation-head">
              <ShieldCheck size={13} />
              <span>{validation ? (validation.valid ? "pack valid" : "pack invalid") : "not validated"}</span>
            </div>
            {(validation?.errors || []).map((message) => <div key={message} className="prompt-packs-validation-error">{message}</div>)}
            {(validation?.warnings || []).map((message) => <div key={message} className="prompt-packs-validation-warning">{message}</div>)}
          </div>
        </section>

        <section className="prompt-packs-editor" aria-label="file editor">
          <div className="prompt-packs-section-head">
            <h2>{selectedFile || "no file selected"}</h2>
            <span className={dirty ? "prompt-packs-dirty" : ""}>{dirty ? "unsaved changes" : ""}</span>
            <span className="prompt-packs-spacer" />
            <button type="button" onClick={() => void handleSaveFile()} disabled={busy || !selectedFile || !dirty}>
              <Check size={13} /> save
            </button>
            <button type="button" className="danger" onClick={() => void handleDeleteFile()} disabled={busy || !selectedFile}>
              <Trash2 size={13} />
            </button>
          </div>
          <textarea
            className="prompt-packs-textarea"
            value={editorValue}
            spellCheck={false}
            placeholder="file content (markdown)"
            onChange={(event) => { setDraft(event.target.value); setDirty(true); }}
          />
        </section>

        <section className="prompt-packs-stack" aria-label="profile prompt order">
          <div className="prompt-packs-section-head">
            <h2>prompt order</h2>
            <select
              className="prompt-packs-select"
              value={planProfileId}
              onChange={(event) => { setPlanProfileId(event.target.value); setPreview(null); setPlanOverride(null); setLoadedStackId(""); }}
              aria-label="profile this order is saved for"
            >
              <option value="">choose a profile</option>
              {profiles.map((profile) => (
                <option key={profile.id} value={profile.id}>{profile.name}</option>
              ))}
            </select>
          </div>
          <p className="prompt-packs-section-desc">
            Pick a profile, then drag the pack's modules into the order that profile's system prompt should load them.
            Saving binds this order to the profile - preview shows the exact assembled prompt.
          </p>
          {boundElsewhere && (
            <div className="prompt-packs-note">
              {selectedProfile?.promptPackId
                ? `'{selectedProfile?.name}' is currently bound to pack '${selectedProfile?.promptPackId}'. Saving the stack re-binds it to '${selectedPackId}'.`
                : ` '${selectedProfile?.name}' has no prompt pack bound yet - its system prompt comes from the profile's own text. Saving the stack binds it to '${selectedPackId}'.`}
            </div>
          )}
          <ol className="prompt-packs-plan">
            {plan.map((moduleId, index) => {
              const meta = modules.find((module) => module.id === moduleId);
              return (
                <li
                  key={moduleId}
                  draggable
                  onDragStart={() => setDragIndex(index)}
                  onDragOver={(event) => event.preventDefault()}
                  onDrop={() => handleDropModule(index)}
                  onDragEnd={() => setDragIndex(null)}
                  className={dragIndex === index ? "prompt-packs-plan-item dragging" : "prompt-packs-plan-item"}
                >
                  <GripVertical size={13} className="prompt-packs-grip" />
                  <span className="prompt-packs-plan-id">{moduleId}</span>
                  <span className="prompt-packs-plan-category">{meta?.category || "?"}</span>
                  {meta?.required && <Pin size={11} className="prompt-packs-pin" aria-label="required module" />}
                  <button type="button" className="prompt-packs-plan-remove" onClick={() => handleRemoveModule(index)} disabled={meta?.required} aria-label={`remove ${moduleId}`}>
                    <X size={12} />
                  </button>
                </li>
              );
            })}
            {plan.length === 0 && <li className="prompt-packs-empty">no modules in the order yet - add some from the dropdown below</li>}
          </ol>
          <div className="prompt-packs-addmodule">
            <select
              value=""
              onChange={(event) => handleAddModule(event.target.value)}
              aria-label="add a module to the order"
            >
              <option value="">add a module…</option>
              {addableModules.map((module) => (
                <option key={module.id} value={module.id}>{module.id} ({module.category})</option>
              ))}
            </select>
            {defaultStacks.length > 0 && (
              <select
                value={loadedStackId}
                onChange={(event) => handleLoadStack(event.target.value)}
                title="replace the current order with a named preset from the pack's manifest"
                aria-label="load a default stack from the manifest"
              >
                <option value="">load a default stack…</option>
                {defaultStacks.map((stack) => (
                  <option key={stack.id} value={stack.id}>{stack.label}</option>
                ))}
              </select>
            )}
          </div>
          <button type="button" className="prompt-packs-preview-button" onClick={() => void handleSavePlan()} disabled={busy || !planProfileId || plan.length === 0} title={planProfileId ? "bind this order to the profile" : "pick a profile first"}>
            <Check size={14} /> save this order to the profile
          </button>
          {selectedProfile?.promptPackId && (
            <button type="button" className="danger-button" onClick={() => void handleUnbindProfile()} disabled={busy} title="stop using a prompt pack for this profile and return to its identity text">
              <X size={14} /> unbind '{selectedProfile.promptPackId}' from this profile
            </button>
          )}
          <button type="button" className="prompt-packs-preview-button" onClick={() => void handlePreview()} disabled={busy || !selectedPackId || plan.length === 0}>
            <Eye size={14} /> preview the full system prompt
          </button>
          {preview && (
            <div className={`prompt-packs-preview ${preview.ok ? "ok" : "error"}`}>
              <div className="prompt-packs-preview-head">
                <span>
                  {preview.ok
                    ? `${preview.totalBytes.toLocaleString()} bytes · ~${preview.approxTokens.toLocaleString()} tokens · ${preview.modules.length} modules`
                    : preview.error || "preview failed"}
                </span>
                <button type="button" onClick={() => void handleCopyPreview()} disabled={!preview.ok}>
                  {copied ? <Check size={12} /> : <Copy size={12} />} {copied ? "copied" : "copy"}
                </button>
              </div>
              {preview.ok && (
                <>
                  <div className="prompt-packs-preview-meta">
                    {preview.daemonBlocks.map((block) => (
                      <span key={block.name} className="prompt-packs-chip">daemon: {block.name} ({block.bytes} B)</span>
                    ))}
                    {preview.dynamicBlocks.map((block) => (
                      <span key={block} className="prompt-packs-chip prompt-packs-chip-dynamic">{block}</span>
                    ))}
                    <span className="prompt-packs-chip">fingerprint {preview.fingerprint.slice(0, 12)}…</span>
                  </div>
                  <pre className="prompt-packs-preview-text">{preview.prompt}</pre>
                </>
              )}
            </div>
          )}
        </section>
      </div>
    </div>
  );
}

type SkillDraft = {
  name: string;
  description: string;
  content: string;
  disableModelInvocation: boolean;
};

function emptySkillDraft(): SkillDraft {
  return { name: "", description: "", content: "", disableModelInvocation: false };
}

function SkillsView({ onListSkills, onSaveSkill, onDeleteSkill, onListTrash, onRestoreSkill, onDeleteTrashSkill, onSetSkillEnabled }: {
  onListSkills: () => Promise<HarnessSkill[]>;
  onSaveSkill: (input: SkillInput) => Promise<HarnessSkill[]>;
  onDeleteSkill: (name: string) => Promise<HarnessSkill[]>;
  onListTrash: () => Promise<HarnessSkillTrash[]>;
  onRestoreSkill: (name: string) => Promise<HarnessSkill[]>;
  onDeleteTrashSkill: (name: string) => Promise<HarnessSkillTrash[]>;
  onSetSkillEnabled: (name: string, enabled: boolean) => Promise<HarnessSkill[]>;
}) {
  const [skills, setSkills] = useState<HarnessSkill[] | null>(null);
  const [trash, setTrash] = useState<HarnessSkillTrash[] | null>(null);
  const [loadError, setLoadError] = useState("");
  const [expanded, setExpanded] = useState<string | null>(null);
  // null = not editing; "new" = creating; anything else = the name of the
  // skill being edited in place.
  const [editing, setEditing] = useState<string | null>(null);
  const [draft, setDraft] = useState<SkillDraft>(emptySkillDraft());
  const [saving, setSaving] = useState(false);
  const [saveError, setSaveError] = useState("");
  const [confirmDelete, setConfirmDelete] = useState<string | null>(null);
  const [confirmTrashDelete, setConfirmTrashDelete] = useState<string | null>(null);

  const load = useCallback(async () => {
    setLoadError("");
    setSkills(null);
    setTrash(null);
    try {
      const [skillList, trashList] = await Promise.all([onListSkills(), onListTrash()]);
      setSkills(skillList);
      setTrash(trashList);
    } catch (err) {
      setLoadError(err instanceof Error ? err.message : "failed to load skills");
    }
  }, [onListSkills, onListTrash]);

  // Fetch the lists once on mount; the effect body stays side-effect-free
  // (every state update lands in a promise continuation).
  useEffect(() => {
    let active = true;
    Promise.all([onListSkills(), onListTrash()])
      .then(([skillList, trashList]) => {
        if (active) {
          setSkills(skillList);
          setTrash(trashList);
        }
      })
      .catch((err) => { if (active) setLoadError(err instanceof Error ? err.message : "failed to load skills"); });
    return () => { active = false; };
    // onListSkills/onListTrash close over the stable client; fetched once.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const startNew = () => {
    setDraft(emptySkillDraft());
    setSaveError("");
    setEditing("new");
  };

  const startEdit = (skill: HarnessSkill) => {
    setDraft({
      name: skill.name,
      description: skill.description,
      content: skill.content,
      disableModelInvocation: skill.disableModelInvocation,
    });
    setSaveError("");
    setEditing(skill.name);
    setExpanded(skill.name);
  };

  const canSave = !saving && draft.name.trim().length > 0 && draft.description.trim().length > 0;

  const save = async () => {
    if (!canSave) return;
    setSaving(true);
    setSaveError("");
    try {
      const next = await onSaveSkill({
        name: draft.name.trim(),
        description: draft.description.trim(),
        content: draft.content,
        disableModelInvocation: draft.disableModelInvocation,
      });
      setSkills(next);
      setEditing(null);
    } catch (err) {
      setSaveError(err instanceof Error ? err.message : "failed to save the skill");
    } finally {
      setSaving(false);
    }
  };

  const remove = async (name: string) => {
    if (confirmDelete !== name) {
      setConfirmDelete(name);
      window.setTimeout(() => setConfirmDelete((current) => (current === name ? null : current)), 2500);
      return;
    }
    setSaving(true);
    setSaveError("");
    try {
      // Delete moves the skill to the trash (recovery path for
      // model-self-editable skills). Fetch the trash list only after the
      // delete has landed server-side, so the moved skill is visible without
      // a manual refresh (a parallel fetch raced the delete and was stale).
      const next = await onDeleteSkill(name);
      const nextTrash = await onListTrash();
      setSkills(next);
      setTrash(nextTrash);
      setEditing(null);
      setExpanded(null);
    } catch (err) {
      setSaveError(err instanceof Error ? err.message : "failed to delete the skill");
    } finally {
      setSaving(false);
      setConfirmDelete(null);
    }
  };

  const toggleEnabled = async (skill: HarnessSkill) => {
    setSaving(true);
    setSaveError("");
    try {
      setSkills(await onSetSkillEnabled(skill.name, !skill.enabled));
    } catch (err) {
      setSaveError(err instanceof Error ? err.message : "failed to toggle the skill");
    } finally {
      setSaving(false);
    }
  };

  const restore = async (name: string) => {
    setSaving(true);
    setSaveError("");
    try {
      const next = await onRestoreSkill(name);
      const nextTrash = await onListTrash();
      setSkills(next);
      setTrash(nextTrash);
    } catch (err) {
      setSaveError(err instanceof Error ? err.message : "failed to restore the skill");
    } finally {
      setSaving(false);
    }
  };

  const removeTrashed = async (name: string) => {
    if (confirmTrashDelete !== name) {
      setConfirmTrashDelete(name);
      window.setTimeout(() => setConfirmTrashDelete((current) => (current === name ? null : current)), 2500);
      return;
    }
    setSaving(true);
    setSaveError("");
    try {
      setTrash(await onDeleteTrashSkill(name));
    } catch (err) {
      setSaveError(err instanceof Error ? err.message : "failed to delete the trashed skill");
    } finally {
      setSaving(false);
      setConfirmTrashDelete(null);
    }
  };

  const renderForm = (key: string) => (
    <div className="skill-form" key={key}>
      <label className="form-field">
        <span>name</span>
        <input
          value={draft.name}
          disabled={editing !== "new"}
          onChange={(event) => setDraft({ ...draft, name: event.target.value })}
          placeholder="e.g. code-review"
          autoFocus={editing === "new"}
        />
      </label>
      <label className="form-field form-field-wide">
        <span>description</span>
        <input
          value={draft.description}
          onChange={(event) => setDraft({ ...draft, description: event.target.value })}
          placeholder="when the model should use this skill"
        />
      </label>
      <label className="form-field form-field-wide">
        <span>instructions (SKILL.md body)</span>
        <textarea
          rows={10}
          value={draft.content}
          onChange={(event) => setDraft({ ...draft, content: event.target.value })}
          placeholder="the markdown instructions the model loads with the skill tool"
        />
      </label>
      <label className="skill-check">
        <input
          type="checkbox"
          checked={draft.disableModelInvocation}
          onChange={(event) => setDraft({ ...draft, disableModelInvocation: event.target.checked })}
        />
        user-invoked only (hidden from the model's skill catalog)
      </label>
      <small>saves to the workspace's .agents/skills/{"{"}name{"}"}/SKILL.md; a same-named global skill is shadowed.</small>
      <div className="skill-form-actions">
        <button className="secondary-button" onClick={() => setEditing(null)} disabled={saving}>cancel</button>
        <button className="primary-button" onClick={() => void save()} disabled={!canSave}>
          <Check size={14} /> {saving ? "saving..." : "save skill"}
        </button>
      </div>
    </div>
  );

  return (
    <div className="standalone-view">
      <div className="standalone-heading">
        <div>
          <span className="eyebrow">systems / capabilities</span>
          <h1>skills</h1>
          <p>
            markdown instruction packs (SKILL.md) the model loads on demand. the
            system prompt carries only names and descriptions; the full text is
            fetched with the skill tool. skills are stored in the workspace's
            .agents/skills/, where they shadow same-named global skills
            (~/.agents/skills/).
          </p>
        </div>
        <div className="skill-heading-actions">
          <button className="secondary-button" onClick={startNew}>
            <Plus size={14} /> new skill
          </button>
          <button className="secondary-button" onClick={() => void load()}>
            <RefreshCw size={14} /> reload
          </button>
        </div>
      </div>
      {loadError && (
        <div className="persona-error">
          <CircleAlert size={14} /> {loadError}
          <button className="secondary-button" onClick={() => void load()}>retry</button>
        </div>
      )}
      {skills === null && !loadError && (
        <div className="blank-card"><LoaderCircle size={18} className="attachment-spinner" /> loading skills from the harness...</div>
      )}
      {skills !== null && (
        <div className="skills-list">
          <div className="skills-list-head">
            <span>installed skills</span>
            <span className="panel-count">{String(skills.length).padStart(2, "0")}</span>
          </div>
          {editing === "new" && (
            <div className="skill-row">
              <div className="skill-row-head">
                <span className="skill-name">new skill</span>
                <span className="skill-desc">writes the workspace's .agents/skills/&lt;name&gt;/SKILL.md</span>
              </div>
              <div className="skill-detail">
                {saveError && <div className="persona-error"><CircleAlert size={14} /> {saveError}</div>}
                {renderForm("new")}
              </div>
            </div>
          )}
          {skills.length === 0 && editing !== "new" && (
            <div className="blank-card">no skills installed. create one with "new skill", or place a SKILL.md in the workspace's .agents/skills/.</div>
          )}
          {skills.map((skill) => (
            <div className="skill-row" key={skill.name}>
              <button
                type="button"
                className="skill-row-head"
                onClick={() => setExpanded(expanded === skill.name ? null : skill.name)}
              >
                <ChevronDown size={14} className={expanded === skill.name ? "skill-chevron open" : "skill-chevron"} />
                <span className="skill-name">{skill.name}</span>
                {skill.disableModelInvocation && <span className="skill-badge">user-invoked only</span>}
                <span className="skill-desc">{skill.description}</span>
              </button>
              {expanded === skill.name && (
                <div className="skill-detail">
                  {editing === skill.name ? (
                    <>
                      {saveError && <div className="persona-error"><CircleAlert size={14} /> {saveError}</div>}
                      {renderForm(skill.name)}
                    </>
                  ) : (
                    <>
                      {!skill.enabled && <span className="skill-badge">disabled for the model</span>}
                      <span className="skill-path">{skill.path}</span>
                      {skill.files && skill.files.length > 1 && (
                        <span className="skill-files">files: {skill.files.join(" · ")}</span>
                      )}
                      <pre className="skill-content">{skill.content}</pre>
                      <div className="skill-actions">
                        <label className="skill-check">
                          <input
                            type="checkbox"
                            checked={skill.enabled}
                            onChange={() => void toggleEnabled(skill)}
                            disabled={saving}
                          />
                          enabled for the model
                        </label>
                        <button className="secondary-button" onClick={() => startEdit(skill)} disabled={saving}>
                          <Pencil size={12} /> edit
                        </button>
                        <button
                          className={`secondary-button ${confirmDelete === skill.name ? "skill-delete-confirm" : ""}`}
                          onClick={() => void remove(skill.name)}
                          disabled={saving}
                        >
                          <Trash2 size={12} /> {confirmDelete === skill.name ? "sure?" : "delete"}
                        </button>
                      </div>
                    </>
                  )}
                </div>
              )}
            </div>
          ))}
          {trash !== null && (
            <div className="skills-list-trash">
              <div className="skills-list-head">
                <span>trashed skills</span>
                <span className="panel-count">{String(trash.length).padStart(2, "0")}</span>
              </div>
              {trash.length === 0 && (
                <div className="blank-card">nothing trashed. deleting a skill moves it here instead of removing it.</div>
              )}
              {trash.map((entry) => (
                <div className="skill-row" key={entry.name}>
                  <div className="skill-row-head">
                    <span className="skill-name">{entry.name}</span>
                    <span className="skill-desc">trashed - restore to bring it back, or delete to remove it for good</span>
                  </div>
                  <div className="skill-detail">
                    <div className="skill-actions">
                      <button className="secondary-button" onClick={() => void restore(entry.name)} disabled={saving}>
                        <RotateCcw size={12} /> restore
                      </button>
                      <button
                        className={`secondary-button ${confirmTrashDelete === entry.name ? "skill-delete-confirm" : ""}`}
                        onClick={() => void removeTrashed(entry.name)}
                        disabled={saving}
                      >
                        <Trash2 size={12} /> {confirmTrashDelete === entry.name ? "sure?" : "delete"}
                      </button>
                    </div>
                  </div>
                </div>
              ))}
            </div>
          )}
        </div>
      )}
    </div>
  );
}

function ToolsView() {
  return <NotWired eyebrow="systems / actions" title="tools" note="custom tool definitions the harness can call on the model's behalf." />;
}

type ProfileDraft = {
  id: string;
  name: string;
  description: string;
  identityPrompt: string;
  baseModel: string;
  provider: string; // empty = the default provider (settings)
  temperature: string; // empty = inherit the provider's temperature
  vision: "" | "on" | "off"; // empty = inherit the connection's capability
  promptPackId: string; // prompt pack bound to this profile; empty = identityPrompt is used
  childProfileAllowlist: string; // comma-separated worker profile ids; empty = may not spawn
};

function emptyProfileDraft(): ProfileDraft {
  return { id: "", name: "", description: "", identityPrompt: "", baseModel: "", provider: "", temperature: "", vision: "", promptPackId: "", childProfileAllowlist: "" };
}

function profileToDraft(profile: HarnessProfile): ProfileDraft {
  return {
    id: profile.id,
    name: profile.name,
    description: profile.description || "",
    identityPrompt: profile.identityPrompt || "",
    baseModel: profile.baseModel || "",
    provider: profile.provider || "",
    temperature: profile.temperature !== undefined ? profile.temperature.toString() : "",
    vision: profile.vision === true ? "on" : profile.vision === false ? "off" : "",
    promptPackId: profile.promptPackId || "",
    childProfileAllowlist: (profile.childProfileAllowlist || []).join(","),
  };
}

function ProfileEditor({
  mode,
  draft,
  models,
  providers,
  profiles,
  onDraft,
  saving,
  error,
  onSave,
  onCancel,
}: {
  mode: "create" | "edit";
  draft: ProfileDraft;
  models: string[];
  providers: HarnessProvider[];
  profiles: HarnessProfile[];
  onDraft: (draft: ProfileDraft) => void;
  saving: boolean;
  error: string;
  onSave: () => void;
  onCancel: () => void;
}) {
  const set = (patch: Partial<ProfileDraft>) => onDraft({ ...draft, ...patch });
  const canSave = !saving && draft.name.trim().length > 0 && draft.id.trim().length > 0;
  // Spawn allowlist as a checklist (one row per profile) instead of a
  // comma string: checking a row is the whole decision. Stale ids (the
  // profile was deleted) stay visible and survive the save untouched.
  const allowlistIds = draft.childProfileAllowlist.split(",").map((id) => id.trim()).filter(Boolean);
  const staleAllowlistIds = allowlistIds.filter((id) => !profiles.some((profile) => profile.id === id));
  const toggleAllowlist = (id: string) => {
    const next = allowlistIds.includes(id) ? allowlistIds.filter((entry) => entry !== id) : [...allowlistIds, id];
    set({ childProfileAllowlist: next.join(",") });
  };
  return (
    <div className="persona-editor">
      <div className="persona-editor-head">
        <div>
          <span className="eyebrow">{mode === "create" ? "profiles / new" : "profiles / edit"}</span>
          <h1>{mode === "create" ? "new profile" : `edit ${draft.id}`}</h1>
        </div>
        <button className="icon-button" onClick={onCancel} disabled={saving} aria-label="Back to profile list">
          <X size={16} />
        </button>
      </div>
      {error && <div className="persona-error"><CircleAlert size={14} /> {error}</div>}
      <div className="persona-form">
        <label className="form-field">
          <span>name</span>
          <input value={draft.name} onChange={(event) => set({ name: event.target.value })} placeholder="e.g. code reviewer" autoFocus />
        </label>
        <label className="form-field">
          <span>id (slug)</span>
          <input
            value={draft.id}
            onChange={(event) => set({ id: event.target.value })}
            placeholder="code-reviewer"
            disabled={mode === "edit" && draft.id === "orchestrator"}
          />
          {mode === "edit" && draft.id !== "orchestrator" && (
            <small>renaming re-points every session that references this profile</small>
          )}
        </label>
        <label className="form-field form-field-wide">
          <span>description</span>
          <input value={draft.description} onChange={(event) => set({ description: event.target.value })} placeholder="what this profile is for" />
        </label>
        <label className="form-field form-field-wide">
          <span>identity prompt</span>
          <textarea
            rows={6}
            value={draft.identityPrompt}
            onChange={(event) => set({ identityPrompt: event.target.value })}
            placeholder="system text prepended to every run in sessions that use this profile. leave empty to add nothing."
          />
          {draft.promptPackId && (
            <small>prompt pack "{draft.promptPackId}" is bound to this profile: its [PROMPT PACK] document is used as the system prompt, and this identity text is not stacked on top of it. unbind the pack in the prompts view to use this text again.</small>
          )}
        </label>
        <label className="form-field">
          <span>connection</span>
          <select value={draft.provider} onChange={(event) => set({ provider: event.target.value })}>
            <option value="">default provider (settings)</option>
            {providers.map((provider) => <option key={provider.name} value={provider.name}>{provider.name} · {provider.model || provider.endpoint}</option>)}
            {draft.provider && !providers.some((provider) => provider.name === draft.provider) && <option value={draft.provider}>{draft.provider}</option>}
          </select>
        </label>
        <label className="form-field">
          <span>base model</span>
          <select value={draft.baseModel} onChange={(event) => set({ baseModel: event.target.value })}>
            <option value="">inherit the connection's model</option>
            {models.map((model) => <option key={model} value={model}>{model}</option>)}
            {draft.baseModel && !models.includes(draft.baseModel) && <option value={draft.baseModel}>{draft.baseModel}</option>}
          </select>
        </label>
        <label className="form-field">
          <span>temperature</span>
          <input
            type="number"
            min="0"
            max="2"
            step="0.1"
            value={draft.temperature}
            onChange={(event) => set({ temperature: event.target.value })}
            placeholder="default"
          />
        </label>
        <label className="form-field">
          <span>vision</span>
          <select
            value={draft.vision}
            onChange={(event) => set({ vision: event.target.value as ProfileDraft["vision"] })}
          >
            <option value="">inherit the connection's capability</option>
            <option value="on">enabled (accept image attachments)</option>
            <option value="off">disabled (text only)</option>
          </select>
        </label>
        <div className="form-field form-field-wide">
          <span>subagent workers (spawn allowlist)</span>
          <div className="spawn-allowlist">
            {profiles.map((profile) => (
              <label key={profile.id} className={`spawn-allowlist-row${profile.id === draft.id ? " self" : ""}`}>
                <input type="checkbox" checked={allowlistIds.includes(profile.id)} onChange={() => toggleAllowlist(profile.id)} />
                <span className="spawn-allowlist-name">{profile.name || profile.id}</span>
                <span className="spawn-allowlist-id">{profile.id}{profile.id === draft.id ? " (this profile)" : ""}</span>
              </label>
            ))}
            {staleAllowlistIds.map((id) => (
              <div key={id} className="spawn-allowlist-row stale">
                <input type="checkbox" checked disabled />
                <span className="spawn-allowlist-name">{id}</span>
                <span className="spawn-allowlist-id">profile no longer exists</span>
              </div>
            ))}
            {profiles.length === 0 && <div className="spawn-allowlist-empty">no other profiles to spawn yet</div>}
          </div>
          <small>checked profiles are spawnable as worker sessions via agent.spawn. none checked = this profile may not spawn; the tool is not declared for it. checking this profile itself allows recursive spawning.</small>
        </div>
      </div>
      <div className="persona-form-actions">
        <button className="secondary-button" onClick={onCancel} disabled={saving}>cancel</button>
        <button className="primary-button" onClick={onSave} disabled={!canSave}>
          <Check size={14} /> {saving ? "saving..." : mode === "create" ? "create profile" : "save changes"}
        </button>
      </div>
    </div>
  );
}

function ProfilesView({
  profiles,
  models,
  providers,
  onSave,
  onDelete,
}: {
  profiles: HarnessProfile[];
  models: string[];
  providers: HarnessProvider[];
  onSave: (draft: ProfileDraft, mode: "create" | "edit", originalId?: string) => Promise<void>;
  onDelete: (id: string) => Promise<void>;
}) {
  const [editing, setEditing] = useState<string | null>(null); // null = list, "new" = create, otherwise the profile id
  const [draft, setDraft] = useState<ProfileDraft>(emptyProfileDraft());
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState("");

  const openCreate = () => {
    setDraft(emptyProfileDraft());
    setError("");
    setEditing("new");
  };

  const openEdit = (profile: HarnessProfile) => {
    setDraft(profileToDraft(profile));
    setError("");
    setEditing(profile.id);
  };

  const backToList = () => {
    setEditing(null);
    setError("");
  };

  const save = async () => {
    setSaving(true);
    setError("");
    try {
      await onSave(draft, editing === "new" ? "create" : "edit", editing === "new" ? undefined : editing ?? undefined);
      backToList();
    } catch (err) {
      setError(err instanceof Error ? err.message : "save failed");
    } finally {
      setSaving(false);
    }
  };

  const remove = async (profile: HarnessProfile) => {
    setError("");
    try {
      await onDelete(profile.id);
    } catch (err) {
      setError(err instanceof Error ? err.message : "delete failed");
    }
  };

  if (editing !== null) {
    return (

      <div className="standalone-view">
        <ProfileEditor
          mode={editing === "new" ? "create" : "edit"}
          draft={draft}
          models={models}
          providers={providers}
          profiles={profiles}
          onDraft={setDraft}
          saving={saving}
          error={error}
          onSave={() => void save()}
          onCancel={backToList}
        />
      </div>
    );
  }

  return (
    <div className="standalone-view">
      <div className="standalone-heading">
        <div>
          <span className="eyebrow">systems / personas</span>
          <h1>profiles</h1>
          <p>named layers over a base model. identity prompt, model, and temperature per profile.</p>
        </div>
        <button className="primary-button" onClick={openCreate}>
          <Plus size={15} /> new profile
        </button>
      </div>
      {error && <div className="persona-error"><CircleAlert size={14} /> {error}</div>}
      <div className="persona-grid">
        {profiles.map((profile) => (
          <div className="persona-card" key={profile.id}>
            <div className="persona-card-head">
              <span className="persona-avatar">{profile.name.charAt(0).toUpperCase()}</span>
              <div className="persona-card-copy">
                <strong>{profile.name}</strong>
                <span>{profile.description || "no description"}</span>
              </div>
              {profile.id === "orchestrator" && <span className="persona-default">default</span>}
            </div>
            <div className="persona-meta">
              <span>{profile.id}</span>
              <span>{profile.baseModel || "default model"}</span>
              <span>{profile.temperature !== undefined ? `temp ${profile.temperature}` : "default temperature"}</span>
              <span className={`persona-capability ${profile.vision === false ? "persona-capability-off" : "persona-capability-on"}`}>
                <ImageIcon size={11} /> {profile.vision === true ? "vision enabled" : profile.vision === false ? "text only" : "vision: inherit connection"}
              </span>
              <span>{profile.identityPrompt ? "identity prompt set" : "no identity prompt"}</span>
            </div>
            <div className="persona-actions">
              <button onClick={() => openEdit(profile)}>
                <Pencil size={13} /> edit
              </button>
              {profile.id !== "orchestrator" && (
                <button className="persona-delete" onClick={() => void remove(profile)}>
                  <Trash2 size={13} /> delete
                </button>
              )}
            </div>
          </div>
        ))}
      </div>
    </div>
  );
}

// ── providers (named model connections) ──────────────────────────────────

// The sampling parameters that get first-class form fields; everything else a
// provider carries is a custom passthrough parameter.
const PROVIDER_COMMON_PARAMS = new Set([
  "temperature", "max_tokens", "top_p", "top_k", "min_p",
  "frequency_penalty", "presence_penalty", "repeat_penalty", "seed",
  "vision", "no_thinking", "context_window",
]);

interface ProviderDraft {
  name: string;
  endpoint: string;
  model: string;
  apiKey: string;
  temperature: string;
  maxTokens: string;
  topP: string;
  topK: string;
  minP: string;
  frequencyPenalty: string;
  presencePenalty: string;
  repeatPenalty: string;
  seed: string;
  contextWindow: string;
  vision: boolean;
  noThinking: boolean;
  customParams: { key: string; value: string }[];
}

function emptyProviderDraft(): ProviderDraft {
  return {
    name: "", endpoint: "", model: "", apiKey: "",
    temperature: "", maxTokens: "", topP: "", topK: "", minP: "",
    frequencyPenalty: "", presencePenalty: "", repeatPenalty: "", seed: "",
    contextWindow: "", vision: false, noThinking: false, customParams: [],
  };
}

function providerToDraft(provider: HarnessProvider): ProviderDraft {
  const p = provider.params || {};
  const str = (key: string) => {
    const value = p[key];
    return value === undefined || value === null ? "" : String(value);
  };
  return {
    name: provider.name,
    endpoint: provider.endpoint,
    model: provider.model,
    apiKey: "",  // write-only; empty means "no change"
    temperature: str("temperature"),
    maxTokens: str("max_tokens"),
    topP: str("top_p"),
    topK: str("top_k"),
    minP: str("min_p"),
    frequencyPenalty: str("frequency_penalty"),
    presencePenalty: str("presence_penalty"),
    repeatPenalty: str("repeat_penalty"),
    seed: str("seed"),
    contextWindow: str("context_window"),
    vision: p["vision"] === true,
    noThinking: p["no_thinking"] === true,
    customParams: Object.entries(p)
      .filter(([key]) => !PROVIDER_COMMON_PARAMS.has(key))
      .map(([key, value]) => ({ key, value: String(value) })),
  };
}

function draftToParams(draft: ProviderDraft): Record<string, string | number | boolean> {
  const params: Record<string, string | number | boolean> = {};
  const setNumber = (key: string, value: string) => {
    const trimmed = value.trim();
    if (trimmed === "") return;
    const num = Number(trimmed);
    if (Number.isFinite(num)) params[key] = num;
  };
  setNumber("temperature", draft.temperature);
  setNumber("max_tokens", draft.maxTokens);
  setNumber("top_p", draft.topP);
  setNumber("top_k", draft.topK);
  setNumber("min_p", draft.minP);
  setNumber("frequency_penalty", draft.frequencyPenalty);
  setNumber("presence_penalty", draft.presencePenalty);
  setNumber("repeat_penalty", draft.repeatPenalty);
  setNumber("seed", draft.seed);
  setNumber("context_window", draft.contextWindow);
  if (draft.vision) params["vision"] = true;
  if (draft.noThinking) params["no_thinking"] = true;
  for (const { key, value } of draft.customParams) {
    const trimmedKey = key.trim();
    const trimmedValue = value.trim();
    if (!trimmedKey || trimmedValue === "") continue;
    const num = Number(trimmedValue);
    params[trimmedKey] = Number.isFinite(num) ? num : trimmedValue;
  }
  return params;
}

function ProvidersView({
  providers,
  onUpsert,
  onDelete,
  onCheckDraft,
  onCheckSaved,
  defaultProvider,
  onSetPrimary,
}: {
  providers: HarnessProvider[];
  onUpsert: (draft: ProviderDraft) => Promise<void>;
  onDelete: (name: string) => Promise<void>;
  onCheckDraft: (endpoint: string, apiKey: string) => Promise<ProviderCheckResult>;
  onCheckSaved: (name: string) => Promise<ProviderCheckResult>;
  defaultProvider: string;
  onSetPrimary: (name: string) => Promise<void>;
}) {
  const [editing, setEditing] = useState<string | null>(null); // null = list, "new" = create, else the provider name
  const [draft, setDraft] = useState<ProviderDraft>(emptyProviderDraft());
  const [saving, setSaving] = useState(false);
  const [checking, setChecking] = useState(false);
  const [checkResult, setCheckResult] = useState<ProviderCheckResult | null>(null);
  const [error, setError] = useState("");

  const set = (patch: Partial<ProviderDraft>) => setDraft((previous) => ({ ...previous, ...patch }));

  const openCreate = () => {
    setDraft(emptyProviderDraft());
    setCheckResult(null);
    setError("");
    setEditing("new");
  };

  const openEdit = (provider: HarnessProvider) => {
    setDraft(providerToDraft(provider));
    setCheckResult(null);
    setError("");
    setEditing(provider.name);
  };

  const backToList = () => {
    setEditing(null);
    setError("");
    setCheckResult(null);
  };

  const save = async () => {
    setSaving(true);
    setError("");
    try {
      await onUpsert(draft);
      backToList();
    } catch (err) {
      setError(err instanceof Error ? err.message : "save failed");
    } finally {
      setSaving(false);
    }
  };

  const remove = async (provider: HarnessProvider) => {
    setError("");
    try {
      await onDelete(provider.name);
    } catch (err) {
      setError(err instanceof Error ? err.message : "delete failed");
    }
  };

  const checkEndpoint = async () => {
    const endpoint = draft.endpoint.trim();
    if (!endpoint) {
      setError("enter an endpoint to check");
      return;
    }
    setChecking(true);
    setError("");
    try {
      // A draft check (endpoint + optional key) works for unsaved
      // connections. For a saved connection with no new key, check by name
      // so the stored key is used.
      const key = draft.apiKey.trim();
      setCheckResult(key || editing === "new"
        ? await onCheckDraft(endpoint, key)
        : await onCheckSaved(draft.name.trim()));
    } catch (err) {
      setCheckResult({ ok: false, models: [], error: err instanceof Error ? err.message : "check failed" });
    } finally {
      setChecking(false);
    }
  };

  if (editing !== null) {
    return (
      <div className="standalone-view">
        <div className="persona-form">
          <div className="persona-form-head">
            <button className="secondary-button" onClick={backToList}><ChevronLeft size={14} /> connections</button>
            <h2>{editing === "new" ? "new connection" : `edit ${editing}`}</h2>
          </div>
          {error && <div className="persona-error"><CircleAlert size={14} /> {error}</div>}
          <div className="persona-form-grid">
            <label className="persona-field">
              <span>name</span>
              <input value={draft.name} onChange={(event) => set({ name: event.target.value })} placeholder="local-27b" disabled={editing !== "new"} />
              <small>{editing === "new" ? "a stable id profiles can reference" : "the name is the profile's reference; rename by editing the profile"}</small>
            </label>
            <label className="persona-field">
              <span>endpoint</span>
              <input value={draft.endpoint} onChange={(event) => set({ endpoint: event.target.value })} placeholder="http://<deployment-lan>:8080/v1" />
              <small>OpenAI-compatible base URL</small>
            </label>
            <label className="persona-field">
              <span>model</span>
              <input value={draft.model} onChange={(event) => set({ model: event.target.value })} placeholder="qwen3.8-27b" />
              <small>the model id to send</small>
            </label>
            <label className="persona-field">
              <span>api key</span>
              <input type="password" value={draft.apiKey} onChange={(event) => set({ apiKey: event.target.value })} placeholder={editing === "new" ? "bearer token (optional)" : "leave blank to keep the current key"} />
              <small>write-only, never returned in plaintext</small>
            </label>
          </div>
          {checkResult && (
            <div className={`persona-check ${checkResult.ok ? "persona-check-ok" : "persona-check-fail"}`}>
              {checkResult.ok ? (
                <>
                  <Check size={14} /> endpoint reachable, {checkResult.models.length} model{checkResult.models.length === 1 ? "" : "s"}:
                  {checkResult.models.map((model) => (
                    <button key={model.id} className="persona-check-model" onClick={() => set({ model: model.id, contextWindow: model.contextWindow > 0 ? String(model.contextWindow) : draft.contextWindow })}>
                      {model.id}{model.contextWindow > 0 ? ` (ctx ${model.contextWindow})` : ""}
                    </button>
                  ))}
                </>
              ) : (
                <><CircleAlert size={14} /> {checkResult.error || "endpoint unreachable"}</>
              )}
            </div>
          )}
          <div className="persona-section-label">sampling parameters</div>
          <div className="persona-form-grid">
            <label className="persona-field"><span>temperature</span><input value={draft.temperature} onChange={(event) => set({ temperature: event.target.value })} placeholder="0.7" inputMode="decimal" /></label>
            <label className="persona-field"><span>max tokens</span><input value={draft.maxTokens} onChange={(event) => set({ maxTokens: event.target.value })} placeholder="0" inputMode="numeric" /></label>
            <label className="persona-field"><span>top_p</span><input value={draft.topP} onChange={(event) => set({ topP: event.target.value })} placeholder="default" inputMode="decimal" /></label>
            <label className="persona-field"><span>top_k</span><input value={draft.topK} onChange={(event) => set({ topK: event.target.value })} placeholder="default" inputMode="numeric" /></label>
            <label className="persona-field"><span>min_p</span><input value={draft.minP} onChange={(event) => set({ minP: event.target.value })} placeholder="default" inputMode="decimal" /></label>
            <label className="persona-field"><span>frequency penalty</span><input value={draft.frequencyPenalty} onChange={(event) => set({ frequencyPenalty: event.target.value })} placeholder="default" inputMode="decimal" /></label>
            <label className="persona-field"><span>presence penalty</span><input value={draft.presencePenalty} onChange={(event) => set({ presencePenalty: event.target.value })} placeholder="default" inputMode="decimal" /></label>
            <label className="persona-field"><span>repeat penalty</span><input value={draft.repeatPenalty} onChange={(event) => set({ repeatPenalty: event.target.value })} placeholder="default" inputMode="decimal" /></label>
            <label className="persona-field"><span>seed</span><input value={draft.seed} onChange={(event) => set({ seed: event.target.value })} placeholder="default" inputMode="numeric" /></label>
            <label className="persona-field"><span>context window</span><input value={draft.contextWindow} onChange={(event) => set({ contextWindow: event.target.value })} placeholder="auto" inputMode="numeric" /></label>
          </div>
          <div className="persona-hint">temperature 0-2, top_p / min_p 0-1, penalties -2 to 2; top_k / seed / max tokens / context window are integers (0 = provider default)</div>
          <div className="persona-form-grid persona-toggle-grid">
            <label className="vision-field">
              <input type="checkbox" checked={draft.vision} onChange={(event) => set({ vision: event.target.checked })} />
              <span><strong>vision</strong><small>accepts image attachments</small></span>
            </label>
            <label className="vision-field">
              <input type="checkbox" checked={draft.noThinking} onChange={(event) => set({ noThinking: event.target.checked })} />
              <span><strong>no thinking</strong><small>disable the model's reasoning trace</small></span>
            </label>
          </div>
          <div className="persona-section-label">custom parameters <small>(sent verbatim to the API)</small></div>
          <div className="persona-hint">provider/model-specific keys, passed through as-is - servers may ignore unknown ones. Reserved keys (model, messages, stream, stream_options) are dropped by the harness; set common sampling keys in the fields above.</div>
          <div className="custom-params">
            {draft.customParams.map((param, index) => (
              <div className="custom-param-row" key={index}>
                <input value={param.key} placeholder="key" onChange={(event) => {
                  const next = [...draft.customParams];
                  next[index] = { ...param, key: event.target.value };
                  set({ customParams: next });
                }} />
                <input value={param.value} placeholder="value" onChange={(event) => {
                  const next = [...draft.customParams];
                  next[index] = { ...param, value: event.target.value };
                  set({ customParams: next });
                }} />
                <button className="secondary-button" onClick={() => set({ customParams: draft.customParams.filter((_, i) => i !== index) })} aria-label="remove parameter"><X size={13} /></button>
              </div>
            ))}
            <button className="secondary-button" onClick={() => set({ customParams: [...draft.customParams, { key: "", value: "" }] })}>
              <Plus size={13} /> add custom parameter
            </button>
          </div>
          <div className="persona-form-actions">
            <button className="secondary-button" onClick={() => void checkEndpoint()} disabled={checking || !draft.endpoint.trim()}>
              {checking ? <LoaderCircle size={14} className="spin" /> : <Search size={14} />} {checking ? "checking..." : "check endpoint"}
            </button>
            <button className="secondary-button" onClick={backToList} disabled={saving}>cancel</button>
            <button className="primary-button" onClick={() => void save()} disabled={saving || !draft.name.trim() || !draft.endpoint.trim()}>
              <Check size={14} /> {saving ? "saving..." : editing === "new" ? "create connection" : "save changes"}
            </button>
          </div>
        </div>
      </div>
    );
  }

  return (
    <div className="standalone-view">
      <div className="standalone-heading">
        <div>
          <span className="eyebrow">systems / connections</span>
          <h1>connections</h1>
          <p>named model endpoints. each carries its own url, model, key, and parameters; profiles route to one of them.</p>
        </div>
        <button className="primary-button" onClick={openCreate}>
          <Plus size={15} /> new connection
        </button>
      </div>
      {error && <div className="persona-error"><CircleAlert size={14} /> {error}</div>}
      {providers.length === 0 ? (
        <div className="blank-card">
          <Cable size={18} />
          <strong>no connections yet</strong>
          <span>the default provider (settings) is always available. add a named connection to route profiles to a specific endpoint.</span>
        </div>
      ) : (
        <div className="persona-grid">
          {providers.map((provider) => {
            const params = provider.params || {};
            const paramSummary = Object.entries(params)
              .filter(([key]) => !PROVIDER_COMMON_PARAMS.has(key) || key === "vision")
              .map(([key, value]) => `${key}=${value}`)
              .slice(0, 3);
            return (
              <div className="persona-card" key={provider.name}>
                <div className="persona-card-head">
                  <span className="persona-avatar"><Cable size={15} /></span>
                  <div className="persona-card-copy">
                    <strong>{provider.name}</strong>
                    <span>{provider.endpoint}</span>
                  </div>
                  {provider.name === defaultProvider && <span className="persona-default">primary</span>}
                </div>
                <div className="persona-meta">
                  <span>{provider.model || "default model"}</span>
                  <span>{params["temperature"] !== undefined ? `temp ${params["temperature"]}` : "default temperature"}</span>
                  <span className={`persona-capability ${params["vision"] ? "persona-capability-on" : "persona-capability-off"}`}>
                    <ImageIcon size={11} /> {params["vision"] ? "vision enabled" : "text only"}
                  </span>
                  {paramSummary.length > 0 && <span>{paramSummary.join(" · ")}</span>}
                </div>
                <div className="persona-actions">
                  <button onClick={() => void onSetPrimary(provider.name === defaultProvider ? "" : provider.name)}>
                    {provider.name === defaultProvider ? "clear primary" : "make primary"}
                  </button>
                  <button onClick={() => openEdit(provider)}><Pencil size={13} /> edit</button>
                  <button className="persona-delete" onClick={() => void remove(provider)}><Trash2 size={13} /> delete</button>
                </div>
              </div>
            );
          })}
        </div>
      )}
    </div>
  );
}

// A restart-required save is persisted but not applied live, so the
// effective value lags the saved one; show what the user set (the saved
// value) unless a higher-precedence source (cli/env) owns the live value.
function settingCurrent(setting: HarnessSetting): string {
  if (setting.source === "cli" || setting.source === "env") {
    return setting.value === null ? "" : String(setting.value);
  }
  if (setting.saved !== null && setting.saved !== undefined) {
    return String(setting.saved);
  }
  return setting.value === null ? "" : String(setting.value);
}

function SettingRow({
  setting,
  draft,
  onDraft,
  saving,
  error,
  onSave,
  onCheck,
  checking,
  options,
  onSelect,
}: {
  setting: HarnessSetting;
  draft: string;
  onDraft: (value: string) => void;
  saving: boolean;
  error: string;
  onSave: () => void;
  onCheck?: () => void;
  checking?: boolean;
  options?: ProviderModelOption[];
  onSelect?: (id: string) => void;
}) {
  const current = settingCurrent(setting);
  // Secrets are write-only: an empty draft means "leave as is", so clearing
  // the box is not the same as clearing the stored value.
  const dirty = setting.secret ? draft.trim() !== "" && draft !== current : draft !== current;
  const useSelect = options !== undefined && options.length > 0 && setting.type === "string";
  const label = setting.key.includes(".") ? setting.key.split(".").pop() : setting.key;

  return (
    <div className={`setting-row ${setting.active ? "" : "setting-row-inactive"}`}>
      <div className="setting-key">
        <strong>{label}</strong>
        <span>{setting.key} · {setting.type}</span>
      </div>
      {setting.type === "boolean" ? (
        <label className="setting-bool">
          <input
            type="checkbox"
            checked={setting.value === true}
            onChange={onSave}
            disabled={saving}
          />
          <span>{setting.value === true ? "on" : "off"}</span>
        </label>
      ) : useSelect ? (
        <select
          className="setting-input"
          value={draft}
          disabled={saving}
          onChange={(event) => onSelect?.(event.target.value)}
          aria-label={setting.key}
        >
          <option value="">{current || "select a model"}</option>
          {options.map((option) => (
            <option key={option.id} value={option.id}>
              {option.id}{option.contextWindow > 0 ? ` (ctx ${option.contextWindow})` : ""}
            </option>
          ))}
        </select>
      ) : (
        <input
          className="setting-input"
          type={setting.secret ? "password" : "text"}
          inputMode={setting.type === "integer" || setting.type === "number" ? "decimal" : undefined}
          value={draft}
          placeholder={setting.secret ? "write-only" : "default"}
          disabled={saving}
          onChange={(event) => onDraft(event.target.value)}
          onKeyDown={(event) => { if (event.key === "Enter" && dirty) onSave(); }}
          aria-label={setting.key}
        />
      )}
      <div className="setting-badges">
        <span className="setting-badge">from {setting.source}</span>
        {setting.secret && <span className="setting-badge">write-only</span>}
        {setting.restartRequired && <span className="setting-badge setting-badge-warm">restart required</span>}
        {!setting.active && <span className="setting-badge setting-badge-dim">inactive</span>}
      </div>
      {onCheck && (
        <button className="setting-save" onClick={onCheck} disabled={checking}>
          <Search size={12} /> {checking ? "checking..." : "check"}
        </button>
      )}
      {setting.type !== "boolean" && (
        <button className="setting-save" onClick={onSave} disabled={!dirty || saving}>
          <Check size={12} /> save
        </button>
      )}
      {error && <div className="setting-error"><CircleAlert size={12} /> {error}</div>}
    </div>
  );
}

function SettingsView({
  settings,
  loaded,
  loadError,
  onSave,
  onReload,
  onCheckEndpoint,
  onCheckMemory,
  alwaysAllowed,
  onRemoveAlwaysAllowed,
  onReloadAlwaysAllowed,
}: {
  settings: HarnessSetting[];
  loaded: boolean;
  loadError: string;
  onSave: (key: string, value: string | number | boolean) => Promise<void>;
  onReload: () => void;
  onCheckEndpoint: (endpoint: string) => Promise<ProviderCheckResult>;
  onCheckMemory: () => Promise<MemoryStatus>;
  alwaysAllowed: ApprovalRule[];
  onRemoveAlwaysAllowed: (tool: string) => void;
  onReloadAlwaysAllowed: () => void;
}) {
  const groups = useMemo(() => {
    const definitions: { label: string; match: (key: string) => boolean }[] = [
      { label: "daemon", match: (key) => !key.includes(".") },
      // provider.* moved to the connections view (named providers); the
      // daemon still accepts it via CLI/env/API for the default route.
      { label: "auxiliary", match: (key) => key.startsWith("auxiliary.") },
      { label: "summary", match: (key) => key.startsWith("summary.") },
      { label: "tools", match: (key) => key.startsWith("tool.") },
      { label: "memory", match: (key) => key.startsWith("memory.") },
      { label: "web", match: (key) => key.startsWith("web.") },
      { label: "titles", match: (key) => key.startsWith("title.") },
      { label: "subagents", match: (key) => key.startsWith("spawn.") },
      { label: "group", match: (key) => key.startsWith("group.") },
      { label: "terminal", match: (key) => key.startsWith("terminal.") },
    ];
    return definitions
      .map((definition) => ({
        label: definition.label,
        items: settings.filter((setting) => definition.match(setting.key)),
      }))
      .filter((group) => group.items.length > 0);
  }, [settings]);
  // Search over the settings keys (the operator's "which flag did X"
  // lookup): filters what each group shows and hides empty groups, so a
  // hit list replaces the 9-group wall without touching the layout.
  const [settingsQuery, setSettingsQuery] = useState("");
  const query = settingsQuery.trim().toLowerCase();
  const visible = query
    ? (groups
        .map((group) => ({ ...group, items: group.items.filter((setting) => setting.key.toLowerCase().includes(query)) }))
        .filter((group) => group.items.length > 0))
    : groups;

  // Drafts live here (not per row) so one field can feed another: picking a
  // model from the checked endpoint also fills its context window.
  const [drafts, setDrafts] = useState<Record<string, string>>({});
  const [savingKey, setSavingKey] = useState<string | null>(null);
  const [rowErrors, setRowErrors] = useState<Record<string, string>>({});
  // Per-endpoint check state: both provider.endpoint and auxiliary.endpoint
  // are checkable, each with its own model list and status message.
  const [modelOptions, setModelOptions] = useState<Record<string, ProviderModelOption[]>>({});
  const [checkingKey, setCheckingKey] = useState<string | null>(null);
  const [checkMessages, setCheckMessages] = useState<Record<string, { kind: "ok" | "error"; text: string }>>({});
  // Group-level check for the memory models: verifies the custom embed model
  // and reranker actually loaded from their configured paths (the daemon
  // lazily loads them on first status call, so this is the wiring test).
  const [memoryChecking, setMemoryChecking] = useState(false);
  const [memoryCheck, setMemoryCheck] = useState<{ kind: "ok" | "error"; text: string } | null>(null);

  const setDraft = (key: string, value: string) => setDrafts((previous) => ({ ...previous, [key]: value }));

  // Seed drafts for rows we have not touched yet; rows already in flight keep
  // their text when the daemon's refreshed list arrives after a save.
  useEffect(() => {
    setDrafts((previous) => {
      const next = { ...previous };
      let changed = false;
      for (const setting of settings) {
        if (!(setting.key in next)) {
          next[setting.key] = settingCurrent(setting);
          changed = true;
        }
      }
      return changed ? next : previous;
    });
  }, [settings]);

  const saveRow = async (setting: HarnessSetting) => {
    setSavingKey(setting.key);
    setRowErrors((previous) => ({ ...previous, [setting.key]: "" }));
    try {
      let value: string | number | boolean;
      if (setting.type === "boolean") {
        value = setting.value !== true;
      } else if (setting.type === "integer" || setting.type === "number") {
        const text = (drafts[setting.key] ?? "").trim();
        if (text === "") throw new Error("enter a number to change this setting");
        const parsed = Number(text);
        if (!Number.isFinite(parsed)) throw new Error("not a number");
        if (setting.type === "integer" && !Number.isInteger(parsed)) throw new Error("not an integer");
        value = parsed;
      } else {
        value = drafts[setting.key] ?? "";
      }
      await onSave(setting.key, value);
      if (setting.secret) setDraft(setting.key, "");  // write-only: consumed
    } catch (err) {
      setRowErrors((previous) => ({ ...previous, [setting.key]: err instanceof Error ? err.message : "save failed" }));
    } finally {
      setSavingKey(null);
    }
  };

  const checkEndpoint = async (key: string) => {
    const endpoint = (drafts[key] ?? "").trim();
    if (!endpoint) {
      setCheckMessages((previous) => ({ ...previous, [key]: { kind: "error", text: "enter an endpoint first" } }));
      return;
    }
    setCheckingKey(key);
    setCheckMessages((previous) => {
      const next = { ...previous };
      delete next[key];
      return next;
    });
    setModelOptions((previous) => {
      const next = { ...previous };
      delete next[key];
      return next;
    });
    try {
      const result = await onCheckEndpoint(endpoint);
      if (result.ok) {
        setModelOptions((previous) => ({ ...previous, [key]: result.models }));
        setCheckMessages((previous) => ({ ...previous, [key]: { kind: "ok", text: `${result.models.length} model(s) available` } }));
      } else {
        setCheckMessages((previous) => ({ ...previous, [key]: { kind: "error", text: result.error || "endpoint check failed" } }));
      }
    } catch (err) {
      setCheckMessages((previous) => ({ ...previous, [key]: { kind: "error", text: err instanceof Error ? err.message : "endpoint check failed" } }));
    } finally {
      setCheckingKey(null);
    }
  };

  const checkMemoryModels = async () => {
    setMemoryChecking(true);
    setMemoryCheck(null);
    try {
      const status = await onCheckMemory();
      if (!status.enabled) {
        setMemoryCheck({ kind: "error", text: "memory is disabled (memory.enabled)" });
        return;
      }
      if (status.custom) {
        const problems: string[] = [];
        if (!status.custom.ready) problems.push("embed model not loaded");
        // The reranker is optional: recall degrades to RRF-only without it,
        // so an unconfigured reranker is not a failure.
        if (status.custom.rerank_model && !status.custom.reranker_loaded) {
          problems.push(`reranker not loaded (${status.custom.rerank_model})`);
        }
        if (problems.length > 0) {
          setMemoryCheck({ kind: "error", text: problems.join(", ") + (status.error ? ` - ${status.error}` : "") });
        } else {
          const total = Object.values(status.custom.units ?? {}).reduce((sum, n) => sum + n, 0);
          const rerank = status.custom.rerank_model ? "reranker ✓" : "reranker off (RRF-only)";
          setMemoryCheck({ kind: "ok", text: `embed ✓ · ${rerank} · ${total} unit(s)` });
        }
      } else if (status.healthy) {
        setMemoryCheck({ kind: "ok", text: `endpoint healthy · bank ${status.bank || "unknown"}` });
      } else {
        setMemoryCheck({ kind: "error", text: status.error || "memory provider not healthy" });
      }
    } catch (err) {
      setMemoryCheck({ kind: "error", text: err instanceof Error ? err.message : "memory check failed" });
    } finally {
      setMemoryChecking(false);
    }
  };

  const selectModel = (key: string, id: string) => {
    setDraft(key, id);
    if (key === "auxiliary.model") {
      const model = (modelOptions["auxiliary.endpoint"] ?? []).find((option) => option.id === id);
      if (model && model.contextWindow > 0) {
        setDraft("auxiliary.context_window", String(model.contextWindow));
      }
    }
  };

  return (
    <div className="standalone-view">
      <div className="standalone-heading">
        <div>
          <span className="eyebrow">systems / runtime</span>
          <h1>settings</h1>
          <p>typed runtime settings. precedence: cli &gt; env &gt; saved &gt; default. secrets are write-only and never returned in plaintext.</p>
        </div>
        <button className="secondary-button" onClick={onReload}>
          <RefreshCw size={14} /> reload
        </button>
      </div>
      {loadError && (
        <div className="persona-error">
          <CircleAlert size={14} /> {loadError}
          <button className="secondary-button" onClick={onReload}>retry</button>
        </div>
      )}
      <div className="settings-group">
        <div className="settings-group-head">
          <span>approvals</span>
        </div>
        <p className="settings-group-note">always-allowed rules skip the approval gate in every session and survive daemon restarts. they are added from an approval card's "always allow" (scoped to the call's fingerprint when one can be derived) and removed here.</p>
        {alwaysAllowed.length === 0 ? (
          <p className="settings-empty">no tools are always allowed.</p>
        ) : (
          <div className="always-allowed-list">
            {alwaysAllowed.map((rule) => (
              <span className="always-allowed-chip" key={rule.tool}>
                {rule.tool}
                {rule.fingerprint && rule.fingerprint !== "whole tool" && <span className="always-allowed-fingerprint">· {rule.fingerprint}</span>}
                <button type="button" onClick={() => onRemoveAlwaysAllowed(rule.tool)} title={`stop always allowing ${rule.tool}`}>remove</button>
              </span>
            ))}
          </div>
        )}
        <button className="secondary-button" onClick={onReloadAlwaysAllowed}><RefreshCw size={14} /> reload</button>
      </div>
      <div className="settings-search">
        <Search size={14} className="settings-search-icon" />
        <input
          value={settingsQuery}
          placeholder="filter settings by key (e.g. timeout, recall, spawn)"
          onChange={(event) => setSettingsQuery(event.target.value)}
          aria-label="filter settings"
        />
        {settingsQuery && <button type="button" className="settings-search-clear" onClick={() => setSettingsQuery("")} aria-label="clear filter"><X size={12} /></button>}
      </div>
      {query && visible.length === 0 && <div className="settings-empty">no settings match "{settingsQuery}".</div>}
      {!loaded && !loadError && <div className="blank-card"><LoaderCircle size={18} className="attachment-spinner" /> loading settings from the harness...</div>}
      {loaded && visible.map((group) => {
        const isMemory = group.label === "memory";
        const endpointKey = group.label === "auxiliary" ? "auxiliary.endpoint" : null;
        const checkMessage = endpointKey
          ? checkMessages[endpointKey]
          : isMemory
            ? memoryCheck ?? undefined
            : undefined;
        const checking = endpointKey ? checkingKey === endpointKey : isMemory && memoryChecking;
        return (
        <div className="settings-group" key={group.label}>
          <div className="settings-group-head">
            <span>{group.label}</span>
            {checkMessage && (
              <span className={checkMessage.kind === "ok" ? "settings-check-ok" : "settings-check-error"}>
                {checking ? (endpointKey ? "checking endpoint..." : "checking models...") : checkMessage.text}
              </span>
            )}
            {isMemory && (
              <button className="secondary-button" onClick={() => void checkMemoryModels()} disabled={memoryChecking}>
                {memoryChecking ? <LoaderCircle size={14} className="spin" /> : <Search size={14} />} {memoryChecking ? "checking..." : "check models"}
              </button>
            )}
            <span className="panel-count">{String(group.items.length).padStart(2, "0")}</span>
          </div>
          {group.items.map((setting) => {
            const isEndpoint = setting.key === "auxiliary.endpoint";
            const isModel = setting.key === "auxiliary.model";
            const modelKey = "auxiliary.endpoint";
            return (
            <SettingRow
              key={setting.key}
              setting={setting}
              draft={drafts[setting.key] ?? settingCurrent(setting)}
              onDraft={(value) => setDraft(setting.key, value)}
              saving={savingKey === setting.key}
              error={rowErrors[setting.key] || ""}
              onSave={() => void saveRow(setting)}
              onCheck={isEndpoint ? () => void checkEndpoint(setting.key) : undefined}
              checking={checkingKey === setting.key}
              options={isModel ? modelOptions[modelKey] : undefined}
              onSelect={isModel ? (id) => selectModel(setting.key, id) : undefined}
            />
            );
          })}
        </div>
        );
      })}
    </div>
  );
}

// Barrier between a render-time crash and a blank app: ChatView renders
// every state the ledger can produce (including mid-retry merges), so an
// unexpected one must surface as evidence, not as an unmounted tree. The
// boundary displays the exact error (the bug report) and keeps the rest of
// the app alive; a click reloads the view from the ledger (the durable
// state) - always recoverable.
class ChatErrorBoundary extends Component<
  { children: ReactNode },
  { error: Error | null; info: string | null }
> {
  state = { error: null as Error | null, info: null as string | null };
  static getDerivedStateFromError(error: Error) {
    return { error };
  }
  componentDidCatch(error: Error, info: React.ErrorInfo) {
    console.error("chat view render error", error, info.componentStack);
    this.setState({ error, info: info.componentStack ?? null });
  }
  render() {
    if (!this.state.error) return this.props.children;
    return (
      <div className="render-error-panel" role="alert">
        <CircleAlert size={18} />
        <div>
          <strong>the transcript hit a render error</strong>
          <p className="render-error-detail">{String(this.state.error.message || this.state.error)}</p>
          {this.state.info && <pre className="render-error-stack">{this.state.info}</pre>}
          <button type="button" className="primary-button" onClick={() => this.setState({ error: null, info: null })}>
            <RefreshCw size={14} /> reload from the ledger
          </button>
        </div>
      </div>
    );
  }
}

function App() {
  const clientRef = useRef<ReturnType<typeof createHarnessClient> | null>(null);
  if (!clientRef.current) clientRef.current = createHarnessClient();
  const client = clientRef.current;
  // Seed the initial route from the URL hash so a refresh restores the view
  // and session. Parsed once; the ref also tells the session-restore effect
  // whether the URL already selected a session.
  const initialRouteRef = useRef<{ view: View; sessionId?: string } | null>(null);
  if (initialRouteRef.current === null) {
    initialRouteRef.current = parseHash(window.location.hash) ?? { view: "chat" };
  }
  const [view, setView] = useState<View>(initialRouteRef.current.view);
  const [sessions, setSessions] = useState<HarnessSession[]>([]);
  const [currentSessionId, setCurrentSessionId] = useState(initialRouteRef.current.sessionId ?? "");
  const [draftProfile, setDraftProfile] = useState("orchestrator");
  // Group chat (docs/group-chat.md): the speaker roster picked in the
  // composer for the next fresh conversation. 2+ speakers on the first send
  // creates a group session; a single-profile send ignores it. Reset on a
  // new conversation.
  const [groupDraft, setGroupDraft] = useState<string[]>([]);

  // Keep the URL hash in sync with the active view + session (state -> URL),
  // and honor back/forward and manual URL edits (URL -> state). pushState does
  // not fire hashchange, so our own sync cannot trigger the apply handler.
  useEffect(() => {
    const target = hashFor(view, currentSessionId);
    if (window.location.hash !== target) {
      history.pushState(null, "", target);
    }
  }, [view, currentSessionId]);

  useEffect(() => {
    const applyHash = () => {
      const parsed = parseHash(window.location.hash);
      if (!parsed) return;
      setView(parsed.view);
      if (parsed.view === "chat") setCurrentSessionId(parsed.sessionId ?? "");
    };
    window.addEventListener("hashchange", applyHash);
    return () => window.removeEventListener("hashchange", applyHash);
  }, []);
  const [messages, setMessages] = useState<HarnessMessage[]>([]);
  const [run, setRun] = useState<HarnessRun | null>(null);
  // The failed-run bar is a nudge, not a verdict: dismissing it hides it for
  // this run without touching the ledger (the inspector still shows FAILED).
  const [dismissedRetryRun, setDismissedRetryRun] = useState<string[]>(() => {
    // Persisted failure acknowledgements (localStorage): an ErrorBar the
    // operator dismissed stays dismissed across reloads - the run's ledger
    // status remains failed, so an in-memory-only dismiss used to re-render
    // the bar on every refresh.
    try {
      const raw = JSON.parse(window.localStorage.getItem("nyaxus-dismissed-failures") || "[]") as unknown;
      return Array.isArray(raw) ? raw.filter((id): id is string => typeof id === "string") : [];
    } catch {
      return [];
    }
  });
  const dismissFailedRun = useCallback((runId: string | null) => {
    setDismissedRetryRun((previous) => {
      const next = runId
        ? Array.from(new Set([...previous, runId])).slice(-200)
        : previous;
      try {
        window.localStorage.setItem("nyaxus-dismissed-failures", JSON.stringify(next));
      } catch {
        // Private-mode or full storage: the dismiss stays session-local.
      }
      return next;
    });
  }, []);
  const [sessionStatus, setSessionStatus] = useState<HarnessSessionStatus | null>(null);
  // Fetched web sources for the active session; feed the References section
  // under cited replies. Re-fetched when a run ends so final citations land.
  const [sources, setSources] = useState<SourceInfo[]>([]);
  const [events, setEvents] = useState<HarnessEvent[]>([]);
  const [artifacts, setArtifacts] = useState<HarnessSnapshot["artifacts"]>([]);
  const [draft, setDraft] = useState("");
  // Per-session composer drafts: a refresh or a session switch must not eat
  // an in-progress message.
  // Cleared on send (the setDraft("") sites); storage is skipped entirely
  // for draft (sessionless) conversations. Composer seeds (edit-queued,
  // fork) still win - they set both the seed and the draft explicitly.
  const draftKey = (id: string | null) => `nyaxus-draft-${id ?? "new"}`;
  const draftRef = useRef(draft);
  draftRef.current = draft;
  const draftSessionRef = useRef<string | null>(null);
  useEffect(() => {
    if (draftSessionRef.current === currentSessionId) return;
    draftSessionRef.current = currentSessionId;
    setDraft(window.sessionStorage.getItem(draftKey(currentSessionId)) ?? "");
  }, [currentSessionId]);
  useEffect(() => {
    const timer = window.setTimeout(() => {
      const text = draftRef.current;
      if (text) window.sessionStorage.setItem(draftKey(currentSessionId), text);
      else window.sessionStorage.removeItem(draftKey(currentSessionId));
    }, 400);
    return () => window.clearTimeout(timer);
  }, [draft, currentSessionId]);
  const [connected, setConnected] = useState(false);
  // Bumping this re-runs the session-load effect, which closes and re-opens the
  // SSE source. Used to recover a stream that went stale while the tab was
  // backgrounded (browsers suspend network for hidden tabs).
  const [reconnectTick, setReconnectTick] = useState(0);
  // While disconnected, the banner shows which phase is in flight so a slow
  // switch is diagnosable instead of a blank "connection interrupted".
  const [pendingPhase, setPendingPhaseState] = useState<string | null>(null);
  const pendingPhaseRef = useRef<string | null>(null);
  const setPhase = useCallback((label: string | null) => {
    pendingPhaseRef.current = label;
    setPendingPhaseState(label);
  }, []);
  const [sidebarOpen, setSidebarOpen] = useState(false);
  const [pendingJump, setPendingJump] = useState<number | null>(null);
  const [inspectorOpen, setInspectorOpen] = useState(true);
  const [theme, setTheme] = useState<"aero" | "night">("aero");
  const [notifications, setNotifications] = useState<AppNotification[]>([]);
  // In-chat warning state: an active run that went quiet for 60s.
  const [streamStalled, setStreamStalled] = useState(false);
  const [bellOpen, setBellOpen] = useState(false);
  const [actionsOpen, setActionsOpen] = useState(false);
  const [searchOpen, setSearchOpen] = useState(false);
  // Global palette shortcut: Ctrl/Cmd+K opens the search overlay from any
  // view; Esc is handled inside the palette.
  useEffect(() => {
    const onKeyDown = (event: globalThis.KeyboardEvent) => {
      if ((event.ctrlKey || event.metaKey) && (event.key === "k" || event.key === "K")) {
        event.preventDefault();
        setSearchOpen((previous) => !previous);
      }
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, []);
  const [profiles, setProfiles] = useState<HarnessProfile[]>([]);
  const [providers, setProviders] = useState<HarnessProvider[]>([]);
  const [modelOptions, setModelOptions] = useState<{ default: string; models: string[] }>({ default: "", models: [] });
  const [settings, setSettings] = useState<HarnessSetting[]>([]);
  const [settingsLoaded, setSettingsLoaded] = useState(false);
  const [settingsLoadError, setSettingsLoadError] = useState("");
  // Tools approved with the "always" scope (persisted server-side): they run
  // ungated in every session and survive daemon restarts until removed here.
  const [alwaysAllowed, setAlwaysAllowed] = useState<ApprovalRule[]>([]);
  const [queuedPrompts, setQueuedPrompts] = useState<QueuedPrompt[]>([]);
  // Queued prompts the user has marked for steering: the active run ends at
  // its next message boundary and the marked prompt starts the next run. The
  // marker is local UI state, cleared when the run reaches a terminal state.
  const [steeringQueueIds, setSteeringQueueIds] = useState<ReadonlySet<string>>(new Set());
  // A-7, confirmed delivery ids: queue item id -> the run the daemon said
  // received the steer (the resolved slot-holder). Drives the lost-steer
  // check below: a terminal resolved run without the item's run having
  // started means the steer never landed at the boundary.
  const steeringConfirmedRef = useRef<Record<string, string>>({});
  // Steer-flash: a transient strip above the composer that
  // confirms what steer / send-now just did. The key re-triggers the CSS
  // animation; the TTL clears it so it never lingers like a banner.
  const [steerFlash, setSteerFlash] = useState<{ text: string; key: number } | null>(null);
  const steerFlashTimer = useRef<number | undefined>(undefined);
  const flashSteer = useCallback((action: "steer" | "deliver" | "send_now" | "restore") => {
    window.clearTimeout(steerFlashTimer.current);
    setSteerFlash({ text: steerFlashText(action), key: Date.now() });
    steerFlashTimer.current = window.setTimeout(() => setSteerFlash(null), 6000);
  }, []);
  const [composerSeed, setComposerSeed] = useState<ComposerSeed | null>(null);
  // Notification policy is also used by the long-lived event stream callback.
  // Keep the latest view/session data in refs so notification updates do not
  // change the callback identity and restart the stream effect.
  const viewRef = useRef(view);
  const currentSessionIdRef = useRef(currentSessionId);
  const sessionGenerationRef = useRef(0);
  const optimisticSessionIdRef = useRef<string | null>(null);
  const optimisticTurnRef = useRef<{ sessionId: string; user: HarnessMessage } | null>(null);
  const sessionsRef = useRef(sessions);
  viewRef.current = view;
  currentSessionIdRef.current = currentSessionId;
  sessionsRef.current = sessions;

  const messagesRef = useRef<HarnessMessage[]>([]);
  const pendingParentRef = useRef<Record<string, number>>({});
  // After a snapshot merge swaps temp rows for persisted rows, the
  // pending-parent map must follow the swap: a run parented onto a temp id
  // (e.g. a retry onto a failed run's still-streaming partial) dangles when
  // that temp is replaced, and the active-path walk silently truncates the
  // whole lineage above it - the transcript reappears only after the next
  // full merge. Runs when a merge lands; idempotent on repeated application.
  const swapPendingParents = useCallback((swaps: Map<number, number>) => {
    if (swaps.size === 0) return;
    const parents = pendingParentRef.current;
    for (const runId of Object.keys(parents)) {
      const swapped = swaps.get(parents[runId]);
      if (swapped !== undefined) parents[runId] = swapped;
    }
  }, []);
  const queuedPromptsRef = useRef<QueuedPrompt[]>([]);
  const startedRunIdsRef = useRef<Set<string>>(new Set());
  const localRunIdsRef = useRef<Set<string>>(new Set());
  // The most recently initiated run (send or regenerate). Runs execute one at
  // a time per session, so when the daemon starts this run the inspector
  // switches to it even if an older run's state is currently shown.
  const latestRunIdRef = useRef<string | null>(null);
  // The run currently holding the session slot (running, waiting approval,
  // or waiting input). The tracked run can be a queued one, so steer and
  // send-now must target this, not the tracked run.
  const liveRunIdRef = useRef<string | null>(null);
  const approvalRequestRef = useRef<string | null>(null);
  const appliedEventsRef = useRef<Set<string>>(new Set());
  const connectionStateRef = useRef<boolean | null>(null);
  const lastEventAtRef = useRef(Date.now());
  const stallNotifiedRef = useRef(false);
  const lastStallRunIdRef = useRef<string | null>(null);
  const runRef = useRef<HarnessRun | null>(null);
  // The model's real context window from the daemon's health endpoint;
  // the default below is only a fallback when discovery failed.
  const contextWindowRef = useRef(131_072);

  // Rebuilt runs carry their own window from run.started (or 0 = unknown);
  // only runs without one fall back to the global discovery value.
  const withContextWindow = useCallback((value: HarnessRun | null): HarnessRun | null => {
    const window = contextWindowRef.current;
    if (!value || value.metrics.contextWindow !== 0) return value;
    return { ...value, metrics: { ...value.metrics, contextWindow: window } };
  }, []);

  // The primary connection (default provider) name, from health; drives the
  // composer's vision resolution for connection-less profiles.
  const [defaultProvider, setDefaultProvider] = useState("");

  // Re-discover the active provider's context window. The window is a property
  // of the configured model, so it can change on a profile swap or a settings
  // edit; re-fetching keeps the context meter honest without a full reload.
  const refreshHealth = useCallback(() => {
    return client.health().then((health) => {
      setDefaultProvider(health.defaultProvider);
      if (health.contextWindow <= 0) return;
      contextWindowRef.current = health.contextWindow;
      // Health may resolve after the session snapshot; patch the loaded run so
      // the context meter shows the real window in either order. A run that
      // already carries its connection's window always wins.
      setRun((previous) => previous && previous.metrics.contextWindow === 0
        ? { ...previous, metrics: { ...previous.metrics, contextWindow: health.contextWindow } }
        : previous);
    }).catch(() => {
      // Best-effort; the context meter falls back to the default window.
    });
  }, [client]);

  // Seed a fresh run's meter: the discovered window, plus the previous run's
  // usage as a floor. A new prompt always includes the previous one, so the
  // old size is a lower bound until the provider's final usage chunk arrives.
  const seedRunMetrics = useCallback((previous: HarnessRun | null) => ({
    ...initialRunMetrics,
    contextWindow: contextWindowRef.current,
    inputTokens: previous ? previous.metrics.inputTokens : 0,
    contextUsed: previous ? previous.metrics.contextUsed : 0,
  }), []);

  useEffect(() => {
    messagesRef.current = messages;
  }, [messages]);
  const eventsRef = useRef<HarnessEvent[]>([]);
  useEffect(() => {
    eventsRef.current = events;
  }, [events]);

  const setQueue = useCallback((next: QueuedPrompt[] | ((previous: QueuedPrompt[]) => QueuedPrompt[])) => {
    setQueuedPrompts((previous) => {
      const value = typeof next === "function" ? next(previous) : next;
      queuedPromptsRef.current = value;
      return value;
    });
  }, []);

  const promoteQueuedPrompt = useCallback((runId: string) => {
    const item = queuedPromptsRef.current.find((prompt) => prompt.runId === runId);
    if (!item) return;
    setQueue((previous) => previous.filter((prompt) => prompt.runId !== runId));
    setMessages((previous) => {
      if (previous.some((message) => message.runId === runId)) return previous;
      const userTempId = tempMessageId(`queued-user-${runId}`);
      pendingParentRef.current[runId] = userTempId;
      return [
        ...previous,
        {
          id: userTempId,
          parentId: getActiveLeaf(previous),
          role: "user",
          content: item.content,
          createdAt: new Date().toISOString(),
          runId,
          isActive: true,
          attachments: item.attachments.length ? item.attachments : undefined,
        },
      ];
    });
  }, [setQueue]);

  // Withdraw a queued turn before it runs. The daemon cancels the run and
  // deletes its orphaned user message, so nothing dangles on the active path.
  // If the run already started, the cancel still lands and the turn stays in
  // the conversation (an edit of a sent message is rejected, not retracted).
  const removeQueuedPrompt = useCallback(async (item: QueuedPrompt) => {
    try {
      await client.cancelRun(item.runId);
    } catch {
      return;
    }
    setQueue((previous) => previous.filter((prompt) => prompt.runId !== item.runId));
  }, [client, setQueue]);

  // Edit = withdraw + repopulate the composer. The content goes back into the
  // text box as a draft and is not sent until the user presses send; sending
  // again re-queues it at the end of the line.
  const editQueuedPrompt = useCallback(async (item: QueuedPrompt) => {
    try {
      await client.cancelRun(item.runId);
    } catch {
      return;
    }
    setQueue((previous) => previous.filter((prompt) => prompt.runId !== item.runId));
    setDraft(item.content);
    setComposerSeed({
      token: Date.now(),
      sessionId: item.sessionId,
      attachments: item.attachments.map((attachment) => ({ ...attachment })),
    });
  }, [client, setQueue]);

  // Steer: the active run ends at its next message boundary (after the
  // current generation and tool batch), and this queued prompt starts the
  // next run on the released slot.
  const steerQueuedPrompt = useCallback(async (item: QueuedPrompt) => {
    // SESSION-scoped (docs/queue-semantics.md): the daemon resolves the run
    // that holds the slot. The old client-side resolution - tracked run id,
    // falling back to the flipped live-run id - broke exactly after a
    // failed turn or a mid-drain reload: the stale id either no-opped
    // silently or steered a run that no longer owned the slot. In a group
    // session the daemon picks the group run (its live-turn propagation
    // applies the boundary flags).
    if (!currentSessionId) return;
    try {
      await client.steerSession(currentSessionId).then((ack) => {
        // A-7: confirm the steer against the run the daemon resolved - the
        // champion is recorded, so the badge can tell the truth about which
        // run carries the item toward the boundary.
        if (ack.runId) {
          steeringConfirmedRef.current[item.id] = ack.runId;
        }
        // No flash strip for steer: the queue row's STEERING badge already
        // carries the confirmation, and both together rendered as a fake
        // bubble between the queue and the composer.
        setSteeringQueueIds((previous) => new Set(previous).add(item.id));
      });
    } catch (error) {
      notify("failed", `steer failed: ${error instanceof Error ? error.message : String(error)}`, currentSessionId, true);
    }
  }, [client, currentSessionId, flashSteer]);

  // Deliver now: promote this queued prompt into a live mid-turn steer of
  // the run holding the slot. The active run continues with the
  // text in context; the queued run cancels without retracting its message.
  // Same session-scoped resolution as steer.
  const deliverQueuedPromptNow = useCallback(async (item: QueuedPrompt) => {
    if (!currentSessionId) return;
    try {
      await client.deliverSession(currentSessionId, item.runId);
      flashSteer("deliver");
    } catch (error) {
      notify("failed", `deliver failed: ${error instanceof Error ? error.message : String(error)}`, currentSessionId, true);
    }
  }, [client, currentSessionId, flashSteer]);

  // Send now: interrupt the run holding the slot; the queue takes it as soon
  // as the cancel lands. Session-scoped: the group's shared cancel flag
  // closes the roundtable too.
  const sendNowQueuedPrompt = useCallback(async () => {
    if (!currentSessionId) return;
    try {
      await client.cancelSession(currentSessionId);
      flashSteer("send_now");
    } catch (error) {
      notify("failed", `send now failed: ${error instanceof Error ? error.message : String(error)}`, currentSessionId, true);
    }
  }, [client, currentSessionId, flashSteer]);

  useEffect(() => {
    runRef.current = run;
    // A new run starts its own 60s clock. Reset the stall baseline when the
    // run id changes so a stale timestamp from a prior idle period can't fire
    // the "no events for 60s" warning the instant a fresh run goes active.
    const runId = run ? run.id : null;
    if (runId !== lastStallRunIdRef.current) {
      lastStallRunIdRef.current = runId;
      lastEventAtRef.current = Date.now();
      stallNotifiedRef.current = false;
    }
  }, [run]);



  const notify = useCallback((kind: NotificationKind, text: string, sessionId?: string, force = false) => {
    // Focus rule: completions, failures, and approvals are already visible in
    // the chat you are looking at, so they only notify from other places.
    // `force` bypasses the gate for errors that have no in-chat presence
    // (e.g. a blocked session delete).
    const focusGated = kind === "completed" || kind === "failed" || kind === "cancelled" || kind === "approval";
    if (focusGated && !force && viewRef.current === "chat" && sessionId === currentSessionIdRef.current) return;
    const session = sessionId ? sessionsRef.current.find((item) => item.id === sessionId) : undefined;
    const label = session ? ` in "${session.title}"` : "";
    setNotifications((previous) => [
      {
        id: `notice-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
        kind,
        text: `${text}${label}`,
        sessionId,
        time: new Date().toISOString(),
        read: false,
      },
      ...previous,
    ].slice(0, 50));
  }, []);

    useEffect(() => {
    if (!run || isRunActive(run.status)) return;
    // The steered run reached a terminal state. For every item confirmed
    // against it (A-7): if the item's own run had started, the queue item
    // already left via promote - just drop the badge; otherwise the steer
    // never landed at the boundary (the run ended first). The item stays
    // in the queue, badge cleared, and the operator sees why instead of
    // a badge that would claim a delivery eternally pending.
    if (steeringQueueIds.size === 0) return;
    const lost: string[] = [];
    for (const item of queuedPromptsRef.current) {
      if (!steeringQueueIds.has(item.id)) continue;
      const confirmed = steeringConfirmedRef.current[item.id];
      if (!confirmed || confirmed !== run.id) continue;
      if (startedRunIdsRef.current.has(item.runId)) {
        // The queue item's run took the slot; the badge dies with it.
      } else {
        lost.push(item.content.slice(0, 40));
      }
      setSteeringQueueIds((previous) => {
        const next = new Set(previous);
        next.delete(item.id);
        return next;
      });
      delete steeringConfirmedRef.current[item.id];
    }
    if (lost.length) {
      notify(
        "failed",
        lost.length === 1
          ? `the steer didn't land: the run ended before the boundary (\u201c${lost[0]}\u201d stayed queued)`
          : `${lost.length} steers didn't land before the boundary (stayed queued)`,
        currentSessionIdRef.current ?? undefined,
        true,
      );
    }
  }, [run, steeringQueueIds, notify]);
  const handleConnection = useCallback((value: boolean) => {
    setConnected(value);
    const previous = connectionStateRef.current;
    connectionStateRef.current = value;
    if (previous === null) return;  // first observation, not a transition
    if (value && !previous) notify("restored", "connection restored, replaying from the last event");
    if (!value && previous) notify("offline", "connection to the harness lost");
  }, [notify]);

  // When the tab is hidden the browser can suspend the SSE socket, and the
  // source may not notice the drop on its own. On return, if we are not
  // connected, re-open the stream (it resumes from the durable cursor, so no
  // events are lost).
  useEffect(() => {
    const onVisibility = () => {
      if (document.visibilityState === "visible" && connectionStateRef.current !== true) {
        setReconnectTick((tick) => tick + 1);
      }
    };
    document.addEventListener("visibilitychange", onVisibility);
    return () => document.removeEventListener("visibilitychange", onVisibility);
  }, []);

  // Stall detector: an active run that goes quiet for 60s is worth flagging,
  // since a slow or wedged endpoint otherwise looks like a long answer. The
  // flag renders as an in-chat warning bar, not a bell notification.
  useEffect(() => {
    const timer = window.setInterval(() => {
      const current = runRef.current;
      if (!current || !isRunActive(current.status)) {
        stallNotifiedRef.current = false;
        setStreamStalled(false);
        return;
      }
      // A run parked on a gate (approval / question) is intentionally quiet -
      // it is waiting on the user, not wedged. Same for a running TOOL: a
      // long shell command or web fetch streams no ledger events until it
      // finishes, and the old idle clock treated that as a stall.
      if (current.status === "waiting_approval" || current.status === "waiting_input" ||
          current.tools.some((tool) => tool.status === "running")) {
        if (stallNotifiedRef.current) {
          stallNotifiedRef.current = false;
          setStreamStalled(false);
        }
        lastEventAtRef.current = Date.now();
        return;
      }
      // A blocked agent.spawn streams in the worker's own session, not here:
      // the parent's ledger is intentionally quiet while a worker is in
      // flight (the inline subagent block shows the worker live).
      const spawning = current.tools.some((tool) => tool.name === "agent.spawn" && tool.status !== "completed" && tool.status !== "failed");
      if (spawning) {
        if (stallNotifiedRef.current) {
          stallNotifiedRef.current = false;
          setStreamStalled(false);
        }
        return;
      }
      if (!stallNotifiedRef.current && Date.now() - lastEventAtRef.current > 60_000) {
        stallNotifiedRef.current = true;
        setStreamStalled(true);
      }
    }, 5000);
    return () => window.clearInterval(timer);
  }, [setStreamStalled]);

  useEffect(() => {
    document.documentElement.dataset.theme = theme;
  }, [theme]);

  // Dev-only heap probe (?heapprobe=1): tick heap size alongside state byte
  // counts so a live leak can be classified at a glance. Heap tracking state
  // bytes = retention (follow the state); heap climbing with flat state =
  // allocation churn (GC sawtooth, not a true leak).
  const heapProbe = useMemo(() => new URLSearchParams(window.location.search).has("heapprobe"), []);
  useEffect(() => {
    if (!heapProbe) return;
    const t0 = performance.now();
    const timer = window.setInterval(() => {
      const memory = (performance as unknown as { memory?: { usedJSHeapSize: number; totalJSHeapSize: number } }).memory;
      const msgs = messagesRef.current;
      const msgBytes = msgs.reduce((n, item) => n + (item.content?.length || 0) + (item.reasoning?.length || 0), 0);
      const evs = eventsRef.current;
      const eventBytes = evs.reduce((n, item) => n + JSON.stringify(item.payload).length, 0);
      const imageBytes = msgs.reduce((n, item) => n + (item.attachments?.filter((attachment) => attachment.kind === "image").reduce((k, attachment) => k + (attachment.content?.length || 0), 0) || 0), 0);
      console.info("[heapprobe]", JSON.stringify({
        t: Math.round((performance.now() - t0) / 1000),
        heapUsedMb: memory ? Math.round(memory.usedJSHeapSize / 1048576) : null,
        heapTotalMb: memory ? Math.round(memory.totalJSHeapSize / 1048576) : null,
        messages: msgs.length,
        messageTextKb: Math.round(msgBytes / 1024),
        events: evs.length,
        eventPayloadKb: Math.round(eventBytes / 1024),
        imageKb: Math.round(imageBytes / 1024),
        domNodes: document.getElementsByTagName("*").length,
        imgs: document.images.length,
        appliedEvents: appliedEventsRef.current.size,
        pendingParents: Object.keys(pendingParentRef.current).length,
        startedRuns: startedRunIdsRef.current.size,
      }));
    }, 5000);
    return () => window.clearInterval(timer);
  }, [heapProbe]);

  // Dev-only: React DOM's development build records a performance measure
  // for every render and never clears them, so the user-timing timeline
  // grows unbounded through a long session (tens of MB). Nothing consumes
  // them continuously - DevTools profiling reads a fresh window - so clear
  // them on a cadence.
  useEffect(() => {
    if (!import.meta.env.DEV) return;
    const timer = window.setInterval(() => {
      performance.clearMarks();
      performance.clearMeasures();
    }, 60_000);
    return () => window.clearInterval(timer);
  }, []);

  useEffect(() => {
    let active = true;
    client.listSessions().then((items) => {
      if (!active || !items.length) return;
      setSessions(items);
      // The URL is the source of truth on load: only auto-restore the most
      // recent session when the route did not already select one.
      if (!initialRouteRef.current?.sessionId) setCurrentSessionId(items[0].id);
    });
    return () => {
      active = false;
    };
  }, [client]);

  useEffect(() => {
    let active = true;
    client.listProfiles().then((items) => {
      if (active && items.length) setProfiles(items);
    }).catch(() => {
      // Profile list is best-effort; the composer falls back to the session default.
    });
    client.listModels().then((options) => {
      if (active && options.models.length) setModelOptions(options);
    }).catch(() => {
      // Model list is best-effort; the editor falls back to "inherit default".
    });
    void refreshHealth();
    return () => {
      active = false;
    };
  }, [client, refreshHealth]);

  const loadSettings = useCallback(() => {
    setSettingsLoaded(false);
    setSettingsLoadError("");
    return client.listSettings()
      .then((items) => {
        setSettings(items);
        setSettingsLoaded(true);
      })
      .catch((err) => {
        setSettingsLoadError(err instanceof Error ? err.message : "settings unavailable");
      });
  }, [client]);

  useEffect(() => {
    void loadSettings();
  }, [loadSettings]);

  const saveSetting = useCallback(async (key: string, value: string | number | boolean) => {
    const updated = await client.updateSettings({ [key]: value });
    setSettings(updated);
    // Provider/model edits change the active context window; re-discover it so
    // the meter updates without a reload.
    if (key.startsWith("provider.")) void refreshHealth();
  }, [client, refreshHealth]);

  const loadAlwaysAllowed = useCallback(() => {
    return client.listAlwaysAllowed()
      .then(setAlwaysAllowed)
      .catch(() => setAlwaysAllowed([]));
  }, [client]);

  const removeAlwaysAllowedTool = useCallback(async (tool: string) => {
    await client.removeAlwaysAllowed(tool);
    setAlwaysAllowed((previous) => previous.filter((item) => item.tool !== tool));
  }, [client]);

  const reloadSession = useCallback(() => {
    const requestedSessionId = currentSessionId;
    if (!requestedSessionId) return;
    client.loadSession(requestedSessionId).then((snapshot) => {
      if (currentSessionIdRef.current !== requestedSessionId) return;
      const optimisticTurn = optimisticTurnRef.current?.sessionId === requestedSessionId
        ? optimisticTurnRef.current
        : null;
      setSessions((previous) => previous.map((item) => item.id === snapshot.session.id ? { ...item, ...snapshot.session } : item));
      const queuedRunIds = new Set(queuedPromptsRef.current.map((item) => item.runId));
      // A mid-stream reload (e.g. session.rewound from another tab) must not
      // drop the live assistant turn: the daemon persists it only when the
      // run ends, so live temp messages are merged back in. A just-created
      // session also keeps its exact optimistic user row until it is durable.
      setMessages((previous) => {
        const parentSwaps = new Map<number, number>();
        const next = mergeOptimisticSnapshot(
          previous,
          snapshot.messages,
          localRunIdsRef.current,
          queuedRunIds,
          optimisticTurn,
          parentSwaps,
        );
        swapPendingParents(parentSwaps);
        return next;
      });
      // A run in flight that this client did not start (another tab) has no
      // persisted assistant message yet; rebuild it from the run's
      // accumulated deltas, as the session-switch path does.
      if (snapshot.run && isRunActive(snapshot.run.status) && (snapshot.run.reasoning || snapshot.run.content)) {
        const inFlight = snapshot.run;
        setMessages((previous) => seedAssistantFromRun(previous, inFlight.id, inFlight.reasoning || "", inFlight.content || ""));
      }
      const localRun = runRef.current;
      setRun(optimisticTurn && !snapshot.run && localRun?.sessionId === requestedSessionId
        ? localRun
        : withContextWindow(snapshot.run));
    }).catch(() => {
      // Keep the live view on reload failure.
    });
  }, [client, currentSessionId, withContextWindow]);

  // Character state log: the latest label feeds the toolbar chip. Fetched on
  // session switch and re-fetched whenever a status.set tool completes in the
  // stream (below). Unavailable on older daemons; the chip simply stays hidden.
  useEffect(() => {
    if (!currentSessionId) {
      setSessionStatus(null);
      return;
    }
    let active = true;
    client.getSessionStatus(currentSessionId).then((status) => {
      if (active) setSessionStatus(status);
    }).catch(() => {
      if (active) setSessionStatus(null);
    });
    return () => { active = false; };
  }, [client, currentSessionId]);

  // Web sources back the References section under cited replies. Fetch on
  // session switch and on run-status transitions (citations finalize at run
  // end); the daemon keeps them per session, so a re-fetch is cheap.
  useEffect(() => {
    if (!currentSessionId) {
      setSources([]);
      return;
    }
    let active = true;
    client.listSources(currentSessionId).then((items) => {
      if (active) setSources(items);
    }).catch(() => {
      if (active) setSources([]);
    });
    return () => { active = false; };
  }, [client, currentSessionId, run?.status]);

  useEffect(() => {
    if (!currentSessionId) return;
    let active = true;
    let streamStop: (() => void) | undefined;
    const t0 = performance.now();
    const trace = (label: string) =>
      console.info(`[harness] ${label} +${Math.round(performance.now() - t0)}ms`, currentSessionId);
    setConnected(false);
    setPhase("loading session snapshot");
    trace("switch start, loading snapshot");
    client.loadSession(currentSessionId).then((snapshot) => {
      if (!active) return;
      trace("snapshot loaded");
      const optimisticTurn = optimisticTurnRef.current?.sessionId === currentSessionId
        ? optimisticTurnRef.current
        : null;
      const optimisticSession = optimisticSessionIdRef.current === currentSessionId || !!optimisticTurn;
      setSessions((previous) => previous.map((item) => item.id === snapshot.session.id ? { ...item, ...snapshot.session } : item));
      const queuedRunIds = new Set(queuedPromptsRef.current.map((item) => item.runId));
      const snapshotMessages = snapshot.messages.filter((message) => !message.runId || !queuedRunIds.has(message.runId));
      setMessages((previous) => {
        if (!optimisticSession) return snapshotMessages;
        const parentSwaps = new Map<number, number>();
        const next = mergeOptimisticSnapshot(previous, snapshot.messages, localRunIdsRef.current, queuedRunIds, optimisticTurn, parentSwaps);
        swapPendingParents(parentSwaps);
        return next;
      });
      // A run in flight has no persisted assistant message yet; rebuild it
      // from the run's accumulated deltas (reconstructed from the snapshot's
      // events) so a refresh mid-stream does not drop the reasoning/response
      // streamed before the reload. Live deltas then append to it by run id.
      if (snapshot.run && isRunActive(snapshot.run.status) && (snapshot.run.reasoning || snapshot.run.content)) {
        const inFlight = snapshot.run;
        setMessages((previous) => seedAssistantFromRun(previous, inFlight.id, inFlight.reasoning || "", inFlight.content || ""));
      }
      const localRun = runRef.current;
      setRun(optimisticSession && !snapshot.run && localRun?.sessionId === currentSessionId
        ? localRun
        : withContextWindow(snapshot.run));
      // Seed the slot-holder from the snapshot: its run.started is already in
      // appliedEventsRef (seeded below), so the stream's replay skips it and
      // never sets the ref. Clear it when this session holds no live run.
      liveRunIdRef.current = snapshot.run && isRunActive(snapshot.run.status)
        ? snapshot.run.id
        : null;
      // A queue item matching the live run is that run, not a queued prompt:
      // its run.started was deduped away with the snapshot, so promote it
      // here or it sits in the queue as "waiting" while it streams.
      if (liveRunIdRef.current) promoteQueuedPrompt(liveRunIdRef.current);
      setEvents(snapshot.events);
      setArtifacts(snapshot.artifacts);
      // Seed the applied-event set from the snapshot so events replayed by the
      // resumable SSE stream are not re-applied on top of the snapshot state.
      appliedEventsRef.current = new Set(snapshot.events.map((event) => event.eventId));
      // Start streaming only after the snapshot is applied to prevent
      // the snapshot from overwriting live deltas or creating duplicates.
      setPhase("opening event stream");
      trace("opening event stream");
      // Batch stream events into one render per flush window (FLUSH_MS). A
      // reasoning-delta burst would otherwise re-render the whole conversation
      // per delta; over a long run that churn balloons the renderer's off-heap
      // allocator (PartitionAlloc) while the JS heap stays flat.
      let pendingEvents: HarnessEvent[] = [];
      let flushScheduled = false;
      const applyEvent = (event: HarnessEvent) => {
        setEvents((previous) => advanceEventWindow(previous, event));
        if (event.type === "reasoning.delta") {
          const text = String(event.payload.text || "");
          setRun((previous) => previous && previous.id === event.runId ? { ...previous, retry: undefined, reasoning: `${previous.reasoning}${text}` } : previous);
          setMessages((previous) =>
            upsertAssistant(previous, event, pendingParentRef.current, text, ""),
          );
        }
        if (event.type === "text.delta") {
          const text = String(event.payload.text || "");
          setMessages((previous) =>
            upsertAssistant(previous, event, pendingParentRef.current, undefined, text),
          );
          setRun((previous) => previous && previous.id === event.runId && previous.retry
            ? { ...previous, retry: undefined }
            : previous);
        }
        if (event.type === "tool.started") {
          const id = String(event.payload.tool_id || event.eventId);
          const tool: HarnessTool = {
            id,
            name: String(event.payload.name || "tool"),
            target: String(event.payload.target || "runtime"),
            status: "running",
            startedAt: event.emittedAt,
            ...(event.payload.arguments !== undefined ? { arguments: String(event.payload.arguments || "") } : {}),
          };
          setRun((previous) => {
            if (!previous) return previous;
            const tools = previous.tools.some((candidate) => candidate.id === id)
              ? previous.tools.map((candidate) => candidate.id === id ? { ...candidate, ...tool } : candidate)
              : [...previous.tools, tool];
            return { ...previous, retry: undefined, tools, status: "running" };
          });
        }
        if (event.type === "tool.completed") {
          const id = String(event.payload.tool_id || event.eventId);
          const result = String(event.payload.result || event.payload.summary || event.payload.error || "");
          setRun((previous) => {
            if (!previous) return previous;
            const completed: HarnessTool = {
              id,
              name: String(event.payload.name || "tool"),
              target: String(event.payload.target || "runtime"),
              status: event.payload.error ? "failed" : "completed",
              durationMs: Number(event.payload.duration_ms || 0),
              result,
              summary: String(event.payload.summary || result),
            };
            const tools = previous.tools.some((candidate) => candidate.id === id)
              ? previous.tools.map((candidate) => candidate.id === id ? {
                ...candidate,
                status: completed.status,
                durationMs: completed.durationMs,
                result: completed.result,
                summary: completed.summary,
                ...(event.payload.name !== undefined ? { name: completed.name } : {}),
                ...(event.payload.target !== undefined ? { target: completed.target } : {}),
              } : candidate)
              : [...previous.tools, completed];
            return { ...previous, tools };
          });
          if (String(event.payload.name || "") === "status.set" && !event.payload.error) {
            // The character just recorded a new state; refresh the chip.
            client.getSessionStatus(event.sessionId).then((status) => {
              if (currentSessionIdRef.current === event.sessionId) setSessionStatus(status);
            }).catch(() => {
              // Status log unavailable; the chip keeps its last value.
            });
          }
        }
        if (event.type === "run.started") {
          startedRunIdsRef.current.add(event.runId);
          liveRunIdRef.current = event.runId;
          promoteQueuedPrompt(event.runId);
          // The resolved context window for this run's connection; the seeded
          // global fallback is replaced only when the daemon reports one.
          const window = Number(event.payload.context_window) || 0;
          // A queued run only starts once the session slot frees up. Switch
          // the inspector to it when it is the newest run the user initiated;
          // ignore starts of older runs that finish first.
          setRun((previous) => {
            if (!previous) return previous;
            if (previous.id === event.runId) {
              return { ...previous, status: "running", model: String(event.payload.model || previous.model), provider: String(event.payload.provider || previous.provider), startedAt: previous.startedAt || event.emittedAt, metrics: window > 0 ? { ...previous.metrics, contextWindow: window } : previous.metrics };
            }
            if (latestRunIdRef.current === event.runId) {
              const seeded = seedRunMetrics(runRef.current);
              return { id: event.runId, sessionId: previous.sessionId, profile: String(event.payload.profile || previous.profile), model: String(event.payload.model || ""), provider: String(event.payload.provider || previous.provider), status: "running", startedAt: event.emittedAt, reasoning: "", tools: [], metrics: window > 0 ? { ...seeded, contextWindow: window } : seeded };
            }
            return previous;
          });
        }
        if (event.type === "group.turn.started") {
          // Group chat (docs/group-chat.md): a speaker turn is about to stream.
          // Pin its parent to the current active leaf so the turn's assistant
          // message chains onto the previous turn (not onto the user message -
          // without this every turn is a sibling of the user message and the
          // active path drops all but the last one, the "stale transcript"
          // that a refresh fixes). The leaf is read inside setMessages so it
          // reflects the latest committed state. The turn run also joins the
          // live-run set, because a snapshot merge fires on every turn's
          // completion and must keep OTHER in-flight turn temps (the live
          // ones) rather than dropping them from the watch view.
          const turnRun = String(event.payload.turn_run || "");
          if (turnRun) {
            localRunIdsRef.current.add(turnRun);
            setMessages((previous) => {
              pendingParentRef.current[turnRun] = getActiveLeaf(previous) ?? 0;
              return previous;
            });
          }
        }
        if (event.type === "run.retrying") {
          // The daemon is between a failed provider attempt and its retry.
          setRun((previous) => previous && previous.id === event.runId
            ? { ...previous, retry: { attempt: Number(event.payload.attempt || 1), maxAttempts: Number(event.payload.max_attempts || 1), reason: String(event.payload.reason || "provider error") } }
            : previous);
        }
        if (event.type === "run.stalled") {
          // Stall watchdog fired (provider went quiet for the silence cap).
          // The retry bar doubles as the stall notice: it clears on the next
          // delta, on a metrics snapshot, or on the terminal run.failed that
          // follows a mid-stream stall.
          setRun((previous) => previous && previous.id === event.runId
            ? { ...previous, retry: { attempt: Number(event.payload.attempt || 1), maxAttempts: Number(event.payload.max_attempts || 2), reason: String(event.payload.reason || "provider stalled") } }
            : previous);
        }
        if (event.type === "metrics.snapshot") {
          // The provider's usage for the latest request: prompt tokens are
          // exactly the context used, completion tokens the output produced.
          const prompt = Number(event.payload.prompt_tokens || 0);
          const completion = Number(event.payload.completion_tokens || 0);
          const reasoning = Number(event.payload.reasoning_tokens || 0);
          // A snapshot that reports cached tokens proves the cache is hit
          // again, so the tripwire alarm (if any) is stale. Absent field =
          // the provider reports no cache data: leave the alarm alone.
          const cached = event.payload.cached_tokens === undefined ? -1 : Number(event.payload.cached_tokens);
          setRun((previous) => previous && previous.id === event.runId ? {
            ...previous,
            retry: undefined,
            kvCacheMiss: cached > 0 ? undefined : previous.kvCacheMiss,
            metrics: {
              ...previous.metrics,
              inputTokens: prompt || previous.metrics.inputTokens,
              outputTokens: completion || previous.metrics.outputTokens,
              reasoningTokens: reasoning || previous.metrics.reasoningTokens,
              contextUsed: prompt || previous.metrics.contextUsed,
              contextWindow: previous.metrics.contextWindow > 0 ? previous.metrics.contextWindow : contextWindowRef.current,
            },
          } : previous);
        }
        if (event.type === "kv_cache_miss") {
          // Prompt-cache tripwire: the latest provider call re-prefilled a
          // long prefix. The bar stays up until a later snapshot proves a
          // cache hit (or the run ends).
          setRun((previous) => previous && previous.id === event.runId
            ? { ...previous, kvCacheMiss: { promptTokens: Number(event.payload.prompt_tokens || 0), kind: String(event.payload.kind || "cold") } }
            : previous);
        }
        if (event.type === "run.completed") {
          localRunIdsRef.current.delete(event.runId);
          delete pendingParentRef.current[event.runId];
          if (liveRunIdRef.current === event.runId) liveRunIdRef.current = null;
          if (optimisticTurnRef.current?.sessionId === event.sessionId) {
            optimisticSessionIdRef.current = null;
            optimisticTurnRef.current = null;
          }
          setQueue((previous) => previous.filter((item) => item.runId !== event.runId));
          setRun((previous) => previous && previous.id === event.runId ? { ...previous, retry: undefined, status: "completed", finishedAt: event.emittedAt, approval: undefined, metrics: { ...previous.metrics, outputTokens: Number(event.payload.output_tokens || previous.metrics.outputTokens) } } : previous);
          notify("completed", "run completed", event.sessionId);
          // OS notification while the tab is hidden: an in-app bell is
          // invisible when the browser is backgrounded.
          // Excerpt from the completed run's answer; no sound.
          if (document.hidden && "Notification" in window) {
            const publish = () => {
              const answer = messagesRef.current
                .filter((message) => message.role === "assistant" && message.runId === event.runId)
                .pop();
              const body = (answer?.content || "run completed").replace(/\s+/g, " ").slice(0, 140);
              try { new Notification("harness completed a run", { body }); } catch { /* denied */ }
            };
            if (Notification.permission === "granted") publish();
            else if (Notification.permission !== "denied") {
              void Notification.requestPermission().then((granted) => {
                if (granted === "granted") publish();
              }).catch(() => { /* the prompt was ignored */ });
            }
          }
          reloadSession();
        }
        if (event.type === "run.cancelled") {
          localRunIdsRef.current.delete(event.runId);
          delete pendingParentRef.current[event.runId];
          if (liveRunIdRef.current === event.runId) liveRunIdRef.current = null;
          if (optimisticTurnRef.current?.sessionId === event.sessionId) {
            optimisticSessionIdRef.current = null;
            optimisticTurnRef.current = null;
          }
          setQueue((previous) => previous.filter((item) => item.runId !== event.runId));
          setRun((previous) => previous && previous.id === event.runId ? {
            ...previous,
            retry: undefined,
            status: "cancelled",
            finishedAt: event.emittedAt,
            approval: undefined,
            tools: previous.tools.map((tool) => tool.status === "running" || tool.status === "waiting_approval"
              ? { ...tool, status: "failed" as const, result: "run cancelled", summary: "run cancelled" }
              : tool),
          } : previous);
          notify("cancelled", "run cancelled", event.sessionId);
          reloadSession();
        }
        if (event.type === "run.failed") {
          localRunIdsRef.current.delete(event.runId);
          delete pendingParentRef.current[event.runId];
          if (liveRunIdRef.current === event.runId) liveRunIdRef.current = null;
          if (optimisticTurnRef.current?.sessionId === event.sessionId) {
            optimisticSessionIdRef.current = null;
            optimisticTurnRef.current = null;
          }
          setQueue((previous) => previous.filter((item) => item.runId !== event.runId));
          setRun((previous) => previous && previous.id === event.runId ? {
            ...previous,
            retry: undefined,
            status: "failed",
            finishedAt: event.emittedAt,
            approval: undefined,
            error: String(event.payload.error || ""),
            errorKind: event.payload.kind ? String(event.payload.kind) : undefined,
            errorHint: event.payload.hint ? String(event.payload.hint) : undefined,
            tools: previous.tools.map((tool) => tool.status === "running" || tool.status === "waiting_approval"
              ? { ...tool, status: "failed" as const, result: String(event.payload.error || "run failed"), summary: String(event.payload.error || "run failed") }
              : tool),
          } : previous);
          notify("failed", `run failed: ${String(event.payload.error || "provider error")}`, event.sessionId);
          reloadSession();
        }
        if (event.type === "session.rewound") {
          // Rewind issued from another client or tab: refetch the snapshot so
          // the active path matches the daemon.
          reloadSession();
        }
        if (event.type === "run.steered_live") {
          // A queued follow-up was promoted into this run's live steer: the
          // daemon re-anchored its message row into the chain. Drop the
          // promoted item from the local queue (the refetch's queued-run
          // filter would hide its row while it is still listed here),
          // re-parent this run's in-flight assistant row onto the steered
          // message (the daemon chains the continuation after it, so the
          // active path must pass through it), then refetch.
          const fromRun = String(event.payload.from_run || "");
          const steeredId = Number(event.payload.message_id || 0);
          if (fromRun) {
            // Clear the queue REF synchronously: the reload's merge excludes
            // rows of queued runs, and the steered row carries from_run's id.
            // setQueue only updates the ref on the next render - a stale ref
            // drops the steered row from the merge and the re-parented
            // assistant dangles (the history above it vanishes until a full
            // page refresh).
            queuedPromptsRef.current = queuedPromptsRef.current.filter((item) => item.runId !== fromRun);
            setQueue((previous) => previous.filter((item) => item.runId !== fromRun));
          }
          if (steeredId > 0) {
            setMessages((previous) => previous.map((message) =>
              message.runId === event.runId && message.role === "assistant"
                ? { ...message, parentId: steeredId }
                : message,
            ));
          }
          reloadSession();
        }
        if (event.type === "context.preparing" || event.type === "context.completed" || event.type === "context.failed" || event.type === "context.fallback"
          || event.type === "context.compaction_started" || event.type === "context.compacted"
          || event.type === "context.recompressing" || event.type === "context.recompressed") {
          const contextStatus = event.type === "context.preparing" || event.type === "context.compaction_started" || event.type === "context.recompressing"
            ? "preparing"
            : event.type === "context.failed" ? "failed" : "ready";
          setSessions((previous) => previous.map((item) => item.id === event.sessionId ? {
            ...item,
            contextMode: "summary",
            contextStatus,
            contextError: contextStatus === "failed" ? String(event.payload.error || "context preparation failed") : "",
          } : item));
          if (event.type !== "context.preparing") reloadSession();
        }
        if (event.type === "context.summary_prompt") {
          // The auxiliary summary failed; the daemon is parked on the
          // operator's decision (primary / deterministic / retry).
          setSessions((previous) => previous.map((item) => item.id === event.sessionId ? {
            ...item,
            contextPrompt: String(event.payload.prompt || "auxiliary model unavailable for compaction. Use the primary model?"),
          } : item));
          notify("approval", "compaction prompt: auxiliary model unavailable", event.sessionId);
        }
        if (event.type === "context.summary_resolved") {
          setSessions((previous) => previous.map((item) => item.id === event.sessionId ? {
            ...item,
            contextPrompt: "",
          } : item));
          reloadSession();
        }
        if (event.type === "approval.requested") {
          // The approval belongs to the run holding the session slot. Show it
          // even if a newer queued run is currently tracked. Add the pending
          // call to the activity list before tool.started arrives, otherwise a
          // multi-call response looks like a completed run waiting for nothing.
          setRun((previous) => {
            const approval = { id: String(event.payload.approval_id || event.eventId), name: String(event.payload.name || "a tool call"), arguments: String(event.payload.arguments || ""), scope: String(event.payload.scope || "once") };
            const pendingTool: HarnessTool = { id: approval.id, name: approval.name, target: "runtime", status: "waiting_approval", arguments: approval.arguments };
            if (!previous || previous.id !== event.runId) {
              return { id: event.runId, sessionId: previous?.sessionId || event.sessionId, profile: previous?.profile || "orchestrator", model: previous?.model || "", provider: previous?.provider || "openai", status: "waiting_approval", startedAt: previous?.startedAt, reasoning: "", tools: [pendingTool], metrics: seedRunMetrics(runRef.current), approval };
            }
            const tools = previous.tools.some((tool) => tool.id === approval.id)
              ? previous.tools.map((tool) => tool.id === approval.id ? { ...tool, ...pendingTool } : tool)
              : [...previous.tools, pendingTool];
            return { ...previous, status: "waiting_approval", tools, approval };
          });
          notify("approval", `approval needed: ${String(event.payload.name || "a tool call")}`, event.sessionId);
        }
        if (event.type === "approval.resolved") {
          const approvalId = String(event.payload.approval_id || "");
          setRun((previous) => {
            if (!previous) return previous;
            const tools = previous.tools.map((tool) => tool.id === approvalId && event.payload.decision !== "deny"
              ? { ...tool, status: "running" as const }
              : tool);
            return previous.approval?.id === approvalId
              ? { ...previous, approval: undefined, status: "running", tools }
              : previous;
          });
        }
        if (event.type === "input.requested") {
          // The run parks on its input gate; tool.started only arrives after
          // the answer, so add the pending tool now - otherwise run.tools
          // stays empty, activityRun is null, and the question card only
          // shows in the inspector.
          let options: string[] = [];
          try {
            const raw = JSON.parse(String(event.payload.options || "[]")) as unknown;
            if (Array.isArray(raw)) options = raw.map((item) => String(item));
          } catch {
            options = [];
          }
          const inputKind = event.payload.kind === "question"
            ? "question"
            : event.payload.kind === "sudo" ? "sudo" : "quiz";
          const input = {
            id: String(event.payload.input_id || event.eventId),
            kind: inputKind as "quiz" | "question" | "sudo",
            question: String(event.payload.question || ""),
            options,
            hasDontKnow: event.payload.has_dont_know === "true" || event.payload.has_dont_know === true,
            ...(inputKind === "sudo"
              ? { command: String(event.payload.command || "") }
              : {}),
          };
          setRun((previous) => {
            const pendingTool: HarnessTool = {
              id: input.id,
              name: input.kind === "quiz"
                ? "quiz"
                : input.kind === "sudo" ? "terminal" : "ask_user_question",
              target: "runtime",
              status: "running",
            };
            // A sudo-gated terminal call STARTED before the gate parked it
            // (the tool row carries the call's real arguments). Bind the gate
            // to that row instead of appending a duplicate keyed by the
            // input id - the later tool.completed carries the call's tool
            // id, which the input-keyed duplicate can never match, so the
            // phantom row would stay pending forever.
            if (inputKind === "sudo" && previous) {
              const commandText = input.command || "";
              const commandKey = commandText
                .replace(/[\\"'\n\r]/g, "")
                .slice(0, 40);
              const gatedRow = previous.tools.some((tool) =>
                tool.name === "terminal" && tool.arguments
                  ? tool.arguments.replace(/[\\"'\n\r]/g, "").includes(commandKey)
                  : false,
              );
              if (gatedRow) {
                return { ...previous, status: "waiting_input", input };
              }
            }
            if (!previous || previous.id !== event.runId) {
              return { id: event.runId, sessionId: previous?.sessionId || event.sessionId, profile: previous?.profile || "orchestrator", model: previous?.model || "", provider: previous?.provider || "openai", status: "waiting_input", startedAt: previous?.startedAt, reasoning: "", tools: [pendingTool], metrics: seedRunMetrics(runRef.current), input };
            }
            const tools = previous.tools.some((tool) => tool.id === input.id)
              ? previous.tools.map((tool) => tool.id === input.id ? { ...tool, ...pendingTool } : tool)
              : [...previous.tools, pendingTool];
            return { ...previous, status: "waiting_input", tools, input };
          });
          notify("approval", String(event.payload.question || "answer needed"), event.sessionId);
        }
        if (event.type === "input.resolved") {
          const inputId = String(event.payload.input_id || "");
          setRun((previous) => previous?.input?.id === inputId
            ? { ...previous, input: undefined, status: previous.status === "waiting_input" ? "running" : previous.status }
            : previous);
        }
      };
      streamStop = client.streamSession(currentSessionId, (event) => {
        if (!active) return;
        if (appliedEventsRef.current.has(event.eventId)) return;
        appliedEventsRef.current.add(event.eventId);
        // The set only exists to dedup the resumable stream's replay, which
        // starts at the cursor (recent events). Cap it so a long session does
        // not grow it unbounded; keep the most recent ids.
        if (appliedEventsRef.current.size > 4000) {
          appliedEventsRef.current = new Set([...appliedEventsRef.current].slice(-2000));
        }
        lastEventAtRef.current = Date.now();
        if (stallNotifiedRef.current) {
          stallNotifiedRef.current = false;
          setStreamStalled(false);
        }
        pendingEvents.push(event);
        if (!flushScheduled) {
          // Time-based rather than rAF: deltas arrive slower than a frame, so
          // rAF cannot coalesce them. A 200 ms window merges consecutive
          // deltas into one concat + one render and caps renders at 5/s.
          flushScheduled = true;
          window.setTimeout(() => {
            flushScheduled = false;
            if (!active) { pendingEvents = []; return; }
            const batch = pendingEvents;
            pendingEvents = [];
            for (const queued of batch) applyEvent(queued);
          }, FLUSH_MS);
        }
      }, (value) => {
        // A stale source from a previous session must not clobber this one.
        if (!active) return;
        trace(value ? "stream connected" : "stream disconnected");
        setPhase(value ? null : "event stream disconnected, retrying");
        handleConnection(value);
      });
    }).catch((error: unknown) => {
      if (!active) return;
      trace(`snapshot load failed: ${String(error)}`);
      setPhase(`snapshot load failed (${String(error)})`);
    });
    const stuckTimer = window.setTimeout(() => {
      if (active && connectionStateRef.current !== true) {
        console.warn(`[harness] still disconnected after 10s (phase: ${pendingPhaseRef.current || "unknown"})`, currentSessionId);
      }
    }, 10_000);
    return () => {
      active = false;
      window.clearTimeout(stuckTimer);
      streamStop?.();
    };
  }, [client, currentSessionId, reconnectTick, reloadSession, notify, handleConnection, setPhase, withContextWindow, promoteQueuedPrompt, seedRunMetrics, setQueue]);

  const currentSession = sessions.find((session) => session.id === currentSessionId);
  const activeRuns = run && isRunActive(run.status) ? 1 : 0;

  // "New conversation" opens a draft conversation without creating a session
  // row. The row is created on the first send, so mashing the button never
  // leaves a pile of blank threads behind.
  const createSession = useCallback(() => {
    // A draft has no daemon snapshot to load, so clear the previous session's
    // projection explicitly instead of leaving its messages/run on screen.
    sessionGenerationRef.current += 1;
    currentSessionIdRef.current = "";
    optimisticSessionIdRef.current = null;
    optimisticTurnRef.current = null;
    setCurrentSessionId("");
    setDraftProfile("orchestrator");
    setGroupDraft([]);
    setMessages([]);
    setRun(null);
    setEvents([]);
    setArtifacts([]);
    setDraft("");
    setComposerSeed(null);
    setStreamStalled(false);
    setConnected(false);
    setPhase(null);
    messagesRef.current = [];
    runRef.current = null;
    latestRunIdRef.current = null;
    localRunIdsRef.current.clear();
    startedRunIdsRef.current.clear();
    pendingParentRef.current = {};
    appliedEventsRef.current = new Set();
    setView("chat");
    setSidebarOpen(false);
  }, [setPhase]);

  const sendMessage = async (composed?: string, attachments: HarnessAttachment[] = [], mentions: string[] = [], speakers: string[] = []) => {
    const generation = sessionGenerationRef.current;
    const selectedProfile = currentSession?.profile || draftProfile;
    const base = composed ?? draft;
    if (!base.trim() && attachments.length === 0) return;
    // A live session needs a live event stream; a draft conversation has no
    // stream yet - the first send is what creates the session row.
    if (currentSessionId && !connected) return;
    const content = base.trim();
    const parentId = getActiveLeaf(messagesRef.current);
    let sessionId = currentSessionId;
    if (!sessionId) {
      try {
        // Group chat: 2+ picked speakers open a group session (one shared
        // transcript, ordered round-robin turns); a 0/1-speaker send is an
        // ordinary session.
        const groupSpeakers = speakers.length >= 2 ? speakers : undefined;
        const session = await client.createSession(selectedProfile, groupSpeakers);
        if (generation !== sessionGenerationRef.current || currentSessionIdRef.current !== "") return;
        setGroupDraft([]);
        setSessions((previous) => [session, ...previous]);
        optimisticSessionIdRef.current = session.id;
        sessionId = session.id;
        currentSessionIdRef.current = session.id;
        setCurrentSessionId(session.id);
      } catch (error) {
        if (generation !== sessionGenerationRef.current) return;
        setMessages((previous) => [...previous, { id: tempMessageId(`error-${Date.now()}`), parentId, role: "system", content: error instanceof Error ? error.message : "could not create the session", createdAt: new Date().toISOString(), isActive: true }]);
        return;
      }
    }
    const provisionalRunId = `pending-${Date.now()}`;
    const userTempId = tempMessageId(`user-${Date.now()}`);
    const userMessage: HarnessMessage = {
      id: userTempId,
      parentId,
      role: "user",
      content,
      createdAt: new Date().toISOString(),
      runId: provisionalRunId,
      isActive: true,
      attachments: attachments.length ? attachments : undefined,
      // A-7: the temp id doubles as the dedupe anchor the daemon stores on
      // the persisted row (client_id).
      clientId: userTempId,
    };
    if (optimisticSessionIdRef.current === sessionId) {
      optimisticTurnRef.current = { sessionId, user: userMessage };
    }
    const shouldQueue = localRunIdsRef.current.size > 0 || (!!runRef.current && isRunActive(runRef.current.status));
    // The slot-holder owes a decision (approval / question): switching the
    // tracked run to the queued prompt would hide its pending gate card from
    // the chat and leave the queue stuck behind an undecidable gate. Keep
    // tracking the slot-holder until run.started flips to the new run.
    const gateOwed = shouldQueue && (runRef.current?.status === "waiting_approval" || runRef.current?.status === "waiting_input");
    localRunIdsRef.current.add(provisionalRunId);
    pendingParentRef.current[provisionalRunId] = userTempId;
    setDraft("");
    if (shouldQueue) {
      setQueue((previous) => [...previous, { id: provisionalRunId, sessionId, runId: provisionalRunId, content, attachments }]);
    } else {
      setMessages((previous) => [...previous, userMessage]);
    }
    if (!gateOwed) {
      setRun({ id: provisionalRunId, sessionId, profile: selectedProfile, model: currentSession?.model || "", provider: "openai", status: "queued", reasoning: "", tools: [], metrics: seedRunMetrics(runRef.current) });
    }
    try {
      const result = await client.sendMessage({ sessionId, message: content, profile: selectedProfile, attachments, mentions, messageId: userTempId });
      if (generation !== sessionGenerationRef.current || currentSessionIdRef.current !== sessionId) {
        localRunIdsRef.current.delete(provisionalRunId);
        return;
      }
      // A-7: the daemon anchored the stored row to the client id and
      // assigned it its own (snowflake) id; the optimistic row renames to it
      // (children re-pointed). Until the response lands, clientId keeps the
      // temp row mergeable against the persisted one.
      const daemonUserId = result.messageId ?? userTempId;
      pendingParentRef.current[result.runId] = daemonUserId;
      latestRunIdRef.current = result.runId;
      localRunIdsRef.current.delete(provisionalRunId);
      localRunIdsRef.current.add(result.runId);
      // Rename the optimistic user row to the daemon's run id: group
      // transcripts sort by the run's ledger events, and the provisional id
      // matches no run.accepted, so the row would pin to the top instead of
      // landing after the prior turns (the "user message missing until a
      // refresh" symptom). The optimistic-turn record renames with it, so a
      // mid-run snapshot merge keys the temp row against the persisted row.
      setMessages((previous) => previous.map((message) =>
        message.id === userTempId
          ? { ...message, id: daemonUserId, runId: result.runId }
          : message.parentId === userTempId
            ? { ...message, parentId: daemonUserId }
            : message
      ));
      if (optimisticTurnRef.current?.sessionId === sessionId &&
          optimisticTurnRef.current.user.id === userTempId) {
        optimisticTurnRef.current = {
          sessionId,
          user: { ...optimisticTurnRef.current.user, id: daemonUserId, runId: result.runId },
        };
      }
      if (shouldQueue) {
        setQueue((previous) => previous.map((item) => item.runId === provisionalRunId ? { ...item, runId: result.runId } : item));
        if (startedRunIdsRef.current.has(result.runId)) promoteQueuedPrompt(result.runId);
      }
      // Only the provisional run is renamed to the daemon's id; a tracked
      // slot-holder (gate owed) keeps its own run until it terminates.
      setRun((previous) => previous && previous.id === provisionalRunId ? { ...previous, id: result.runId } : previous);
    } catch (error) {
      localRunIdsRef.current.delete(provisionalRunId);
      if (generation !== sessionGenerationRef.current || currentSessionIdRef.current !== sessionId) return;
      setRun((previous) => previous && previous.id === provisionalRunId ? { ...previous, status: "failed" } : previous);
      setMessages((previous) => [...previous, { id: tempMessageId(`error-${Date.now()}`), parentId, role: "system", content: error instanceof Error ? error.message : "run failed", createdAt: new Date().toISOString(), isActive: true }]);
    }
  };

  // A-6: an interrupted run restores its queued prompts to the composer as
  // one reviewable block. Each queued run is withdrawn first (the daemon
  // would otherwise start it the moment the slot frees), so nothing keeps
  // running that the user just stopped. A FAILED run does not restore: the
  // queue is allowed to drain after a failure (the retry bar is the
  // operator's recovery point).
  const restoreQueuedToComposer = useCallback(async (sessionId: string) => {
    const items = queuedPromptsRef.current.filter((item) => item.sessionId === sessionId);
    if (items.length === 0) return;
    const blocks: string[] = [];
    const attachments: HarnessAttachment[] = [];
    for (const item of items) {
      blocks.push(item.content);
      for (const attachment of item.attachments) attachments.push({ ...attachment });
      try {
        await client.cancelRun(item.runId);
      } catch {
        // The run may have started already: its message stays in the
        // transcript, and the text is restored below regardless.
      }
    }
    setQueue((previous) => previous.filter((item) => item.sessionId !== sessionId));
    setSteeringQueueIds((previous) => {
      if (previous.size === 0) return previous;
      const next = new Set(previous);
      for (const item of items) {
        next.delete(item.id);
        delete steeringConfirmedRef.current[item.id];
      }
      return next;
    });
    setDraft((previous) => previous.trim()
      ? `${previous}\n\n${blocks.join("\n\n")}`
      : blocks.join("\n\n"));
    if (attachments.length > 0) {
      setComposerSeed({ token: Date.now(), sessionId, attachments });
    }
    flashSteer("restore");
  }, [client, setQueue, flashSteer]);

  const stopRun = useCallback(async () => {
    const current = runRef.current;
    if (!current || current.id.startsWith("pending-")) return;
    const sessionId = current.sessionId;
    await client.cancelRun(current.id);
    setRun((previous) => previous ? { ...previous, status: "cancelled" } : previous);
    await restoreQueuedToComposer(sessionId);
  }, [client, restoreQueuedToComposer]);

  // Esc interrupts the tracked run (same command as the composer's stop
  // button, including the A-6 queue restore). Field editors and the search
  // palette consume Esc first (defaultPrevented / a text-field target); the
  // attachment viewer registers its own window listener after this one, so
  // the backdrop check keeps a dialog close from also cancelling the run.
  useEffect(() => {
    const onKeyDown = (event: globalThis.KeyboardEvent) => {
      if (event.key !== "Escape" || event.isComposing || event.defaultPrevented) return;
      const target = event.target as HTMLElement | null;
      if (target && target.closest("input, textarea, select, [contenteditable='true']")) return;
      if (document.querySelector(".overlay-backdrop")) return;
      const current = runRef.current;
      if (!current || !isRunActive(current.status) || current.id.startsWith("pending-")) return;
      event.preventDefault();
      void stopRun();
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [stopRun]);

  const regenerateMessage = async (runId: string) => {
    if (!currentSessionId) return;
    const target = messagesRef.current.find((message) => message.runId === runId);
    // A failed run has no assistant message yet: its target is the user message
    // itself, and the daemon attaches the retry to that message id.
    const parentId = target ? (target.role === "user" ? target.id : target.parentId) : getActiveLeaf(messagesRef.current);
    try {
      const result = await client.regenerate({ sessionId: currentSessionId, runId });
      localRunIdsRef.current.add(result.runId);
      pendingParentRef.current[result.runId] = parentId;
      latestRunIdRef.current = result.runId;
      setMessages((previous) => [
        ...previous.map((message) =>
          message.role === "assistant" && message.parentId === parentId
            ? { ...message, isActive: false }
            : message,
        ),
        { id: tempMessageId(`regen-${Date.now()}`), parentId, role: "assistant", content: "", createdAt: new Date().toISOString(), runId: result.runId, isActive: true },
      ]);
      setRun({ id: result.runId, sessionId: currentSessionId, profile: currentSession?.profile || "orchestrator", model: currentSession?.model || "", provider: "openai", status: "queued", reasoning: "", tools: [], metrics: seedRunMetrics(runRef.current) });
    } catch (error) {
      setMessages((previous) => [...previous, { id: tempMessageId(`error-${Date.now()}`), parentId, role: "system", content: error instanceof Error ? error.message : "regenerate failed", createdAt: new Date().toISOString(), isActive: true }]);
    }
  };

  const selectIteration = async (runId: string) => {
    if (!currentSessionId) return;
    try {
      await client.selectIteration({ sessionId: currentSessionId, runId });
      reloadSession();
    } catch {
      // Selection sync is best-effort; the local view already updated.
    }
  };

  const rewindTo = async (messageId: number, trim = false) => {
    if (!currentSessionId) return;
    try {
      await client.rewind({ sessionId: currentSessionId, messageId, trim });
      reloadSession();
    } catch (error) {
      notify("failed", `rewind failed: ${error instanceof Error ? error.message : String(error)}`, currentSessionId, true);
    }
  };

  // Delete is destructive: the message and everything after it in that branch
  // are removed permanently; sibling branches under its parent remain.
  const deleteMessage = async (message: HarnessMessage) => {
    if (!currentSessionId) return;
    try {
      await client.deleteMessage({ sessionId: currentSessionId, messageId: message.id });
      reloadSession();
    } catch (error) {
      notify("failed", `delete failed: ${error instanceof Error ? error.message : String(error)}`, currentSessionId, true);
    }
  };

  // Rewind here is intentionally destructive: it removes every descendant
  // message and branch instead of merely changing the active projection.
  const rewindHere = (message: HarnessMessage) => void rewindTo(message.id, true);

  const selectMessageBranch = async (messageId: number) => {
    if (!currentSessionId) return;
    try {
      await client.selectMessage({ sessionId: currentSessionId, messageId });
      reloadSession();
    } catch {
      // Selection sync is best-effort; the local view already updated.
    }
  };

  // Stable identity: the palette's debounce effect depends on it, and App
  // re-renders on every event-stream flush.
  const searchAll = useCallback((query: string) => client.searchAll(query), [client]);

  // ⌘K search jump: select the hit's session and hand the message id to
  // ChatView's pendingJump effect, which widens the tail window and scrolls.
  const openSearchMatch = (match: GlobalSearchMatch) => {
    setSearchOpen(false);
    if (match.sessionId !== currentSessionId) {
      setCurrentSessionId(match.sessionId);
    }
    setView("chat");
    setPendingJump(match.messageId);
  };

  // A jump target sitting on a dead branch of the loaded session is not on
  // the active path ChatView's scroll effect walks, so select its branch
  // first; the reload re-activates the message and the pendingJump effect
  // re-runs against the rebuilt path. Also covers cross-session jumps, where
  // the message list is still loading when the click lands.
  useEffect(() => {
    if (pendingJump === null || !currentSessionId) return;
    const message = messages.find((item) => item.id === pendingJump);
    if (message && !message.isActive) {
      void client.selectMessage({ sessionId: currentSessionId, messageId: pendingJump });
    }
  }, [pendingJump, currentSessionId, messages, client]);

  // Edit = a branch on the user side, mirroring regenerate on the model side:
  // the daemon creates a sibling user message with the new text, makes it the
  // active leaf, and starts a run against it. The original message and its
  // tail become an inactive branch.
  const editMessage = async (message: HarnessMessage, content: string) => {
    if (!currentSessionId) return;
    const tempUserId = tempMessageId(`edit-user-${Date.now()}`);
    try {
      const result = await client.editMessage({ sessionId: currentSessionId, messageId: message.id, content });
      localRunIdsRef.current.add(result.runId);
      pendingParentRef.current[result.runId] = tempUserId;
      latestRunIdRef.current = result.runId;
      // Mirror the branch locally: the edited message and everything after it
      // on the active path become inactive.
      const path = getActivePath(messagesRef.current);
      const index = path.findIndex((m) => m.id === message.id);
      const branchIds = new Set<number>(index >= 0 ? path.slice(index).map((m) => m.id) : [message.id]);
      setMessages((previous) => [
        ...previous.map((m) => (branchIds.has(m.id) ? { ...m, isActive: false } : m)),
        { id: tempUserId, parentId: message.parentId, role: "user", content, createdAt: new Date().toISOString(), runId: result.runId, isActive: true, attachments: message.attachments },
        { id: tempMessageId(`edit-assistant-${Date.now()}`), parentId: tempUserId, role: "assistant", content: "", createdAt: new Date().toISOString(), runId: result.runId, isActive: true },
      ]);
      setRun({ id: result.runId, sessionId: currentSessionId, profile: currentSession?.profile || "orchestrator", model: currentSession?.model || "", provider: "openai", status: "queued", reasoning: "", tools: [], metrics: seedRunMetrics(runRef.current) });
    } catch (error) {
      notify("failed", `edit failed: ${error instanceof Error ? error.message : String(error)}`, currentSessionId, true);
    }
  };

  const forkConversation = async (mode: ForkMode) => {
    if (!currentSessionId) return;
    const path = getActivePath(messagesRef.current).map((message) => message.id);
    if (!path.length || path.some((id) => id <= 0)) return;
    try {
      const session = await client.forkSession({ sourceId: currentSessionId, path, mode });
      setSessions((previous) => [session, ...previous.filter((item) => item.id !== session.id)]);
      setCurrentSessionId(session.id);
      setView("chat");
      setSidebarOpen(false);
    } catch (error) {
      notify("failed", `fork failed: ${error instanceof Error ? error.message : String(error)}`, currentSessionId, true);
    }
  };

  const selectProfile = async (profileId: string) => {
    if (!currentSessionId) {
      setDraftProfile(profileId);
      return;
    }
    if (profileId === currentSession?.profile) return;
    const previousProfile = currentSession?.profile || "orchestrator";
    setSessions((previous) => previous.map((session) => session.id === currentSessionId ? { ...session, profile: profileId } : session));
    try {
      await client.selectProfile(currentSessionId, profileId);
      // A profile swap can change the model route and therefore the context
      // window; re-discover it so the meter reflects the new profile.
      void refreshHealth();
    } catch (error) {
      // Keep the browser projection truthful when the daemon rejects a switch,
      // for example because the active path contains images.
      setSessions((previous) => previous.map((session) => session.id === currentSessionId ? { ...session, profile: previousProfile } : session));
      notify("failed", `profile switch failed: ${error instanceof Error ? error.message : String(error)}`, currentSessionId, true);
    }
  };

  const refreshProfiles = useCallback(() => {
    return client.listProfiles().then((items) => {
      if (items.length) setProfiles(items);
    });
  }, [client]);

  const saveProfile = useCallback(async (draft: ProfileDraft, mode: "create" | "edit", originalId?: string) => {
    const input: ProfileInput = {
      id: draft.id.trim(),
      name: draft.name.trim(),
      description: draft.description.trim(),
      identityPrompt: draft.identityPrompt,
      baseModel: draft.baseModel.trim(),
      provider: draft.provider?.trim() || "",
      vision: draft.vision === "on" ? true : draft.vision === "off" ? false : null,
      childProfileAllowlist: draft.childProfileAllowlist.split(",").map((id) => id.trim()).filter(Boolean),
    };
    const temperature = Number(draft.temperature);
    if (draft.temperature.trim() !== "" && Number.isFinite(temperature)) {
      input.temperature = temperature;
    }
    if (mode === "create") {
      await client.createProfile(input);
    } else {
      // A slug rename is a reference change: rename first so the store and
      // every session agree, then update the definition under the new id.
      if (originalId && originalId !== input.id) {
        await client.renameProfile(originalId, input.id);
      }
      await client.updateProfile(input);
    }
    await refreshProfiles();
  }, [client, refreshProfiles]);

  const removeProfile = useCallback(async (id: string) => {
    await client.deleteProfile(id);
    await refreshProfiles();
  }, [client, refreshProfiles]);

  // Bind a profile to a prompt pack and save its ordered module stack.
  // The other definition fields are preserved from the live profile.
  const saveProfilePlan = useCallback(async (profileId: string, packId: string, plan: string[]) => {
    const profile = (await client.listProfiles()).find((item) => item.id === profileId);
    if (!profile) throw new Error(`profile '${profileId}' not found`);
    const input: ProfileInput = {
      id: profile.id,
      name: profile.name,
      description: profile.description || "",
      identityPrompt: profile.identityPrompt || "",
      baseModel: profile.baseModel || "",
      provider: profile.provider || "",
      vision: profile.vision,
      promptPackId: packId,
      promptPlan: plan,
    };
    if (profile.temperature !== undefined) input.temperature = profile.temperature;
    await client.updateProfile(input);
    await refreshProfiles();
  }, [client, refreshProfiles]);

  const refreshProviders = useCallback(() => {
    return client.listProviders().then((items) => {
      setProviders(items);
    });
  }, [client]);

  useEffect(() => {
    void refreshProviders();
  }, [refreshProviders]);

  const saveProvider = useCallback(async (draft: ProviderDraft) => {
    await client.upsertProvider({
      name: draft.name.trim(),
      endpoint: draft.endpoint.trim(),
      model: draft.model.trim(),
      ...(draft.apiKey.trim() ? { apiKey: draft.apiKey.trim() } : {}),
      params: draftToParams(draft),
    });
    await refreshProviders();
  }, [client, refreshProviders]);

  const removeProvider = useCallback(async (name: string) => {
    await client.deleteProvider(name);
    await refreshProviders();
  }, [client, refreshProviders]);

  const removeSession = useCallback(async (id: string) => {
    try {
      await client.deleteSession(id);
    } catch (error) {
      notify("failed", `delete failed: ${error instanceof Error ? error.message : String(error)}`, id, true);
      return;
    }
    const remaining = sessions.filter((item) => item.id !== id);
    setSessions(remaining);
    if (currentSessionId === id) {
      // Land in the clean chat draft (/#/chat) instead of auto-opening
      // another conversation - the operator explicitly closed this one.
      // Clear the deleted session's projection the way a new draft does:
      // the chat view reads local state, not the session row, so without
      // this the deleted conversation keeps rendering on screen.
      sessionGenerationRef.current += 1;
      currentSessionIdRef.current = "";
      optimisticSessionIdRef.current = null;
      optimisticTurnRef.current = null;
      setCurrentSessionId("");
      setDraftProfile("orchestrator");
      setMessages([]);
      setRun(null);
      setEvents([]);
      setArtifacts([]);
      setDraft("");
      setComposerSeed(null);
      setStreamStalled(false);
      setConnected(false);
      setPhase(null);
      messagesRef.current = [];
      runRef.current = null;
      latestRunIdRef.current = null;
      localRunIdsRef.current.clear();
      startedRunIdsRef.current.clear();
      pendingParentRef.current = {};
      appliedEventsRef.current = new Set();
      setView("chat");
      setSidebarOpen(false);
    }
  }, [client, sessions, currentSessionId, notify, setPhase]);

  const renameSession = useCallback(async (id: string, title: string) => {
    try {
      await client.renameSession(id, title);
      setSessions((previous) => previous.map((item) => item.id === id ? { ...item, title } : item));
    } catch (error) {
      notify("failed", `rename failed: ${error instanceof Error ? error.message : String(error)}`, id, true);
    }
  }, [client, notify]);

  // Jump the main view to a worker session (from the inline subagent block).
  // Workers are hidden from the sidebar, so this is the only path to them.
  const openWorkerSession = useCallback((id: string) => {
    setCurrentSessionId(id);
    setView("chat");
    setSidebarOpen(false);
  }, []);

  const setSessionTags = useCallback(async (id: string, tags: string[]) => {
    try {
      await client.setSessionTags(id, tags);
      setSessions((previous) => previous.map((item) => item.id === id ? { ...item, tags } : item));
    } catch (error) {
      notify("failed", `tags failed: ${error instanceof Error ? error.message : String(error)}`, id, true);
    }
  }, [client, notify]);

  const retitleSession = useCallback(async (id: string) => {
    try {
      await client.retitleSession(id);
      notify("completed", "retitle armed: the model sets a fresh title on the next run", id);
    } catch (error) {
      notify("failed", `retitle failed: ${error instanceof Error ? error.message : String(error)}`, id, true);
    }
  }, [client, notify]);

  // Stable identities: the inspector's journey load effect keys on them.
  const getJourney = useCallback((sessionId: string) => client.getJourney(sessionId), [client]);
  const requestJourneyUpdate = useCallback((sessionId: string) => client.requestJourneyUpdate(sessionId), [client]);

  const linkVault = useCallback(async (id: string, path: string) => {
    try {
      await client.linkVault(id, path);
      setSessions((previous) => previous.map((item) => item.id === id ? { ...item, mdLogPath: path } : item));
    } catch (error) {
      notify("failed", `vault link failed: ${error instanceof Error ? error.message : String(error)}`, id, true);
    }
  }, [client, notify]);

  const unlinkVault = useCallback(async (id: string) => {
    try {
      await client.unlinkVault(id);
      setSessions((previous) => previous.map((item) => item.id === id ? { ...item, mdLogPath: "" } : item));
    } catch (error) {
      notify("failed", `vault unlink failed: ${error instanceof Error ? error.message : String(error)}`, id, true);
    }
  }, [client, notify]);

  // The pending-actions center resolves gates by run id, so it can answer
  // items from any session the ledger carries (not just the tracked run).
  const respondApprovalFor = async (runId: string, approvalId: string, decision: "allow_once" | "allow_turn" | "allow_session" | "allow_always" | "deny") => {
    const key = `${runId}\u0000${approvalId}`;
    if (approvalRequestRef.current === key) return;
    approvalRequestRef.current = key;
    try {
      await client.respondApproval(runId, approvalId, decision);
    } catch (error) {
      notify("failed", `approval failed: ${error instanceof Error ? error.message : String(error)}`, currentSessionId, true);
    } finally {
      if (approvalRequestRef.current === key) approvalRequestRef.current = null;
    }
  };

  const answerInputFor = async (runId: string, inputId: string, choice: number) => {
    const key = `${runId}\u0000${inputId}`;
    if (approvalRequestRef.current === key) return;
    approvalRequestRef.current = key;
    try {
      await client.answerInput(runId, inputId, choice);
    } catch (error) {
      notify("failed", `answer failed: ${error instanceof Error ? error.message : String(error)}`, currentSessionId, true);
    } finally {
      if (approvalRequestRef.current === key) approvalRequestRef.current = null;
    }
  };

  // Conversation export: fresh snapshot (the full ledger, not the windowed
  // client state) serialized client-side, then downloaded.
  const exportConversation = useCallback(async (format: "json" | "markdown" | "csv") => {
    if (!currentSessionId) return;
    const snapshot = await client.loadSession(currentSessionId);
    const session = snapshot.session;
    if (format === "json") {
      downloadText(sessionFileName(session, "json"), JSON.stringify(snapshot, null, 2), "application/json");
    } else if (format === "markdown") {
      downloadText(sessionFileName(session, "md"), exportMarkdown(snapshot), "text/markdown");
    } else {
      downloadText(sessionFileName(session, "csv"), exportEventsCsv(snapshot.events), "text/csv");
    }
    notify("restored", `conversation exported (${format})`, currentSessionId);
  }, [client, currentSessionId, notify]);

  const respondApproval = async (decision: "allow_once" | "allow_turn" | "allow_session" | "allow_always" | "deny") => {
    const approval = run?.approval;
    if (!approval) return;
    const runId = run.id;
    const approvalId = approval.id;
    const key = `${runId}\u0000${approvalId}`;
    if (approvalRequestRef.current === key) return;
    approvalRequestRef.current = key;
    try {
      await client.respondApproval(runId, approvalId, decision);
      // A denial is returned to the model as a tool result; the daemon keeps
      // the run alive so it can answer or choose another path. Only terminal
      // events decide that a run failed. Do not clear a newer approval.
      setRun((previous) => previous && previous.id === runId && previous.approval?.id === approvalId
        ? { ...previous, approval: undefined, status: "running" }
        : previous);
    } catch (error) {
      notify("failed", `approval failed: ${error instanceof Error ? error.message : String(error)}`, currentSessionId, true);
    } finally {
      if (approvalRequestRef.current === key) approvalRequestRef.current = null;
    }
  };

  // Operator-triggered compaction ("compress now"): the daemon folds the
  // oldest turns into the checkpoint (selected mode) or re-compresses the
  // active tail (summary mode). Idle sessions only (409 while a run runs).
  const compactContext = async () => {
    if (!currentSessionId) return;
    const key = `compact\u0000${currentSessionId}`;
    if (approvalRequestRef.current === key) return;
    approvalRequestRef.current = key;
    try {
      const result = await client.compactContext(currentSessionId);
      const label = result.status === "compacted"
        ? "context compressed into the checkpoint"
        : result.status === "recompressing"
          ? "re-compressing the active tail"
          : result.status === "reused"
            ? "checkpoint is current, nothing to compress"
            : `compress skipped: ${result.detail || "unknown"}`;
      notify("completed", label, currentSessionId);
      reloadSession();
    } catch (error) {
      notify("failed", `compress failed: ${error instanceof Error ? error.message : String(error)}`, currentSessionId, true);
    } finally {
      if (approvalRequestRef.current === key) approvalRequestRef.current = null;
    }
  };

  // Resolve the pending compaction fallback prompt (the auxiliary summary
  // failed): the daemon wakes its parked summary job and proceeds with the
  // chosen channel. Do not clear a newer prompt.
  const respondContextDecision = async (decision: "primary" | "deterministic" | "retry") => {
    if (!currentSession?.contextPrompt) return;
    const sessionId = currentSessionId;
    const key = `context\u0000${sessionId}`;
    if (approvalRequestRef.current === key) return;
    approvalRequestRef.current = key;
    try {
      await client.respondContextDecision(sessionId, decision);
      setSessions((previous) => previous.map((item) => item.id === sessionId ? { ...item, contextPrompt: "" } : item));
    } catch (error) {
      notify("failed", `compaction prompt failed: ${error instanceof Error ? error.message : String(error)}`, sessionId, true);
    } finally {
      if (approvalRequestRef.current === key) approvalRequestRef.current = null;
    }
  };

  // Answer a pending interactive tool (quiz / ask_user_question). The
  // daemon resolves the run's input gate and the graded answer flows back
  // as the tool result. Do not clear a newer input.
  const answerInput = async (choice: number) => {
    const input = run?.input;
    if (!input) return;
    const runId = run.id;
    const inputId = input.id;
    const key = `${runId}\u0000${inputId}`;
    if (approvalRequestRef.current === key) return;
    approvalRequestRef.current = key;
    try {
      await client.answerInput(runId, inputId, choice);
      setRun((previous) => previous && previous.id === runId && previous.input?.id === inputId
        ? { ...previous, input: undefined, status: "running" }
        : previous);
    } catch (error) {
      notify("failed", `answer failed: ${error instanceof Error ? error.message : String(error)}`, currentSessionId, true);
    } finally {
      if (approvalRequestRef.current === key) approvalRequestRef.current = null;
    }
  };

  // Answer the sudo password gate (docs/sudo-gate.md): the password is sent
  // over the authorized command channel and never stored in app state - the
  // field clears with the component. The run.input card clears on the daemon's
  // input.resolved event; the local clear matches the quiz flow.
  const answerSudo = async (password: string, scope: string) => {
    const input = run?.input;
    if (!input) return;
    const runId = run.id;
    const inputId = input.id;
    const key = `${runId}\u0000${inputId}`;
    if (approvalRequestRef.current === key) return;
    approvalRequestRef.current = key;
    try {
      await client.answerSudo(runId, inputId, password, scope);
      setRun((previous) => previous && previous.id === runId && previous.input?.id === inputId
        ? { ...previous, input: undefined, status: "running" }
        : previous);
    } catch (error) {
      notify("failed", `sudo answer failed: ${error instanceof Error ? error.message : String(error)}`, currentSessionId, true);
    } finally {
      if (approvalRequestRef.current === key) approvalRequestRef.current = null;
    }
  };

  // The actions center's data: pending decisions + provenance trail, derived
  // from the session's event ledger (deduped by id, newest provenance first).
  const appLedgerSig = useMemo(() => ledgerSignature(events), [events]);
  const actions = useStableDerived(appLedgerSig, () => pendingActionsFromEvents(events));
  // Session graph data: lane layout of the current session's message tree plus
  // the per-run map for the details panel (same derivation the chat uses).
  // Only the graph view reads them - skip the O(tree) walks on chat renders.
  const graphRows = useMemo(() => (view === "graph" ? computeGraphRows(messages) : []), [view, messages]);
  const graphRuns = useMemo(
    () => (view === "graph" ? runsFromEvents(events, currentSessionId) : new Map<string, HarnessRun>()),
    [view, events, currentSessionId],
  );
  // Chat callbacks by reference: TurnBlock/ToolRow are memoized, so inline
  // wrappers in the JSX below would defeat them on every render.
  const chatOnApproval = useEventCallback(async (decision: "allow_once" | "allow_turn" | "allow_session" | "allow_always" | "deny") => void respondApproval(decision));
  const chatOnAnswerInput = useEventCallback(async (choice: number) => void answerInput(choice));
  const chatOnAnswerSudo = useEventCallback(async (password: string, scope: string) => void answerSudo(password, scope));
  const chatOnSetGroupRounds = useEventCallback(async (rounds: number | null) => {
    if (!currentSessionId) return;
    try {
      await client.setSessionGroupRounds(currentSessionId, rounds);
      setSessions((previous) => previous.map((item) => item.id === currentSessionId
        ? { ...item, groupRoundsOverride: rounds === null ? undefined : rounds }
        : item));
    } catch (error) {
      notify("failed", `could not set the rounds: ${error instanceof Error ? error.message : String(error)}`, currentSessionId, true);
    }
  });
  const chatOnRegenerate = useEventCallback(async (runId: string) => void regenerateMessage(runId));
  // Retry-that-continues (docs/stream-rules): the ErrorBar's retry resumes
  // the interrupted turn from its persisted partial answer instead of
  // regenerating (which would re-run the tool batch and orphan the partial
  // as a dimmed sibling). The daemon falls back to regenerate when nothing
  // was persisted, so one button covers both shapes.
  const continueFailedRun = async (runId: string) => {
    if (!currentSessionId) return;
    const target = messagesRef.current.find((message) => message.runId === runId);
    // The continuation chains after the failed turn's own row, whatever role it
    // is: a user row that failed before any reply, or the assistant partial.
    // Parenting a user-failed turn to the user's PARENT would fork the user row
    // off the active path and make it vanish until the next re-render.
    const parentId = target ? target.id : getActiveLeaf(messagesRef.current);
    try {
      const result = await client.continueRun({ sessionId: currentSessionId, runId });
      localRunIdsRef.current.add(result.runId);
      pendingParentRef.current[result.runId] = parentId;
      latestRunIdRef.current = result.runId;
      setMessages((previous) => [
        ...previous,
        { id: tempMessageId(`continue-${Date.now()}`), parentId, role: "assistant", content: "", createdAt: new Date().toISOString(), runId: result.runId, isActive: true },
      ]);
      setRun({ id: result.runId, sessionId: currentSessionId, profile: currentSession?.profile || "orchestrator", model: currentSession?.model || "", provider: "openai", status: "queued", reasoning: "", tools: [], metrics: seedRunMetrics(runRef.current) });
    } catch (error) {
      setMessages((previous) => [...previous, { id: tempMessageId(`error-${Date.now()}`), parentId, role: "system", content: error instanceof Error ? error.message : "continue failed", createdAt: new Date().toISOString(), isActive: true }]);
    }
  };
  const chatOnContinueFailedRun = useEventCallback(async (runId: string) => void continueFailedRun(runId));
  const chatOnSelectIteration = useEventCallback(async (runId: string) => void selectIteration(runId));
  const chatOnSelectMessage = useEventCallback(async (messageId: number) => void selectMessageBranch(messageId));
  const chatOnEdit = useEventCallback(async (message: HarnessMessage, content: string) => void editMessage(message, content));
  const chatOnDelete = useEventCallback(async (message: HarnessMessage) => deleteMessage(message));
  const chatOnRewindHere = useEventCallback(async (message: HarnessMessage) => rewindHere(message));

  return (
    <div className="app-shell">
      <div className="ambient-grid" />
      <div className="app-layout">
        <Sidebar view={view} onView={(next) => { setView(next); setSidebarOpen(false); }} onHome={() => { setView("home"); setSidebarOpen(false); }} sessions={sessions} currentSessionId={currentSessionId} onSession={(id) => { setCurrentSessionId(id); setView("chat"); setSidebarOpen(false); }} onNew={() => createSession()} open={sidebarOpen} onClose={() => setSidebarOpen(false)} session={currentSession} activeRuns={activeRuns} onDelete={(id) => void removeSession(id)} onRename={(id, title) => void renameSession(id, title)} onRetitle={(id) => void retitleSession(id)} />
        <main className="main-column">
          <Header session={currentSession} sessions={sessions} view={view} connected={connected} connectionDetail={pendingPhase} connectionTarget={client.connectionTarget} lastEventType={events.length ? events[events.length - 1].type : null} theme={theme} onTheme={() => setTheme((previous) => previous === "aero" ? "night" : "aero")} onMenu={() => setSidebarOpen(true)} inspectorOpen={inspectorOpen} onInspector={() => setInspectorOpen((previous) => !previous)} onGraph={() => setView("graph")} notifications={notifications} bellOpen={bellOpen} onBell={() => { setBellOpen((previous) => !previous); setNotifications((previous) => previous.map((notification) => notification.read ? notification : { ...notification, read: true })); }} onPick={(sessionId) => { setCurrentSessionId(sessionId); setView("chat"); setBellOpen(false); }} onClear={() => setNotifications((previous) => previous.map((notification) => ({ ...notification, read: true })))} pendingActions={actions.pending} provenance={actions.provenance} actionsOpen={actionsOpen} onActions={() => setActionsOpen((previous) => !previous)} searchOpen={searchOpen} onSearch={() => setSearchOpen((previous) => !previous)} onApproveAction={(runId, approvalId, decision) => void respondApprovalFor(runId, approvalId, decision)} onAnswerAction={(runId, inputId, choice) => void answerInputFor(runId, inputId, choice)} />
          <SearchPalette open={searchOpen} onSearch={searchAll} onPick={openSearchMatch} onClose={() => setSearchOpen(false)} />
          {pendingPhase && <div className="loading-bar" role="status" aria-label={pendingPhase} />}
          <div className="main-content">
            {view === "home" && <HomeView sessions={sessions} run={run} profilesCount={profiles.length} onNew={() => void createSession()} onSession={(id) => { setCurrentSessionId(id); setView("chat"); }} onView={(next) => setView(next)} />}
            {view === "chat" && <div className="view-form"><ChatErrorBoundary><ChatView session={currentSession} messages={messages} run={run} events={events} client={client} onOpenSession={openWorkerSession} artifacts={artifacts} status={sessionStatus} draft={draft} onDraft={setDraft} onSend={(content, attachments, mentions, speakers) => void sendMessage(content, attachments, mentions, speakers)} onStop={() => void stopRun()} groupDraft={groupDraft} onGroupSpeakers={setGroupDraft} onApproval={chatOnApproval} onAnswerInput={chatOnAnswerInput} onAnswerSudo={chatOnAnswerSudo} onSetGroupRounds={chatOnSetGroupRounds} onContextDecision={(decision) => void respondContextDecision(decision)} onRegenerate={chatOnRegenerate} onContinueFailedRun={chatOnContinueFailedRun} onSelectIteration={chatOnSelectIteration} onSelectProfile={(id) => void selectProfile(id)} onSearch={(query) => client.searchSession(currentSessionId, query)} profiles={profiles} providers={providers} defaultProvider={defaultProvider} draftProfile={draftProfile} connected={connected} disconnectedDetail={pendingPhase} stalled={streamStalled} inspectorOpen={inspectorOpen} dismissedRetryRun={dismissedRetryRun} dismissFailedRun={dismissFailedRun} queuedPrompts={queuedPrompts.filter((item) => item.sessionId === currentSessionId)} onQueueEdit={(item) => void editQueuedPrompt(item)} onQueueRemove={(item) => void removeQueuedPrompt(item)} onQueueSteer={(item) => void steerQueuedPrompt(item)} onQueueDeliverNow={(item) => void deliverQueuedPromptNow(item)} onQueueSendNow={() => void sendNowQueuedPrompt()} onCompactContext={() => void compactContext()} steeringQueueIds={steeringQueueIds} steerFlash={steerFlash} composerSeed={composerSeed} onListSnippets={() => client.listPromptSnippets()} onEdit={chatOnEdit} onDelete={chatOnDelete} onRewindHere={chatOnRewindHere} onFork={(mode) => void forkConversation(mode)} onSelectMessage={chatOnSelectMessage} onLinkVault={(path) => void linkVault(currentSessionId, path)} onUnlinkVault={() => void unlinkVault(currentSessionId)} onListFs={(path) => client.listFs(path)} onRenameSession={(title) => void renameSession(currentSessionId, title)} onSetSessionTags={(tags) => void setSessionTags(currentSessionId, tags)} onRetitleSession={() => void retitleSession(currentSessionId)} onGetJourney={getJourney} onRequestJourneyUpdate={requestJourneyUpdate} onExport={exportConversation} sources={sources} pendingJump={pendingJump} onJumpHandled={() => setPendingJump(null)} /></ChatErrorBoundary></div>}
            {view === "runs" && <RunsView sessions={sessions} run={run} onOpen={(id) => { setCurrentSessionId(id); setView("chat"); setSidebarOpen(false); }} />}
            {view === "graph" && <GraphView session={currentSession} rows={graphRows} runs={graphRuns} onOpenChat={(messageId) => { if (messageId != null && !messages.some((m) => m.id === messageId && m.isActive)) void selectMessageBranch(messageId); setPendingJump(messageId); setView("chat"); }} onSelectBranch={(id) => void selectMessageBranch(id)} onRewind={(message) => rewindHere(message)} />}
            {view === "memory" && <MemoryView currentSessionId={currentSessionId} onStatus={() => client.memoryStatus()} onRecall={(input) => client.memoryRecall(input)} onBank={(input) => client.memoryBank(input)} onUnitDetail={(id) => client.getMemoryUnit(id)} onInvalidateUnit={(id, reason) => client.invalidateMemoryUnit(id, reason)} onReactivateUnit={(id, reason) => client.reactivateMemoryUnit(id, reason)} onSupersedeUnit={(id, text, reason) => client.supersedeMemoryUnit(id, text, reason)} onDeleteUnit={(id) => client.deleteMemoryUnit(id)} onSetUnitCore={(id, on) => client.setMemoryUnitCore(id, on)} onConsolidate={() => client.consolidateMemory()} onListRecords={(status) => client.listMemoryRecords(status)} onCreateRecord={(input) => client.createMemoryRecord(input)} onApproveRecord={(id) => client.approveMemoryRecord(id)} onRejectRecord={(id) => client.rejectMemoryRecord(id)} onListConsolidations={(status) => client.listConsolidations(status)} onApproveConsolidation={(id) => client.approveConsolidation(id)} onRejectConsolidation={(id) => client.rejectConsolidation(id)} />}
            {view === "workspace" && <WorkspaceView />}
            {view === "observatory" && <ObservatoryView onInfra={() => client.getInfra()} />}
            {view === "providers" && <ProvidersView providers={providers} onUpsert={saveProvider} onDelete={removeProvider} onCheckDraft={(endpoint, apiKey) => client.checkProvider({ endpoint, apiKey })} onCheckSaved={(name) => client.checkProviderRoute(name)} defaultProvider={defaultProvider} onSetPrimary={(name) => saveSetting("provider.default", name)} />}
            {view === "profiles" && <ProfilesView profiles={profiles} models={modelOptions.models} providers={providers} onSave={saveProfile} onDelete={removeProfile} />}
            {view === "knowledge" && <KnowledgeView />}
            {view === "prompts" && <PromptsView profiles={profiles} onListPacks={() => client.listPromptPacks()} onCreatePack={(input) => client.createPromptPack(input)} onGetPack={(id) => client.getPromptPack(id)} onUpsertFile={(packId, path, content) => client.upsertPackFile(packId, path, content)} onDeleteFile={(packId, path) => client.deletePackFile(packId, path)} onDeletePack={(id) => client.deletePromptPack(id)} onPreview={(packId, plan, profileId) => client.previewPromptPack(packId, plan, profileId)} onSaveProfilePlan={(profileId, packId, plan) => saveProfilePlan(profileId, packId, plan)} />}
            {view === "skills" && <SkillsView onListSkills={() => client.listSkills()} onSaveSkill={(input) => client.saveSkill(input)} onDeleteSkill={(name) => client.deleteSkill(name)} onListTrash={() => client.listSkillTrash()} onRestoreSkill={(name) => client.restoreSkill(name)} onDeleteTrashSkill={(name) => client.deleteSkillTrash(name)} onSetSkillEnabled={(name, enabled) => client.setSkillEnabled(name, enabled)} />}
            {view === "tools" && <ToolsView />}
            {view === "settings" && <SettingsView settings={settings} loaded={settingsLoaded} loadError={settingsLoadError} onSave={saveSetting} onReload={() => void loadSettings()} onCheckEndpoint={(endpoint) => client.checkProvider({ endpoint })} onCheckMemory={() => client.memoryStatus()} alwaysAllowed={alwaysAllowed} onRemoveAlwaysAllowed={(tool) => void removeAlwaysAllowedTool(tool)} onReloadAlwaysAllowed={() => void loadAlwaysAllowed()} />}
          </div>
          <footer className="statusbar"><span><span className="statusbar-mark" /> {connected ? "harness connected" : "harness offline"}</span><span className="statusbar-spacer" /></footer>
        </main>
      </div>
    </div>
  );
}

export default App;
