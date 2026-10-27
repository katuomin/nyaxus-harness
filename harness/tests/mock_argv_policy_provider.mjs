// Mock provider for the read-only argv auto-approve e2e test.
//
// Behavior keys on the run's user message, so one mock process serves all
// scenarios. The daemon prefers a unix /bin/sh (git-bash on Windows), so
// every command is a POSIX command:
//   "auto approve"    -> terminal {"command":"git status"} (read-only) ->
//     the daemon must skip the gate entirely.
//   "gate write"      -> terminal {"command":"rm x.txt"} (write) -> the
//     daemon must publish approval.requested.
//   "redirect file"   -> terminal {"command":"ls > out.txt"} -> gated
//     (output redirect to a file).
//   "redirect nul"    -> terminal {"command":"ls > /dev/null"} ->
//     auto-approved (output redirect to the null device).
//   "sequence ok"     -> terminal {"command":"ls && git status"} ->
//     auto-approved (every stage read-only).
//   "sequence mixed"   -> terminal {"command":"ls && rm x"} -> gated (one
//     write stage).
//   "dynamic gates"   -> terminal {"command":"ls $HOME"} -> gated ($
//     expansion is dynamic).
// Risk-verb taxonomy negatives (audited external checklist) - every shape
// below must gate:
//   "date set"        -> date -s "next week"
//   "ip add"          -> ip addr add 10.0.0.1/24 dev eth0
//   "mount gate"      -> mount -t tmpfs tmpfs /mnt/x
//   "kill gate"       -> kill -9 1234
//   "dotnet install"  -> dotnet tool install --global mytool
//   "commit msg"      -> git commit -m fix
//   "sort out"        -> sort -o tmp.txt data.txt
//   "find follow"     -> find -L . -type f -name "*.txt"
//   "rg follow"       -> rg --follow pattern src/
//   "grep follow"     -> grep -R pattern .
// Link-following must not change plain non-following reads, so two
// positive controls stay auto-approved:
//   "recurse plain"   -> grep -r pattern .
//   "find plain"      -> find . -type f -name "*.txt"
// Command-data vs filesystem-operand separation: command data that looks
// like a write flag stays auto-approved, and the process-spawn data
// smuggles found by the audit must gate:
//   "data pattern"    -> grep -e -R pattern .    (auto: -e data)
//   "sort key"        -> sort -k2 data.txt       (auto: key-spec data)
//   "compress prog"   -> sort --compress-program=x data.txt (gate: exec)
//   "man pager"       -> man -P less gcc         (gate: pager exec)
//   "fd exec"         -> fd -x echo x            (gate: exec)
//   "git pager"       -> git grep -O pattern     (gate: pager exec)
//
// sed inline-script classification: provably pure stream editing stays
// auto-approved; script data that writes files, executes shell, or loads
// an unvouched script file must gate:
//   "sed auto"        -> sed 's/a/b/g'                  (auto)
//   "sed script"      -> sed -e 's/a/b/' -e 'y/ab/cd/'   (auto)
//   "sed write"       -> sed 's/a/b/w out.txt'           (gate: s w flag)
//   "sed exec"        -> sed 'e echo hi'                 (gate: shell exec)
//   "sed file"        -> sed -f script.sed               (gate: unvouched)
//   "sed inplace"     -> sed -i 's/a/b/' file.txt        (gate: in-place)
//
// Each scenario answers with a scenario-specific text on request n>=2 so the
// run cannot hang.
//
// Usage: MOCK_PORT=8929 node harness/tests/mock_argv_policy_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8929);
const delayMs = Number(process.env.MOCK_DELAY_MS || 50);

const perScenario = {};

// user message -> [first tool command, final text]
const scenarios = {
  "auto approve": ["git status", "auto approve done"],
  "gate write": ["rm x.txt", "gate write done"],
  "redirect file": ["ls > out.txt", "redirect file done"],
  "redirect nul": ["ls > /dev/null", "redirect nul done"],
  "sequence ok": ["ls && git status", "sequence ok done"],
  "sequence mixed": ["ls && rm x", "sequence mixed done"],
  "dynamic gates": ["ls $HOME", "dynamic gates done"],
  "date set": ["date -s \"next week\"", "date set done"],
  "ip add": ["ip addr add 10.0.0.1/24 dev eth0", "ip add done"],
  "mount gate": ["mount -t tmpfs tmpfs /mnt/x", "mount gate done"],
  "kill gate": ["kill -9 1234", "kill gate done"],
  "dotnet install": ["dotnet tool install --global mytool", "dotnet install done"],
  "commit msg": ["git commit -m fix", "commit msg done"],
  "sort out": ["sort -o tmp.txt data.txt", "sort out done"],
  "find follow": ["find -L . -type f -name \"*.txt\"", "find follow done"],
  "rg follow": ["rg --follow pattern src/", "rg follow done"],
  "grep follow": ["grep -R pattern .", "grep follow done"],
  "recurse plain": ["grep -r pattern .", "recurse plain done"],
  "find plain": ["find . -type f -name \"*.txt\"", "find plain done"],
  "data pattern": ["grep -e -R pattern .", "data pattern done"],
  "sort key": ["sort -k2 data.txt", "sort key done"],
  "compress prog": ["sort --compress-program=x data.txt", "compress prog done"],
  "man pager": ["man -P less gcc", "man pager done"],
  "fd exec": ["fd -x echo x", "fd exec done"],
  "git pager": ["git grep -O pattern", "git pager done"],
  "sed auto": ["sed 's/a/b/g'", "sed auto done"],
  "sed script": ["sed -e 's/a/b/' -e 'y/ab/cd/'", "sed script done"],
  "sed write": ["sed 's/a/b/w out.txt'", "sed write done"],
  "sed exec": ["sed 'e echo hi'", "sed exec done"],
  "sed file": ["sed -f script.sed", "sed file done"],
  "sed inplace": ["sed -i 's/a/b/' file.txt", "sed inplace done"],
};

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
      let userMsg = "";
      try {
        const messages = JSON.parse(body).messages || [];
        const lastUser = [...messages].reverse().find((m) => m.role === "user");
        userMsg = lastUser ? lastUser.content : "";
      } catch {}
      perScenario[userMsg] = (perScenario[userMsg] || 0) + 1;
      const n = perScenario[userMsg];
      setTimeout(() => {
        res.writeHead(200, {
          "content-type": "text/event-stream",
          "cache-control": "no-cache",
        });
        const chunk = (delta) => {
          res.write(`data: ${JSON.stringify({
            id: "mock-argv-policy",
            object: "chat.completion.chunk",
            choices: [{ index: 0, delta }],
          })}\n\n`);
        };
        const tool = (command) =>
          chunk({
            tool_calls: [{
              index: 0,
              id: `call-${n}`,
              type: "function",
              function: { name: "terminal", arguments: JSON.stringify({ command }) },
            }],
          });
        // Final usage chunk (OpenAI include_usage pattern: top-level usage,
        // empty choices) - the daemon's metrics.snapshot source.
        const usage = () =>
          res.write(`data: ${JSON.stringify({
            id: "mock-argv-policy",
            object: "chat.completion.chunk",
            choices: [],
            usage: { prompt_tokens: 10, completion_tokens: 5, total_tokens: 15 },
          })}\n\n`);
        let command = "";
        let finalText = "";
        for (const [key, [cmd, text]] of Object.entries(scenarios)) {
          if (userMsg.includes(key)) {
            command = cmd;
            finalText = text;
            break;
          }
        }
        if (n === 1 && command) {
          tool(command);
        } else {
          chunk({ content: finalText || "unrecognized scenario" });
        }
        usage();
        res.write("data: [DONE]\n\n");
        res.end();
      }, delayMs);
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock argv-policy provider listening on 127.0.0.1:${port} (delay=${delayMs}ms)`);
});
