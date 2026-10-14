// Mock provider for the spill-locator e2e test.
//
// The CALLS env var is a JSON array of script entries ({"name","arguments"}
// tool call or {"text":"..."} plain answer), answered in order by global
// completion count. Tool-call argument values may contain the placeholder
// "$LOCATOR": before answering, the mock looks for the newest spill locator
// ("spill:<session>:<n>") in the request's messages (the tool results the
// daemon sent back) and substitutes it - so a scripted spill.read call can
// use the locator the previous turn's spilled result produced.
//
// Usage: MOCK_PORT=9041 CALLS='[...]' node harness/tests/mock_spill_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9041);
const calls = JSON.parse(process.env.CALLS || "[]");
let completions = 0;

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    const payload = JSON.stringify({ data: [{ id: "mock-model", meta: { n_ctx: 262144 } }] });
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
      console.error(`[mock] POST chat/completions #${completions} (body ${Buffer.byteLength(body)} bytes)`);
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      res.on("error", () => {});
      req.socket.on("error", () => {});
      const send = (obj) => res.write(`data: ${JSON.stringify(obj)}\n\n`);

      const entry = calls[completions - 1];
      if (entry && entry.name) {
        // Substitute the $LOCATOR placeholder from the conversation so far.
        let argumentsJson = JSON.stringify(entry.arguments);
        const locatorMatch = body.match(/spill:[^"\\]+:\d+/);
        if (argumentsJson.includes("$LOCATOR") && !locatorMatch) {
          console.error(`[mock] all spill occurrences: ${JSON.stringify([...body.matchAll(/spill:[^"\\]{0,40}/g)].map(m => m[0]))}`);
        }
        if (locatorMatch && argumentsJson.includes("$LOCATOR")) {
          argumentsJson = argumentsJson.replaceAll("$LOCATOR", locatorMatch[0]);
        }
        send({
          id: `mock-${completions}`,
          object: "chat.completion.chunk",
          choices: [
            {
              index: 0,
              delta: {
                tool_calls: [
                  {
                    index: 0,
                    id: `call_${completions}`,
                    function: { name: entry.name, arguments: argumentsJson },
                  },
                ],
              },
            },
          ],
        });
      } else {
        send({
          id: `mock-${completions}`,
          object: "chat.completion.chunk",
          choices: [{ index: 0, delta: { content: (entry && entry.text) || "done" } }],
        });
      }

      send({
        id: `mock-${completions}`,
        object: "chat.completion.chunk",
        choices: [],
        usage: {
          prompt_tokens: 100 + completions * 10,
          completion_tokens: 12 + completions,
          total_tokens: 112 + completions * 11,
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
  console.log(`mock spill provider listening on 127.0.0.1:${port}`);
});
