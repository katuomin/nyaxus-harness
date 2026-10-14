// Regression test for the composer's vision capability resolution.
//
// The daemon accepts an image when the profile's vision tristate resolves
// to enabled (explicit true, or unset + connection vision). The composer
// gate must mirror that exactly: before the fix, an unset profile was
// treated as "text only" even when its connection had vision enabled.
import { profileVisionEnabled, type HarnessProfile, type HarnessProvider } from "../src/harness";

function assert(condition: boolean, message: string): void {
  if (!condition) throw new Error(`vision_test failed: ${message}`);
}

const provider = (name: string, params: Record<string, string | number | boolean>): HarnessProvider => ({
  name,
  endpoint: `http://127.0.0.1:9${name}/v1`,
  model: "m",
  apiKey: "",
  params,
  createdAt: "t",
  updatedAt: "t",
});

const profile = (patch: Partial<HarnessProfile> & { vision: boolean | null }): HarnessProfile => ({
  id: "p",
  name: "p",
  vision: patch.vision,
  ...patch,
});

const visionOn = provider("on", { vision: true });
const visionOff = provider("off", { vision: false });
const noVision = provider("bare", {});

// Explicit profile settings always win.
assert(profileVisionEnabled(profile({ vision: true }), [visionOff]) === true, "explicit on beats connection off");
assert(profileVisionEnabled(profile({ vision: false }), [visionOn]) === false, "explicit off beats connection on");

// Unset inherits the resolved connection's capability.
assert(profileVisionEnabled(profile({ vision: null, provider: "on" }), [visionOn]) === true, "unset inherits connection on");
assert(profileVisionEnabled(profile({ vision: null, provider: "off" }), [visionOff]) === false, "unset inherits connection off");
assert(profileVisionEnabled(profile({ vision: null, provider: "bare" }), [noVision]) === false, "unset + no vision param -> off");

// Unknown or missing connection resolves to the default provider.
assert(profileVisionEnabled(profile({ vision: null, provider: "ghost" }), [visionOn]) === false, "unknown connection -> off");
assert(profileVisionEnabled(profile({ vision: null }), [visionOn]) === false, "no connection, no primary -> off");
assert(profileVisionEnabled(undefined, [visionOn]) === false, "no profile -> off");

// A connection-less profile inherits the primary connection's capability.
assert(profileVisionEnabled(profile({ vision: null }), [visionOn], "on") === true, "unset + primary on -> on");
assert(profileVisionEnabled(profile({ vision: null }), [visionOff], "off") === false, "unset + primary off -> off");
assert(profileVisionEnabled(profile({ vision: null }), [noVision], "bare") === false, "unset + primary no vision -> off");
// An explicit profile connection still wins over the primary.
assert(profileVisionEnabled(profile({ vision: null, provider: "on" }), [visionOn, visionOff], "off") === true, "profile connection beats primary");
// An explicit profile setting always wins, even over the primary.
assert(profileVisionEnabled(profile({ vision: false }), [visionOn], "on") === false, "explicit off beats primary on");

console.log("vision_test passed");
