// Mock provider for the sudo password gate e2e (docs/sudo-gate.md).
//
// One completion counter drives five runs, each two completions (the
// terminal call, then the closing text that echoes the expected tool
// result - the echo is what the test asserts against the recorded
// provider requests):
//   run 1: terminal {"command":"sudo whoami"}   -> gate, answered once
//   run 2: terminal {"command":"sudo id"}       -> gate, answered WRONG
//   run 3: terminal {"command":"sudo whoami"}   -> gate, answered ttl
//   run 4: terminal {"command":"sudo ls"}       -> ttl grant covers, no gate
//   run 5: terminal {"command":"sudo ps"}       -> nnp hint, no gate
//
// Usage: MOCK_PORT=9075 node harness/tests/mock_sudo_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9075);
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
        id: `mock-sudo-${completions}`,
        object: "chat.completion.chunk",
        choices: [{ index: 0, delta }],
      });

      const terminalCall = (command) => {
        send(chunk({
          tool_calls: [
            { index: 0, id: `call_term_${completions}`, function: { name: "terminal", arguments: JSON.stringify({ command }) } },
          ],
        }));
      };
      const textCall = (text) => {
        send(chunk({ content: text }));
      };

      switch (completions) {
        case 1: terminalCall("sudo whoami"); break;
        case 2: textCall("r1 result: sudo-ok whoami"); break;
        case 3: terminalCall("sudo id"); break;
        case 4: textCall("r2 result: rejected"); break;
        case 5: terminalCall("sudo whoami"); break;
        case 6: textCall("r3 result: sudo-ok whoami"); break;
        case 7: terminalCall("sudo ls"); break;
        case 8: textCall("r4 result: sudo-ok ls"); break;
        case 9: terminalCall("sudo ps"); break;
        case 10: textCall("r5 result: hinted"); break;
        default: textCall("done"); break;
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
  console.log(`mock sudo provider listening on 127.0.0.1:${port}`);
});
