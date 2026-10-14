// Mock web target for the web research e2e test.
//
// Serves:
//   GET /page.html     -> a small HTML page (title, script/style to strip,
//                         entities to decode, block structure)
//   GET /chunked.html  -> the same page with chunked transfer encoding
//   GET /redirect.html -> 302 to /page.html
//   GET /missing.html  -> 404
//   GET /search        -> SearXNG-shaped JSON (results point at /page.html);
//                         q=firecrawl fallback query -> empty results (to
//                         exercise the daemon's Firecrawl search fallback)
//   POST /v1/scrape    -> Firecrawl-shaped scrape response (markdown)
//   POST /v1/search    -> Firecrawl-shaped search response
//
// Usage: MOCK_PORT=9021 node harness/tests/mock_web_server.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9021);
const origin = `http://127.0.0.1:${port}`;

const PAGE = [
  "<!DOCTYPE html>",
  "<html><head>",
  "<title>Web Research Test Page</title>",
  "<style>body { color: red; }</style>",
  "</head><body>",
  "<h1>Hello from the mock web</h1>",
  "<script>var secret = 'should-not-appear';</script>",
  "<p>First paragraph with an entity: &amp;lt;quoted&amp;gt; text.</p>",
  "<ul><li>item one</li><li>item two</li></ul>",
  "<p>Second paragraph ends the page.</p>",
  "</body></html>",
].join("\n");

// The read-budget pager's fixture: a page whose extracted text is exactly
// 24,000 characters, one section marker per 1,000 characters, so page 1
// (the default read budget) carries the SECTION-1..12 range and page 2 the
// SECTION-13..24 range. The exact size makes page 2 the final page (no
// continuation note) while page 1 names one more page.
const BIG_PAGE = (() => {
  const parts = [
    "<!DOCTYPE html>",
    "<html><head><title>Big Paging Test</title></head><body>",
  ];
  let used = 0;
  let section = 0;
  while (used < 24000) {
    section += 1;
    const marker = `<p>SECTION-${section} starts the read-budget page.`;
    const filler = " lorem".repeat(Math.ceil(1000 / 6));
    parts.push(marker + filler + "</p>");
    used += marker.length + filler.length;
  }
  parts.push("</body></html>");
  return parts.join("\n");
})();

const SEARCH = JSON.stringify({
  query: "web research test page",
  number_of_results: 1,
  results: [
    {
      title: "Web Research Test Page",
      url: `${origin}/page.html`,
      content: "A mock page served by the web research test.",
    },
  ],
  answers: [],
  infoboxes: [],
  unresponsive_engines: [],
});

// SearXNG-shaped JSON whose body overruns a single 8 KiB read: served with
// an explicit content-length and header + body in one segment (as a LAN
// instance does), the daemon's client used to append the first chunk twice
// and truncate the tail of the document. The final result is the marker.
const BIG_SEARCH = (() => {
  const results = [];
  for (let i = 1; i <= 20; i += 1) {
    results.push({
      title: `Big search result ${i}`,
      url: `${origin}/bigsearch/${i}`,
      content:
        `Snippet ${i} of the big search fixture. ` +
        "filler-text ".repeat(60).trim(),
    });
  }
  return JSON.stringify({
    query: "big search test",
    number_of_results: 20,
    results,
    answers: [],
    infoboxes: [],
    unresponsive_engines: [],
  });
})();

const server = http.createServer((req, res) => {
  const url = new URL(req.url, origin);

  if (req.method === "GET" && url.pathname === "/page.html") {
    const payload = PAGE;
    res.writeHead(200, {
      "content-type": "text/html; charset=utf-8",
      "content-length": Buffer.byteLength(payload),
    });
    res.end(payload);
    return;
  }

  if (req.method === "GET" && url.pathname === "/big.html") {
    const payload = BIG_PAGE;
    res.writeHead(200, {
      "content-type": "text/html; charset=utf-8",
      "content-length": Buffer.byteLength(payload),
    });
    res.end(payload);
    return;
  }

  if (req.method === "GET" && url.pathname === "/chunked.html") {
    res.writeHead(200, {
      "content-type": "text/html; charset=utf-8",
      "transfer-encoding": "chunked",
    });
    // Two chunks + the terminating zero chunk.
    const half = Buffer.byteLength(PAGE) >> 1;
    const a = PAGE.slice(0, half);
    const b = PAGE.slice(half);
    res.write(`${Buffer.byteLength(a).toString(16)}\r\n${a}\r\n`);
    res.write(`${Buffer.byteLength(b).toString(16)}\r\n${b}\r\n`);
    res.write("0\r\n\r\n");
    res.end();
    return;
  }

  if (req.method === "GET" && url.pathname === "/redirect.html") {
    res.writeHead(302, { location: "/page.html", "content-length": 0 });
    res.end();
    return;
  }

  if (req.method === "GET" && url.pathname === "/missing.html") {
    const payload = "<html><head><title>Missing</title></head><body>nope</body></html>";
    res.writeHead(404, {
      "content-type": "text/html",
      "content-length": Buffer.byteLength(payload),
    });
    res.end(payload);
    return;
  }

  if (req.method === "GET" && url.pathname === "/search") {
    // A specific query returns no SearXNG results, so the daemon falls back
    // to the Firecrawl /v1/search endpoint (also served by this mock).
    const q = url.searchParams.get("q") || "";
    const payload =
      q === "firecrawl fallback query"
        ? JSON.stringify({ query: q, number_of_results: 0, results: [], answers: [], infoboxes: [], unresponsive_engines: [] })
        : q === "big search test"
          ? BIG_SEARCH
          : SEARCH;
    res.writeHead(200, {
      "content-type": "application/json",
      "content-length": Buffer.byteLength(payload),
    });
    res.end(payload);
    return;
  }

  if (req.method === "POST" && url.pathname === "/v1/scrape") {
    const payload = JSON.stringify({
      success: true,
      data: {
        markdown: "Firecrawl scraped page",
        metadata: { title: "Firecrawl Page", statusCode: 200, sourceURL: "https://example.com/firecrawl-page" },
      },
    });
    res.writeHead(200, {
      "content-type": "application/json",
      "content-length": Buffer.byteLength(payload),
    });
    res.end(payload);
    return;
  }

  if (req.method === "POST" && url.pathname === "/v1/search") {
    const payload = JSON.stringify({
      success: true,
      data: [
        {
          url: `${origin}/page.html`,
          title: "Firecrawl Result",
          description: "from firecrawl",
        },
      ],
    });
    res.writeHead(200, {
      "content-type": "application/json",
      "content-length": Buffer.byteLength(payload),
    });
    res.end(payload);
    return;
  }

  res.writeHead(404, { "content-type": "application/json", "content-length": 16 });
  res.end('{"error":"nope"}');
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock web server listening on 127.0.0.1:${port}`);
});
