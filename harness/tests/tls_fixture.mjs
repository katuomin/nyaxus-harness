// Local TLS fixture for the fetch-path tests: serves one fixed page over
// https with its own self-signed CA (cert passed as the daemon's
// web.ca_bundle / probe's ca argument). Node's stock https module.
// Usage: node tls_fixture.mjs <port> <cert.pem> <key.pem>
import https from "node:https";
import { readFileSync } from "node:fs";

const port = Number(process.argv[2] || 9443);
const cert = readFileSync(process.argv[3] || "ca.pem");
const key = readFileSync(process.argv[4] || "key.pem");

const page = `<!doctype html><html><head><title>nyaxus tls fixture</title></head>
<body><h1>nyaxus tls fixture</h1><p>bold text</p></body></html>`;

https
    .createServer({ cert, key }, (req, res) => {
        res.writeHead(200, { "Content-Type": "text/html" });
        res.end(page);
    })
    .listen(port, "127.0.0.1", () => console.log(`fixture listening on 127.0.0.1:${port}`));
