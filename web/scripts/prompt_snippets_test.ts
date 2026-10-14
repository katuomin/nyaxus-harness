// Regression test for the prompt-snippets merge (docs/prompt-snippets.md):
// selected snippet bodies merge into the sent
// message as prepend-group -> text -> append-group, each group sorted by
// (order, name); an empty selection leaves the message unchanged.
import { mergePromptSnippets, type PromptSnippet } from "../src/harness";

function fail(message: string): never {
  throw new Error(`prompt_snippets_test failed: ${message}`);
}

function snippet(
  name: string,
  placement: "prepend" | "append",
  order: number,
  content: string,
): PromptSnippet {
  return { name, placement, order, content };
}

const ALL = [
  snippet("tone", "prepend", 10, "Be concise."),
  snippet("citations", "append", 10, "Cite sources."),
  snippet("style", "prepend", 5, "Use UK spelling."),
  snippet("none", "append", 0, ""),
];

// No selection: the message is unchanged.
{
  if (mergePromptSnippets("hello", ALL, new Set()) !== "hello") {
    fail("empty selection must return the message unchanged");
  }
  if (mergePromptSnippets("hello", ALL, new Set(["missing"])) !== "hello") {
    fail("selecting a non-existent snippet must be a no-op");
  }
}

// Prepend group before, append group after, each sorted by (order, name).
{
  const selected = new Set(["tone", "citations", "style"]);
  const merged = mergePromptSnippets("the message", ALL, selected);
  const expected = [
    "Use UK spelling.", // prepend, order 5
    "Be concise.", // prepend, order 10
    "the message",
    "Cite sources.", // append, order 10
  ].join("\n\n");
  if (merged !== expected) fail(`merge order wrong:\n${merged}`);
}

// Order ties break by name.
{
  const tied = [
    snippet("b", "prepend", 1, "B"),
    snippet("a", "prepend", 1, "A"),
  ];
  const merged = mergePromptSnippets("m", tied, new Set(["a", "b"]));
  if (merged !== "A\n\nB\n\nm") fail(`tie-break wrong:\n${merged}`);
}

// Empty snippet bodies are dropped, not blank lines.
{
  const merged = mergePromptSnippets("m", ALL, new Set(["tone", "none"]));
  if (merged !== "Be concise.\n\nm") fail(`empty body leaked:\n${merged}`);
}

console.log("prompt_snippets_test passed");
