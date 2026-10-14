// Regression test for the skills trash/restore + enable toggle, against the
// fixture client (offline, no daemon): delete moves a skill to the trash,
// restore brings it back, trash/delete removes it for good, and the enable
// toggle flips the per-skill flag that hides a skill from the model.
import { FixtureHarnessClient } from "../src/harness";

function fail(message: string): never {
  throw new Error(`skills_test failed: ${message}`);
}

const client = new FixtureHarnessClient();

// Baseline: the three fixture skills, all enabled by default.
{
  const skills = await client.listSkills();
  if (skills.length !== 3) fail(`expected 3 fixture skills, got ${skills.length}`);
  for (const skill of skills) {
    if (skill.enabled !== true) fail(`fixture skill ${skill.name} should be enabled by default`);
  }
  const trash = await client.listSkillTrash();
  if (trash.length !== 0) fail(`trash should start empty, got ${trash.length}`);
}

// Enable toggle flips one skill and leaves the others untouched.
{
  const skills = await client.setSkillEnabled("code-review", false);
  const target = skills.find((skill) => skill.name === "code-review");
  if (!target || target.enabled !== false) fail("code-review should be disabled after the toggle");
  const other = skills.find((skill) => skill.name === "web-research");
  if (!other || other.enabled !== true) fail("web-research should stay enabled");
  // Toggling back restores enabled.
  const back = await client.setSkillEnabled("code-review", true);
  if (back.find((skill) => skill.name === "code-review")?.enabled !== true) {
    fail("code-review should be enabled again after the inverse toggle");
  }
}

// Delete = trash: gone from the list, present in the trash.
{
  const skills = await client.deleteSkill("release-notes");
  if (skills.some((skill) => skill.name === "release-notes")) {
    fail("release-notes should be gone from the list after delete");
  }
  const trash = await client.listSkillTrash();
  if (trash.length !== 1 || trash[0].name !== "release-notes") {
    fail(`trash should hold release-notes: ${JSON.stringify(trash)}`);
  }
}

// Restore = back in the list, trash empty again.
{
  const skills = await client.restoreSkill("release-notes");
  if (!skills.some((skill) => skill.name === "release-notes")) {
    fail("release-notes should be back in the list after restore");
  }
  const trash = await client.listSkillTrash();
  if (trash.length !== 0) fail("trash should be empty after restore");
}

// Trash delete = permanently gone.
{
  await client.deleteSkill("release-notes");
  const trash = await client.deleteSkillTrash("release-notes");
  if (trash.length !== 0) fail("trash should be empty after permanent delete");
  const skills = await client.listSkills();
  if (skills.some((skill) => skill.name === "release-notes")) {
    fail("release-notes should not be resurrected");
  }
}

// saveSkill creates an enabled skill.
{
  const skills = await client.saveSkill({
    name: "fresh-skill",
    description: "a new one",
    content: "body",
    disableModelInvocation: false,
  });
  const created = skills.find((skill) => skill.name === "fresh-skill");
  if (!created || created.enabled !== true) {
    fail("a saved skill should be enabled by default");
  }
}

console.log("skills_test passed");
