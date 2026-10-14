// Minimal OpenAI-compatible mock provider for the web research e2e test.
//
// Behavior (stateful, per server lifetime):
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> SSE stream
//     request 1: web.search tool call
//     request 2: web.fetch tool call (the mock web server's /page.html)
//     request 3: web.use tool call (src:1)
//     request 4: a plain text answer that cites [src:1] (ends the tool loop)
//     request 5: web.search tool call (a query SearXNG returns nothing for,
//                so the daemon falls back to Firecrawl /v1/search)
//     request 6: web.fetch tool call (an https:// URL, routed through
//                Firecrawl /v1/scrape)
//     request 7: web.use tool call (src:2)
//     request 8: a plain text answer that cites [src:2] (ends the tool loop)
//
// The test asserts the run completes without any approve/deny command (all
// three web tools are read-only), that the page text reached the model, and
// that the source ledger records the fetch with used=true.
//
// Usage: MOCK_PORT=9022 WEB_PORT=9021 node harness/tests/mock_web_research_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9022);
const webPort = process.env.WEB_PORT || "9021";
let completions = 0;

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    // Explicit content-length: the daemon's client reads until EOF and does
    // not decode chunked transfer encoding on this path.
    const payload = JSON.stringify({ data: [{ id: "mock-model", meta: { n_ctx: 4096 } }] });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }

  if (req.method === "POST" && req.url === "/v1/chat/completions") {
    let body = "";
    req.on("data", (chunk) => {
      body += chunk;
    });
    req.on("end", () => {
      completions += 1;
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      const send = (obj) => res.write(`data: ${JSON.stringify(obj)}\n\n`);
      const toolCall = (id, name, args) => ({
        id: `mock-${completions}`,
        object: "chat.completion.chunk",
        choices: [
          {
            index: 0,
            delta: {
              tool_calls: [
                { index: 0, id, function: { name, arguments: JSON.stringify(args) } },
              ],
            },
          },
        ],
      });

      if (body.includes("search the big list")) {
        // Big-search branch: a SearXNG-shaped JSON body that overruns a
        // single 8 KiB read (header + body in one segment). Checked BEFORE
        // the pager branch: a later session's [PREV SESSION] block carries
        // the pager session's title, so the pager substring would match
        // first and swallow this request. Echo-driven (not
        // counter-driven): once the search result is in the conversation
        // the run is done, and title/extraction requests that carry the
        // message text but no result get a tool call their endpoint
        // ignores.
        if (body.includes("Big search result")) {
          send({
            id: `mock-big-${completions}`,
            object: "chat.completion.chunk",
            choices: [{ index: 0, delta: { content: "the big list ends with result 20." } }],
          });
        } else {
          send(toolCall("call_bigsearch", "web.search", { query: "big search test" }));
        }
      } else if (completions <= 0 || body.includes("read the big page in full")) {
        // Page-fixture branch: request-driven (NOT completion-indexed -
        // other flows (title generation) also hit the provider and would
        // skew an index). Echo-driven: page 1 first, page 2 once page 1's
        // result is in the conversation, done once page 2's is.
        if (body.includes("SECTION-13")) {
          send({
            id: `mock-pager-${completions}`,
            object: "chat.completion.chunk",
            choices: [{ index: 0, delta: { content: "both pages read." } }],
          });
        } else if (body.includes("SECTION-1 ")) {
          send(toolCall("call_fetch_big2", "web.fetch", { url: `http://127.0.0.1:${webPort}/big.html`, page: 2 }));
        } else {
          send(toolCall("call_fetch_big", "web.fetch", { url: `http://127.0.0.1:${webPort}/big.html` }));
        }
      } else if (completions === 1) {
        send(toolCall("call_search", "web.search", { query: "web research test page" }));
      } else if (completions === 2) {
        send(toolCall("call_fetch", "web.fetch", { url: `http://127.0.0.1:${webPort}/page.html` }));
      } else if (completions === 3) {
        send(toolCall("call_use", "web.use", { ids: "src:1" }));
      } else if (completions === 4) {
        send({
          id: `mock-${completions}`,
          object: "chat.completion.chunk",
          choices: [
            {
              index: 0,
              delta: {
                content:
                  "The mock page says hello from the web research test. [src:1]",
              },
            },
          ],
        });
      } else if (completions === 5) {
        // A query the mock SearXNG returns nothing for -> the daemon must
        // fall back to the Firecrawl /v1/search endpoint.
        send(toolCall("call_search2", "web.search", { query: "firecrawl fallback query" }));
      } else if (completions === 6) {
        // An https URL -> the daemon must route it through Firecrawl
        // /v1/scrape (the daemon's client has no TLS).
        send(toolCall("call_fetch2", "web.fetch", { url: "https://example.com/firecrawl-page" }));
      } else if (completions === 7) {
        // Fresh temp DB: run 1's fetch was src:1, so this one is src:2.
        send(toolCall("call_use2", "web.use", { ids: "src:2" }));
      } else if (completions === 9) {
        // Read-budget pager: page 1 of a page whose extraction overruns the
        // per-page budget (the default read). No page parameter.
        send(toolCall("call_fetch_big", "web.fetch", { url: `http://127.0.0.1:${webPort}/big.html` }));
      } else if (completions === 10) {
        // The continuation page: the previous result named page=2.
        send(toolCall("call_fetch_big2", "web.fetch", { url: `http://127.0.0.1:${webPort}/big.html`, page: 2 }));
      } else if (completions === 11) {
        send({
          id: `mock-${completions}`,
          object: "chat.completion.chunk",
          choices: [
            {
              index: 0,
              delta: {
                content: "both pages read.",
              },
            },
          ],
        });
      } else {
        send({
          id: `mock-${completions}`,
          object: "chat.completion.chunk",
          choices: [
            {
              index: 0,
              delta: {
                content: "The firecrawl page says scraped. [src:2]",
              },
            },
          ],
        });
      }

      // Final chunk carrying usage, like llama.cpp's OpenAI server.
      send({
        id: `mock-${completions}`,
        object: "chat.completion.chunk",
        choices: [],
        usage: {
          prompt_tokens: 120 + completions * 10,
          completion_tokens: 12 + completions,
          total_tokens: 132 + completions * 11,
        },
      });

      res.write("data: [DONE]\n\n");
      res.end();
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock provider listening on 127.0.0.1:${port}`);
});
