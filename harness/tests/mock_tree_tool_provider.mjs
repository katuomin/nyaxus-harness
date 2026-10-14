// Mock provider for the process-tree e2e test.
//
// Behavior:
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> request 1: a "terminal" tool call that
//     spawns a long-lived grandchild (`sh -c "ping -n 60 ..."`); request
//     2+: a plain text answer.
//
// The command is a nested shell: the outer shell (direct child) waits for
// the inner shell, which waits for ping/sleep - a true grandchild. The
// point is to leave that grandchild running when the tool times out, so the
// test can assert the whole tree (not just the direct child) was
// terminated.
//
// Usage: MOCK_PORT=8950 node harness/tests/mock_tree_tool_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8950);
// Platform-specific long-lived grandchild: nested cmd on Windows,
// nested sh on POSIX (a true grandchild either way).
const treeCommand =
  process.env.MOCK_COMMAND || "cmd /c ping -n 60 127.0.0.1";
let completions = 0;

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    // Explicit content-length: the daemon's get_body client reads until EOF
    // and does not decode chunked transfer encoding.
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

      if (completions === 1) {
        send({
          id: `mock-tree-${completions}`,
          object: "chat.completion.chunk",
          choices: [
            {
              index: 0,
              delta: {
                tool_calls: [
                  {
                    index: 0,
                    id: "call_tree",
                    function: { name: "terminal", arguments: JSON.stringify({ command: treeCommand }) },
                  },
                ],
              },
            },
          ],
        });
      } else {
        send({
          id: `mock-tree-${completions}`,
          object: "chat.completion.chunk",
          choices: [{ index: 0, delta: { content: "done" } }],
        });
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
  console.log(`mock tree-tool provider listening on 127.0.0.1:${port}`);
});
