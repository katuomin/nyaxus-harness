// Regression test for the inspector's client surface (offline, fixture
// client): the infra snapshot shape the observatory renders (host + gpu +
// containers + providers, all sections present with stable field names) and
// the web-sources ledger contract (empty in fixture mode = the UI's
// "no web fetches" state, never a throw).
import { FixtureHarnessClient } from "../src/harness";

function fail(message: string): never {
  throw new Error(`inspector_test failed: ${message}`);
}

const client = new FixtureHarnessClient();

// Infra snapshot: every section the observatory renders must be present with
// the exact field names the view reads.
{
  const report = await client.getInfra();
  if (!report.host || typeof report.host.available !== "boolean") {
    fail("infra host.available must be a boolean");
  }
  if (!Number.isFinite(report.host.memTotalMb) || report.host.memTotalMb <= 0) {
    fail("infra host.memTotalMb must be a positive number");
  }
  const hostMetrics = [report.host.memAvailableMb, report.host.swapTotalMb, report.host.swapFreeMb, report.host.load1, report.host.load5, report.host.load15, report.host.cores];
  if (!hostMetrics.every((value) => Number.isFinite(value))) {
    fail("infra host metrics must all be finite numbers");
  }
  if (!report.gpu || typeof report.gpu.available !== "boolean" || !Array.isArray(report.gpu.devices)) {
    fail("infra gpu must carry available + devices[]");
  }
  for (const device of report.gpu.devices) {
    if (!Number.isFinite(device.vramTotalMb) || !Number.isFinite(device.vramUsedMb) || !Number.isFinite(device.utilPct)) {
      fail(`gpu device ${device.name} metrics must be finite numbers`);
    }
  }
  if (typeof report.dockerAvailable !== "boolean" || !Array.isArray(report.containers) || !Array.isArray(report.providers)) {
    fail("infra dockerAvailable/containers/providers must be present");
  }
  for (const provider of report.providers) {
    if (typeof provider.online !== "boolean" || !Number.isFinite(provider.latencyMs) || !provider.endpoint) {
      fail(`provider ${provider.name} must carry endpoint + online + latencyMs`);
    }
  }
  if (!report.providers.some((provider) => provider.name === "primary")) {
    fail("fixture infra must expose the primary provider led");
  }
  if (!Number.isFinite(report.collectedAtMs) || report.collectedAtMs <= 0) {
    fail("infra collectedAtMs must be a positive number");
  }
}

// Web-sources ledger: fixture mode has no fetches - the contract is an empty
// array (the UI's "no web fetches" state), not a throw.
{
  const sources = await client.listSources("fixture-session");
  if (!Array.isArray(sources) || sources.length !== 0) {
    fail(`fixture listSources must be an empty array, got ${JSON.stringify(sources)}`);
  }
}

// Session tags: the fixture client mirrors the daemon contract (replace the
// label set, session list reflects it, unknown session is a no-op).
{
  const session = (await client.listSessions())[0];
  if (!session) fail("fixture must expose at least one session");
  await client.setSessionTags(session.id, ["ops", "urgent"]);
  const after = (await client.listSessions()).find((item) => item.id === session.id);
  if (!after?.tags || after.tags.length !== 2 || after.tags[0] !== "ops" || after.tags[1] !== "urgent") {
    fail(`fixture setSessionTags must store the label set, got ${JSON.stringify(after?.tags)}`);
  }
  await client.setSessionTags("missing-session", ["ops"]);
  console.log("tags fixture round-trip ok");
}

// Journey excerpt: fixture mode serves a canned journey (the section's
// layout proof) and the refresh request resolves without throwing.
{
  const journey = await client.getJourney("fixture-session");
  if (!journey || !journey.text || journey.watermark <= 0) {
    fail(`fixture getJourney must return a journey with text + watermark, got ${JSON.stringify(journey)}`);
  }
  await client.requestJourneyUpdate("fixture-session");
  console.log("journey fixture ok");
}

console.log("inspector_test passed");
