// Replay the daemon's exact raw HTTP request against Hindsight and time it.
// Manual diagnostic: node harness/tests/raw_replay.mjs BANK=<bank>
import net from "node:net";

const host = "<deployment-lan>";
const port = 8888;
const bank = process.env.BANK;
const body = JSON.stringify({
  async: false,
  items: [
    {
      content: "User: the vault passphrase is copper-meridian-7\n\nAssistant: noted",
      context: "Nyaxus session probe (profile orchestrator): completed turn",
      document_id: "nyaxus-turn:probe-raw",
      tags: ["nyaxus.session:probe-raw", "nyaxus.profile:orchestrator", "nyaxus.run:probe-raw"],
    },
  ],
});

const request =
  `POST /v1/default/banks/${bank}/memories HTTP/1.1\r\n` +
  `Host: ${host}\r\n` +
  `Connection: close\r\n` +
  `content-type: application/json\r\n` +
  `Content-Length: ${Buffer.byteLength(body)}\r\n` +
  `\r\n` +
  body;

const t0 = Date.now();
const log = (msg) => console.log(`+${((Date.now() - t0) / 1000).toFixed(1)}s ${msg}`);

const sock = net.connect(port, host, () => {
  log("connected, sending");
  sock.write(request);
});
let buf = "";
let headersSeen = false;
sock.on("data", (d) => {
  buf += d.toString();
  if (!headersSeen && buf.includes("\r\n\r\n")) {
    headersSeen = true;
    log("response headers received: " + buf.slice(0, buf.indexOf("\r\n\r\n")).split("\r\n")[0]);
  }
});
sock.on("end", () => {
  log(`EOF, body bytes: ${buf.length}`);
  console.log(buf.slice(-300));
  sock.destroy();
  process.exit(0);
});
sock.on("error", (e) => {
  console.log("socket error:", e.message);
  process.exit(1);
});
setTimeout(() => {
  log("TIMEOUT (client gave up)");
  sock.destroy();
  process.exit(2);
}, 420000);
