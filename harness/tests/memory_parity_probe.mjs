// memory_parity_probe.mjs - recall-parity comparison: the SAME query is sent
// to the real Hindsight bank and to the Nyaxus custom provider (which holds
// the imported copy of that bank), and the ranked results are compared.
//
// Targets are sampled from the local memory.db (active units, type-diverse)
// and the query is the unit's own text (verbatim self-recall), so both
// systems have ground truth to find. Parity = the custom provider's hit@1
// and hit@5 are at least as good as Hindsight's.
//
// Targets are sampled through the daemon's bank browse route (type-diverse,
// active units), so this probe needs only fetch - no local DB access.
//
// Env: PARITY_HINDSIGHT (endpoint), PARITY_BANK (bank id),
//      PARITY_DAEMON (daemon api base), PARITY_SAMPLES (default 12).
const HINDSIGHT = process.env.PARITY_HINDSIGHT;
const BANK = process.env.PARITY_BANK || "nyaxus";
const DAEMON = process.env.PARITY_DAEMON;
const SAMPLES = Number(process.env.PARITY_SAMPLES || 12);
const TOP_K = 5;

if (!HINDSIGHT || !DAEMON) {
  console.error("PARITY_HINDSIGHT and PARITY_DAEMON are required");
  process.exit(2);
}

const norm = (s) => (s || "").replace(/\s+/g, " ").trim().toLowerCase();
const fingerprint = (s) => norm(s).slice(0, 80);

async function getJson(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url} -> HTTP ${res.status}`);
  return res.json();
}

async function pick(type, k) {
  const page = await getJson(
    `${DAEMON}/memory/bank?type=${encodeURIComponent(type)}&limit=${k}`,
  );
  return (page.units || []).map((u) => u.text);
}

const targets = [
  ...(await pick("world", Math.ceil(SAMPLES * 0.5))),
  ...(await pick("observation", Math.ceil(SAMPLES * 0.35))),
  ...(await pick("experience", SAMPLES - Math.ceil(SAMPLES * 0.5) - Math.ceil(SAMPLES * 0.35))),
]
  .filter((t) => norm(t).length >= 40)
  .slice(0, SAMPLES);

if (targets.length < 3) {
  console.error(`only ${targets.length} sampleable units found (need >= 3)`);
  process.exit(2);
}

async function post(url, body, timeoutMs = 60000) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const res = await fetch(url, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(body),
      signal: controller.signal,
    });
    const text = await res.text();
    if (!res.ok) throw new Error(`${url} -> HTTP ${res.status}: ${text.slice(0, 200)}`);
    return JSON.parse(text);
  } finally {
    clearTimeout(timer);
  }
}

// First rank (1-based) at which the target itself appears, or 0 if absent.
function hitRank(texts, target) {
  const fp = fingerprint(target);
  for (let i = 0; i < texts.length; i++) {
    const t = fingerprint(texts[i]);
    if (t && (fp.includes(t) || t.includes(fp))) return i + 1;
  }
  return 0;
}

const jaccard = (a, b) => {
  const sa = new Set(a.map(fingerprint));
  const sb = new Set(b.map(fingerprint));
  const inter = [...sa].filter((x) => sb.has(x)).length;
  const union = new Set([...sa, ...sb]).size;
  return union === 0 ? 1 : inter / union;
};

console.log(
  `== recall parity: ${targets.length} verbatim self-recall queries, top-${TOP_K} ==`,
);
console.log(
  `hindsight: ${HINDSIGHT} bank=${BANK}\ncustom:    ${DAEMON}`,
);

// Hindsight's server-side embedding backend can be down while the API is up
// (recall then 500s). Collect what each side returns independently; the
// parity verdict only applies when both sides answered every query.
let hsHit1 = 0, cuHit1 = 0, hsHitK = 0, cuHitK = 0, jaccSum = 0;
let hsOk = 0, cuOk = 0, failed = false;
const hsErrors = [];

for (let i = 0; i < targets.length; i++) {
  const target = targets[i];
  const query = target.slice(0, 500);

  const hsStarted = Date.now();
  let hsTexts = null;
  try {
    const hs = await post(
      `${HINDSIGHT}/v1/default/banks/${encodeURIComponent(BANK)}/memories/recall`,
      // Hindsight 0.9.1 takes a named budget (low|mid|high), not a number.
      { query, budget: "mid" },
    );
    hsTexts = (hs.results || []).slice(0, TOP_K).map((r) => r.text);
    hsOk++;
  } catch (e) {
    if (hsErrors.length < 3) hsErrors.push(e.message);
    // One more attempt: the first recall after idle can fail while the
    // server's embedding backend warms up.
    try {
      const retry = await post(
        `${HINDSIGHT}/v1/default/banks/${encodeURIComponent(BANK)}/memories/recall`,
        { query, budget: "mid" },
      );
      hsTexts = (retry.results || []).slice(0, TOP_K).map((r) => r.text);
      hsOk++;
    } catch (e2) {
      if (hsErrors.length < 3) hsErrors.push(`retry: ${e2.message}`);
    }
  }
  const hsMs = Date.now() - hsStarted;

  const cuStarted = Date.now();
  let cuTexts = [], cuSource = "", cuFallback = false;
  try {
    const cu = await post(`${DAEMON}/memory/recall`, { query });
    cuSource = cu.source || "";
    cuFallback = Boolean(cu.fallback);
    cuTexts = (cu.results || []).slice(0, TOP_K).map((r) => r.text);
    if (cuSource === "local" || cuFallback) {
      console.log(`[${i + 1}] FAIL: custom recall fell back to local (${cuSource})`);
      failed = true;
      continue;
    }
    cuOk++;
  } catch (e) {
    console.log(`[${i + 1}] custom error: ${e.message}`);
    failed = true;
    continue;
  }
  const cuMs = Date.now() - cuStarted;

  const cuRank = hitRank(cuTexts, target);
  const cu1 = cuRank === 1 ? 1 : 0;
  const cuK = cuRank > 0 && cuRank <= TOP_K ? 1 : 0;
  cuHit1 += cu1; cuHitK += cuK;

  if (hsTexts === null) {
    console.log(`[${i + 1}] custom hit@1=${cu1} hit@${TOP_K}=${cuK} (${cuMs}ms) | hindsight: unavailable`);
    continue;
  }

  const hsRank = hitRank(hsTexts, target);
  const hs1 = hsRank === 1 ? 1 : 0;
  const hsK = hsRank > 0 && hsRank <= TOP_K ? 1 : 0;
  hsHit1 += hs1; hsHitK += hsK;
  jaccSum += jaccard(hsTexts, cuTexts);

  const mark = (a, b) => (a === b ? " " : "*");
  console.log(
    `[${i + 1}] hit@1 hs=${hs1}${mark(hs1, cu1)} cu=${cu1} | ` +
      `hit@${TOP_K} hs=${hsK} cu=${cuK} | jaccard=${jaccard(hsTexts, cuTexts).toFixed(2)} | ` +
      `${hsMs}ms vs ${cuMs}ms`,
  );
}

const n = targets.length;
console.log(
  `\ncustom self-recall: hit@1=${cuHit1}/${n} hit@${TOP_K}=${cuHitK}/${n}`,
);

if (hsOk < n) {
  console.log(
    `\nBLOCKED: Hindsight answered ${hsOk}/${n} queries ` +
      `(its embedding backend appears down). Last error: ${hsErrors[0] || "n/a"}`,
  );
  if (failed) process.exit(1);
  process.exit(3);
}

console.log(
  `parity: hit@1 hindsight=${hsHit1}/${n} custom=${cuHit1}/${n} | ` +
    `hit@${TOP_K} hindsight=${hsHitK}/${n} custom=${cuHitK}/${n} | ` +
    `avg top-${TOP_K} jaccard=${(jaccSum / n).toFixed(3)}`,
);

if (cuHit1 < hsHit1 || cuHitK < hsHitK) {
  console.log("FAIL: custom provider is behind Hindsight on recall parity");
  process.exit(1);
}
if (failed) {
  console.log("FAIL: one or more queries errored");
  process.exit(1);
}
console.log("PARITY_OK: custom recall is at least as good as Hindsight");
