// Mock provider for the group chat invite e2e test (docs/group-chat.md).
//
// Behavior:
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> scripted responses, one per completion.
//
//   S1: 1:1 session (profile alice, no speakers):
//     1: text "Solo reply one."                          (alice, ordinary run)
//   (the test invites bob; the roster becomes alice,bob)
//   S1 continued - first group run (budget 3, global default):
//     2: text "Alice in the group."                      (alice r1)
//     3: text "Bob in the group."                        (bob r1)
//     4: text "Alice round two."                         (alice r2)
//     5: group.abstain                                   (bob r2)
//     6: group.abstain                                   (alice r3)
//     7: group.abstain                                   (bob r3)
//        -> group.completed {rounds: 2, termination: unanimous abstention}
//        (rounds counts per-speaker depth: alice reached 3, bob 3 - both at
//        the cap with the last round abstaining, so the closure fires.)
//   (the test invites carol; the roster becomes alice,bob,carol)
//   Second group run:
//     8:  text "Carol joins the table."                  (alice r1)
//     9:  text "Bob again."                              (bob r1)
//     10: text "Carol first words."                      (carol r1)
//     11: group.abstain                                  (alice r2)
//     12: group.abstain                                  (bob r2)
//     13: group.abstain                                  (carol r2)
//        -> group.completed {rounds: 2, termination: unanimous abstention}
//
//   GET  /requests             -> the recorded request bodies, so the test
//     can assert the group framing names the joined roster.
//
// Usage: MOCK_PORT=9094 node harness/tests/mock_invite_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9094);
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
        id: `mock-invite-${completions}`,
        object: "chat.completion.chunk",
        choices: [{ index: 0, delta }],
      });

      const textCall = (text) => {
        send(chunk({ content: text }));
      };
      const abstainCall = () => {
        send(chunk({
          tool_calls: [
            { index: 0, id: `call_abstain_${completions}`, function: { name: "group.abstain", arguments: "{}" } },
          ],
        }));
      };

      switch (completions) {
        case 1: textCall("Solo reply one."); break;
        case 2: textCall("Alice in the group."); break;
        case 3: textCall("Bob in the group."); break;
        case 4: textCall("Alice round two."); break;
        case 5: abstainCall(); break;
        case 6: abstainCall(); break;
        case 7: abstainCall(); break;
        case 8: textCall("Carol joins the table."); break;
        case 9: textCall("Bob again."); break;
        case 10: textCall("Carol first words."); break;
        case 11: abstainCall(); break;
        case 12: abstainCall(); break;
        case 13: abstainCall(); break;
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

server.listen(port, "127.0.0.1", () => {});
