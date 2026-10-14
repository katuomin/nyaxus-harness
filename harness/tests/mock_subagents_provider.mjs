// Mock provider for the agent.spawn (subagents) e2e test.
//
// Behavior:
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> scripted responses, one per completion:
//     1: agent.spawn {profile: "worker", objective: ...}   (parent run 1)
//     2: text "Worker report: counted 1, 2, 3."            (the child run)
//     3: text "Done."                                       (parent run 1, after harvest)
//     4: agent.spawn {profile: "notallowed", ...}          (parent run 2, allowlist miss)
//     5: text "done 2"                                      (parent run 2, after the error)
//   GET  /requests             -> the recorded request bodies, so the test
//     can assert which requests carried the agent.spawn tool declaration.
//
// The parent and the child share this one endpoint; the completion counter
// is what separates their turns.
//
// Usage: MOCK_PORT=9045 node harness/tests/mock_subagents_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9045);
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
        id: `mock-spawn-${completions}`,
        object: "chat.completion.chunk",
        choices: [{ index: 0, delta }],
      });

      const spawnCall = (profile) => {
        send(chunk({
          tool_calls: [
            { index: 0, id: `call_spawn_${completions}`, function: { name: "agent.spawn", arguments: JSON.stringify({ profile, objective: `task ${completions}` }) } },
          ],
        }));
      };
      const textCall = (text) => {
        send(chunk({ content: text }));
      };

      if (completions === 1) spawnCall("worker");
      else if (completions === 2) textCall("Worker report: counted 1, 2, 3.");
      else if (completions === 3) textCall("Done.");
      else if (completions === 4) spawnCall("notallowed");
      else textCall("done 2");

      res.write("data: [DONE]\n\n");
      res.end();
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock subagents provider listening on 127.0.0.1:${port}`);
});
