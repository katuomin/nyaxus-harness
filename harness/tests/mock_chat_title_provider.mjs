// Mock provider for the chat.title e2e test.
//
// Behavior:
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> scripted responses, one per completion:
//     1: chat.title "Mock Title Check"   (provisional title, first turn)
//     2: text "done"
//     3: chat.title "Retitled Mock"      (manual re-title armed)
//     4: text "done 2"
//     5: chat.title "Stray Title"        (hard guard: title already final)
//     6: text "done 3"
//   GET  /requests             -> the recorded request bodies, so the test
//     can assert which requests carried the chat.title tool declaration.
//
// Usage: MOCK_PORT=8950 node harness/tests/mock_chat_title_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8950);
let completions = 0;
const requests = [];

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    const payload = JSON.stringify({ data: [{ id: "mock-model", meta: { n_ctx: 4096 } }] });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }

  if (req.method === "GET" && req.url === "/requests") {
    const payload = JSON.stringify(requests);
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
      let parsed = null;
      try {
        parsed = JSON.parse(body);
      } catch {
        parsed = { unparseable: true };
      }
      requests.push(parsed);
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      const send = (obj) => res.write(`data: ${JSON.stringify(obj)}\n\n`);
      const chunk = (delta) => ({
        id: `mock-title-${completions}`,
        object: "chat.completion.chunk",
        choices: [{ index: 0, delta }],
      });

      const titleCall = (title) => {
        send(chunk({ reasoning_content: `titling ${completions}` }));
        send(chunk({
          tool_calls: [
            { index: 0, id: `call_title_${completions}`, function: { name: "chat.title", arguments: JSON.stringify({ title }) } },
          ],
        }));
      };

      if (completions === 1) titleCall("Mock Title Check");
      else if (completions === 3) titleCall("Retitled Mock");
      else if (completions === 5) titleCall("Stray Title");
      else {
        send(chunk({ reasoning_content: `final ${completions}` }));
        send(chunk({ content: `done ${completions}` }));
      }

      res.write("data: [DONE]\n\n");
      res.end();
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock chat-title provider listening on 127.0.0.1:${port}`);
});
