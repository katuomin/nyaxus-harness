// Mock provider for the group chat e2e test (docs/group-chat.md).
//
// Behavior:
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> scripted responses, one per completion.
//     One completion per turn (an abstain turn breaks the tool loop right
//     after the group.abstain dispatch, so it consumes exactly one too):
//
//   S1 (speakers alice,bob; budget 3) - unanimous-abstention closure:
//     1: text "I think we should ship it this week."     (alice r1)
//     2: group.abstain                                    (bob r1)
//     3: group.abstain                                    (alice r2)
//     4: group.abstain                                    (bob r2)
//        -> group.completed {rounds: 2, termination: unanimous abstention}
//   S2 (speakers alice,bob; budget set to 1 via settings) - budget cap:
//     5: text "Round one point A."                        (alice r1)
//     6: text "Round one point B."                        (bob r1)
//        -> group.completed {rounds: 1, termination: budget}
//   S3 (speakers alice,bob,carol; budget 3) - @-nudge (immediate: the
//   mentioned peer takes the floor right after the mentioning turn):
//     7:  text "Launch plan: freeze scope on Monday."     (alice r1)
//     8:  text "@Carol please draft the rollback plan."   (carol r1, pulled)
//     9:  text "I will own the rollback plan."            (bob r1)
//     10: group.abstain                                   (carol r2, pass opens on her)
//     11: group.abstain                                   (alice r2)
//     12: group.abstain                                   (bob r2)
//        -> group.completed {rounds: 2, termination: unanimous abstention}
//   S4 (ordinary session, no speakers):
//     13: text "Hello from a normal session."
//   S5 (speakers alice,bob; budget 1) - queued deliver-now (zap) mid-turn:
//     14: text "Launch one: scope freeze."                (alice r1, delayed)
//     15: text "Steered: budget noted. Continuing."       (steer continuation)
//     16: text "Side comment."                            (bob r1)
//        -> group.completed {rounds: 1, termination: budget}
//   S6 (speakers alice,bob; per-session override 1, then cleared):
//     17: text "Quick answer one."                        (alice r1)
//     18: group.abstain                                   (bob r1)
//        -> group.completed {rounds: 1, termination: budget}
//     19: text "Pass answer two."                         (alice r1)
//     20: group.abstain                                   (bob r1)
//     21: group.abstain                                   (alice r2)
//     22: group.abstain                                   (bob r2)
//        -> group.completed {rounds: 2, termination: unanimous abstention}
//
//   GET  /requests             -> the recorded request bodies, so the test
//     can assert which requests carried the group.abstain tool declaration
//     and the "Name: " attribution in the committed history.
//
// Usage: MOCK_PORT=9065 node harness/tests/mock_group_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9065);
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
        id: `mock-group-${completions}`,
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

      const streamBody = () => {
      switch (completions) {
        case 1: textCall("I think we should ship it this week."); break;
        case 2: abstainCall(); break;
        case 3: abstainCall(); break;
        case 4: abstainCall(); break;
        case 5: textCall("Round one point A."); break;
        case 6: textCall("Round one point B."); break;
        case 7: textCall("@Carol please draft the rollback plan."); break; // alice r1, nudge
        case 8: textCall("I will own the rollback plan."); break; // carol r1 (pulled)
        case 9: textCall("Launch plan: freeze scope on Monday."); break; // bob r1
        case 10: abstainCall(); break; // carol r2
        case 11: abstainCall(); break; // alice r2
        case 12: abstainCall(); break; // bob r2
        case 13: textCall("Hello from a normal session."); break;
        case 14: textCall("Launch one: scope freeze."); break; // S5 alice r1 (delayed below)
        case 15: {
          // S5: either the deliver-now continuation of alice's turn (its
          // close carries the promoted steer) or bob's own turn (the zap
          // missed the boundary window and the daemon transferred the
          // delivery to the next turn) - the steer detection decides.
          const last = requests[requests.length - 1];
          const userText = ((last && last.messages) || [])
            .map((m) => String(m.content || "")).join("\n");
          if (userText.includes("remember the budget")) {
            textCall("Steered: budget noted. Continuing.");
          } else {
            abstainCall();
          }
          break;
        }
        case 16: textCall("Side comment."); break; // S5 bob r1
        case 17: textCall("Quick answer one."); break; // S6 run1 alice r1
        case 18: abstainCall(); break; // S6 run1 bob r1
        case 19: textCall("Pass answer two."); break; // S6 run2 alice r1
        case 20: abstainCall(); break; // S6 run2 bob r1
        case 21: abstainCall(); break; // S6 run2 alice r2
        case 22: abstainCall(); break; // S6 run2 bob r2 -> unanimous
        default: {
          // The deliver-now continuation detection, kept for robustness.
          const last = requests[requests.length - 1];
          const userText = ((last && last.messages) || [])
            .map((m) => String(m.content || "")).join("\n");
          if (userText.includes("remember the budget")) {
            textCall("Steered: budget noted. Continuing.");
          } else {
            textCall("done");
          }
          break;
        }
      }
      // Every path (delayed or not) closes the stream the same way.
      res.write("data: [DONE]\n\n");
      res.end();
      };
      // S5 steer race window: the test queues + zaps while completion 14 (a
      // plain text turn) is pending; a scheduled stream start gives it 1.5s
      // of determinism without sleeping on every turn.
      if (completions === 14) {
        setTimeout(streamBody, 1500);
        return;
      }
      streamBody();
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock group provider listening on 127.0.0.1:${port}`);
});
