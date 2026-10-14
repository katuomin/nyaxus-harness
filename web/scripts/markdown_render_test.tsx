import { renderToStaticMarkup } from "react-dom/server";
import { MarkdownContent } from "../src/markdown";

function assert(condition: boolean, message: string): void {
  if (!condition) throw new Error(`markdown_render_test failed: ${message}`);
}

const markdown = [
  "# CommonMark + GFM",
  "",
  "A paragraph with **bold**, *italic*, ***bold-italic***, ~~strike~~, `inline code`, and a [safe link](https://example.com).",
  "",
  "Setext heading",
  "---------------",
  "",
  "- unordered one",
  "  - nested two",
  "  - nested three",
  "- back to one",
  "",
  "1. ordered one",
  "2. ordered two",
  "   1. nested ordered",
  "   2. nested ordered two",
  "",
  "- [x] done",
  "- [ ] todo",
  "",
  "> outer quote",
  ">> nested quote",
  "> back to outer",
  "",
  "| Left | Center | Right |",
  "| :--- | :----: | ----: |",
  "| a | b | c |",
  "",
  "Here is a four-backtick fence:",
  "````js",
  "```js",
  "const value = `inside`;",
  "```",
  "````",
  "",
  "https://example.com and javascript:alert(1)",
  "",
  "<div onclick=\"alert(1)\">raw html</div>",
  "",
  "The function $f(x) = x^2$ is quadratic, and a lone $5 stays plain text.",
  "",
  "$$",
  "\\frac{a}{b} + \\int_0^1 x\\,dx",
  "$$",
  "",
  "Math stays raw inside fences:",
  "```",
  "$f(x)$ and $$",
  "```",
].join(String.fromCharCode(10));

const html = renderToStaticMarkup(<MarkdownContent content={markdown} />);

assert(html.includes("<h1>CommonMark + GFM</h1>"), "heading missing");
assert(html.includes("<h2>Setext heading</h2>"), "setext heading missing");
assert(html.includes("<strong>bold</strong>"), "bold missing");
assert(html.includes("<em>italic</em>"), "italic missing");
assert(html.includes("<em><strong>bold-italic</strong></em>") || html.includes("<strong><em>bold-italic</em></strong>"), "combined emphasis missing");
assert(html.includes("<del>strike</del>"), "strikethrough missing");
assert(html.includes("class=\"markdown-inline-code\">inline code</code>"), "inline code missing");
assert(html.includes("<ul>"), "unordered list missing");
assert(html.includes("<ol>"), "ordered list missing");
assert(html.includes("nested ordered"), "nested ordered list missing");
assert(html.includes("type=\"checkbox\""), "task list missing");
assert(html.includes("<blockquote>"), "blockquote missing");
assert(html.includes("<table>"), "table missing");
assert(html.includes("language-js"), "fenced code language missing");
assert(html.includes("const value = `inside`;"), "nested fence content missing");
assert(html.includes("href=\"https://example.com\""), "safe link missing");
assert(!html.includes("href=\"javascript:alert(1)\""), "unsafe url survived");
assert(!html.includes("onclick"), "raw html attribute survived");
assert(html.includes("class=\"katex\""), "inline math not rendered by KaTeX");
assert(html.includes("class=\"katex-display\""), "display math not rendered by KaTeX");
assert(html.includes("$5"), "lone dollar sign was treated as math");
assert(html.includes("$f(x)$ and $$"), "math inside a code fence was rendered");

console.log("markdown_render_test passed");

// Repair pass: a REAL wrapped answer gets unwrapped once, while normal
// mid-message fenced blocks - including fences with text around them -
// pass through untouched (regression: text + ```block``` + text rendered
// as only the block interior).
import { repairMarkdownFences } from "../src/markdown";
{
  const wrapped = ["```", "line one", "line two", "```"].join("\n");
  if (repairMarkdownFences(wrapped) !== "line one\nline two\n") {
    fail(`a true envelope should unwrap: ${JSON.stringify(repairMarkdownFences(wrapped))}`);
  }
  const around = ["this is my message", "```", "with a block quote", "```", "so cool"].join("\n");
  if (repairMarkdownFences(around) !== around) {
    fail(`mid-message fences must not unwrap: ${JSON.stringify(repairMarkdownFences(around))}`);
  }
  const twoBlocks = ["intro", "```", "first", "```", "middle", "```", "second", "```", "outro"].join("\n");
  if (repairMarkdownFences(twoBlocks) !== twoBlocks) {
    fail(`multiple fenced blocks must pass through: ${JSON.stringify(repairMarkdownFences(twoBlocks))}`);
  }
  const fencedLanguage = ["```js", "const a = 1;", "```"].join("\n");
  if (repairMarkdownFences(fencedLanguage) !== fencedLanguage) {
    fail("an info-string fence is content intent, not a wrapper");
  }
  const trailingText = ["intro", "```", "code", "```"].join("\n");
  if (repairMarkdownFences(trailingText) !== trailingText) {
    fail("a closed block at the end of a message is not a wrapper");
  }
  const indentedEnvelope = ["", "```", "kept", "```"].join("\n");
  if (repairMarkdownFences(indentedEnvelope) !== "kept\n") {
    fail(`leading blank lines should not defeat the envelope: ${JSON.stringify(repairMarkdownFences(indentedEnvelope))}`);
  }
  // The renderer-level check: the user-message shape renders ALL of its lines.
  const userMessageHtml = renderToStaticMarkup(
    <MarkdownContent content={around} />, // unchanged passthrough
  );
  if (!userMessageHtml.includes("this is my message") ||
      !userMessageHtml.includes("so cool") ||
      !userMessageHtml.includes("with a block quote")) {
    fail("a mid-message fence message lost its text around the block");
  }
}
