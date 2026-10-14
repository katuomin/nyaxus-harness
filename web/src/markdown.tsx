import { memo, useMemo } from "react";
import type { ReactNode } from "react";
import Markdown from "react-markdown";
import type { Components } from "react-markdown";
import remarkGfm from "remark-gfm";
import remarkMath from "remark-math";
import rehypeKatex from "rehype-katex";
import "katex/dist/katex.min.css";

function stripMarkdownNode<T extends { node?: unknown }>(props: T): Omit<T, "node"> {
  const result = { ...props } as T & { node?: unknown };
  delete result.node;
  return result;
}

function safeUrlTransform(url: string): string {
  const value = url.trim();
  if (/^(https?:|mailto:)/i.test(value)) return value;
  if (value.startsWith("/") && !value.startsWith("//")) return value;
  if (value.startsWith("#")) return value;
  return "";
}

// Chat messages expect a single newline to render as a visible line break
// (Slack/Discord style), but CommonMark treats a lone newline as a soft break
// that collapses to a space. Convert single newlines outside fenced code
// blocks and display-math blocks into hard breaks by trailing each line with
// two spaces. Trailing whitespace is ignored by headings, lists, tables, and
// blockquotes, so this is safe to apply broadly.
function toHardBreaks(content: string): string {
  const lines = content.split("\n");
  const out: string[] = [];
  let fence: string | null = null;
  let math = false;
  for (const line of lines) {
    const marker = /^\s*(```|~~~)/.exec(line)?.[1];
    if (marker) {
      if (fence === null) fence = marker;
      else if (marker === fence) fence = null;
      out.push(line);
      continue;
    }
    if (fence !== null) {
      out.push(line);
      continue;
    }
    // Display math ($$ ... $$) must keep its exact line structure: a
    // single-line $$...$$ block passes through untouched, and a block opens
    // on a line starting with $$ and closes on the line carrying the
    // closing $$.
    if (!math) {
      const open = /^ {0,3}\$\$/.exec(line);
      if (open && line.trim().endsWith("$$") && line.length > open[0].length + 1) {
        out.push(line);
        continue;
      }
      if (open) math = true;
    }
    if (math) {
      out.push(line);
      if (/\$\$\s*$/.test(line)) math = false;
      continue;
    }
    if (/[ \t]{2,}$/.test(line) || /\\$/.test(line)) {
      out.push(line);
      continue;
    }
    out.push(line + "  ");
  }
  return out.join("\n");
}

// web_research citations ([src:2]) become chips: rewritten to an inline code
// span here, tagged by the code component below for a distinct style.
function chipifyCitations(content: string): string {
  return content.replace(/\[src:(\d+)\]/g, "`src:$1`");
}

const markdownComponents: Components = {
  pre: (props) => {
    const { children } = props;
    return <pre {...stripMarkdownNode(props)} className="markdown-code">{children}</pre>;
  },
  code: (props) => {
    const { className, children } = props;
    const rest = stripMarkdownNode(props);
    const text = String(Array.isArray(children) ? children.join("") : children ?? "");
    const citation = !className && /^src:\d+$/.test(text);
    return <code {...rest} className={className || (citation ? "markdown-inline-code markdown-citation" : "markdown-inline-code")}>{children}</code>;
  },
  a: (props) => {
    const { href, title, children } = props;
    return (
      <a
        {...stripMarkdownNode(props)}
        href={href}
        title={title}
        target="_blank"
        rel="noreferrer noopener"
      >
        {children}
      </a>
    );
  },
  img: (props) => {
    const { src, alt, title } = props;
    if (!src) return <span className="markdown-image-missing">image unavailable</span>;
    return <img {...stripMarkdownNode(props)} src={src} alt={alt || "markdown image"} title={title} loading="lazy" />;
  },
  table: (props) => {
    const { children } = props;
    return (
      <div className="markdown-table-wrap">
        <table {...stripMarkdownNode(props)}>{children}</table>
      </div>
    );
  },
};

// Clicking a citation chip jumps to the References section for this message.
// Re-triggers the flash animation on repeat clicks by resetting it first.
function jumpToCitation(targetId: string) {
  const el = document.getElementById(targetId);
  if (!el) return;
  el.scrollIntoView({ behavior: "smooth", block: "center" });
  el.classList.remove("citation-flash");
  void el.offsetWidth;
  el.classList.add("citation-flash");
  window.setTimeout(() => el.classList.remove("citation-flash"), 1200);
}

// `markdownComponents` is shared across all messages; only messages that own a
// References section need the per-instance override that makes chips clickable.
function componentsWithCitations(citationTarget: string): Components {
  return {
    ...markdownComponents,
    code: (props) => {
      const { className, children } = props;
      const rest = stripMarkdownNode(props);
      const text = String(Array.isArray(children) ? children.join("") : children ?? "");
      const citation = !className && /^src:\d+$/.test(text);
      if (citation) {
        return (
          <code
            {...rest}
            className="markdown-inline-code markdown-citation markdown-citation-link"
            onClick={() => jumpToCitation(citationTarget)}
          >
            {children}
          </code>
        );
      }
      return <code {...rest} className={className || "markdown-inline-code"}>{children}</code>;
    },
  };
}

// LLMs sometimes wrap a complete answer in an outer ``` fence, which then
// renders as one broken literal text block. Strip the wrapper ONLY for a
// true all-envelope:
// the opener is the FIRST line of the content (a bare fence, no info
// string) and the closer is the LAST non-empty line - a message that has
// text before or after its own fenced blocks (```` ```code``` ```` mid-
// message) is normal markdown and must pass through untouched. The interior
// must contain no same-marker close (CommonMark would have ended the block
// there; unclear intent, leave untouched).
export function repairMarkdownFences(content: string): string {
  if (!content.includes("```") && !content.includes("~~~")) return content;
  const lines = content.split("\n");
  let start = 0;
  while (start < lines.length && lines[start].trim() === "") start++;
  const opener = /^\s*(```|~~~)\s*$/.exec(lines[start] ?? "");
  if (!opener) return content; // the envelope must START the content
  const marker = opener[1];
  let last = -1;
  for (let i = lines.length - 1; i >= 0; i--) {
    if (lines[i].trim() === "") continue;   // trailing whitespace only
    if (/^\s*(```|~~~)\s*$/.test(lines[i])) last = i;
    break;                                   // the final line decides
  }
  if (last <= start) return content;
  for (let i = start + 1; i < last; i++) {
    if (lines[i].includes(marker)) return content;
  }
  return lines.slice(start + 1, last).join("\n") + "\n";
}

// Memoized: streaming deltas re-render the whole transcript on every event
// and re-parsing unchanged messages' markdown is the dominant cost in long
// chats. A stable content string skips the re-parse entirely.
export const MarkdownContent = memo(function MarkdownContent({
  content,
  citationTarget,
}: { content: string; citationTarget?: string }): ReactNode {
  const components = useMemo(
    () => (citationTarget ? componentsWithCitations(citationTarget) : markdownComponents),
    [citationTarget],
  );
  return (
    <Markdown
      remarkPlugins={[remarkGfm, remarkMath]}
      rehypePlugins={[rehypeKatex]}
      skipHtml
      urlTransform={safeUrlTransform}
      components={components}
    >
      {toHardBreaks(chipifyCitations(repairMarkdownFences(content)))}
    </Markdown>
  );
});
