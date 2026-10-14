// Probe: does the vite dev server close idle keep-alive connections after
// Node's default keepAliveTimeout (5s)? A browser reusing such a socket for
// its EventSource gets a failed first attempt, then waits its default
// reconnect interval (~5s) before trying again.
import net from "node:net";

const host = "127.0.0.1";
const port = Number(process.argv[3] || 4173);
const sid = process.argv[2] || "session-1788276027393827000";
const path = process.argv[4] || `/api/v1/sessions/${sid}`;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const socket = net.connect({ host, port });
let buffer = "";
let respondResolve;
const waiting = () => new Promise((r) => (respondResolve = r));

socket.on("data", (chunk) => {
  buffer += chunk.toString("latin1");
  // A complete HTTP/1.1 response ends with \r\n\r\n + content-length body.
  const head = buffer.split("\r\n\r\n");
  const headers = head[0];
  const cl = /content-length:\s*(\d+)/i.exec(headers);
  if (cl && buffer.length >= headers.length + 4 + Number(cl[1])) {
    const body = buffer.slice(headers.length + 4, headers.length + 4 + Number(cl[1]));
    buffer = buffer.slice(headers.length + 4 + Number(cl[1]));
    respondResolve?.({ status: /HTTP\/1\.1 (\d+)/.exec(headers)?.[1], body: body.slice(0, 80) });
    respondResolve = undefined;
  }
});
const closed = new Promise((r) => socket.on("close", () => r("socket closed by server")));
socket.on("error", (e) => console.log("socket error:", String(e)));

const request = () =>
  socket.write(
    `GET ${path} HTTP/1.1\r\nhost: ${host}:${port}\r\nconnection: keep-alive\r\n\r\n`,
  );

await new Promise((r) => socket.on("connect", r));

request();
const t0 = Date.now();
const first = await Promise.race([waiting(), closed.then((w) => ({ closed: w }))]);
console.log("request 1:", first, `(${Date.now() - t0}ms)`);

// Age the idle connection past Node's default keepAliveTimeout (5s).
await sleep(6500);

request();
const t1 = Date.now();
const second = await Promise.race([
  waiting(),
  closed.then((w) => ({ closed: w, t: Date.now() - t1 })),
  sleep(8000).then(() => ({ timeout: "no response within 8s" })),
]);
console.log("request 2 after 6.5s idle:", second, `(${Date.now() - t1}ms)`);

socket.destroy();
process.exit(0);
