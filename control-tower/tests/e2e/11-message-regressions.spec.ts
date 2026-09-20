import { expect, test } from "@playwright/test";
import {
  createEmptyDashboard,
  csrfFrom,
  openFineTune,
  previewPixels,
  shot,
  signIn,
  stablePixels,
} from "./helpers";

/**
 * The two things the Message got wrong, in a browser.
 *
 * LINE GAP. The report was "changing it does nothing". It is true in a tile
 * only tall enough for one line, and false everywhere else, and the fix was to
 * make the renderer say which. So one spec drives a tile that can hold two
 * lines and insists the pixels move, and another drives the tile the original
 * report came from and insists the inspector explains itself.
 *
 * EMOJI. They used to become a question mark with no trace. Now they are drawn
 * or, where the panel has no drawing, marked and named.
 */

const LONG =
  "Ceci est une note assez longue pour envelopper sur plusieurs lignes du panneau";

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

/**
 * A dashboard holding one Message, at a size of our choosing.
 *
 * Built through the API rather than by dragging the resize handle: a pointer
 * drag is covered by the designer spec, and repeating it here would make a
 * line-gap test fail for a reason that has nothing to do with line gaps.
 */
async function messageDashboard(
  page: import("@playwright/test").Page,
  name: string,
  span: { w: number; h: number },
): Promise<string> {
  const { id } = await createEmptyDashboard(page, name);
  const csrf = await csrfFrom(page);

  const current = await page.request.get(`/api/dashboards/${id}`);
  expect(current.status()).toBe(200);
  const { record } = (await current.json()) as {
    record: { doc: { modules: unknown[] } };
  };
  record.doc.modules = [
    {
      id: "m_message",
      type: "message",
      x: 0,
      y: 0,
      w: span.w,
      h: span.h,
      hidden: false,
      options: {},
    },
  ];

  const saved = await page.request.post(`/api/dashboards/${id}/versions`, {
    headers: { "content-type": "application/json", "x-csrf-token": csrf },
    data: { doc: record.doc, note: "One message, for the line gap specs" },
  });
  expect(saved.status(), await saved.text()).toBe(201);

  await page.goto(`/dashboards/${id}/edit`);
  await page.getByTestId("module-message").click();
  await expect(page.getByTestId("module-inspector")).toBeVisible();
  return id;
}

test("two line gaps lay a multiline message out differently", async ({
  page,
}, info) => {
  const id = await messageDashboard(page, `Line gap ${info.project.name}`, {
    w: 6,
    h: 2,
  });

  await page.getByTestId("opt-body").fill(LONG);
  await openFineTune(page);
  await page.getByTestId("opt-body-size").selectOption("13");
  await page.getByTestId("opt-body-lineSpacing").selectOption("0");
  await page.waitForTimeout(300);
  const tight = await previewPixels(page);

  // The control is not inert here, so nothing tells the user it is.
  await expect(page.getByTestId("opt-body-lineSpacing-inert")).toHaveCount(0);

  await page.getByTestId("opt-body-lineSpacing").selectOption("6");
  await page.waitForTimeout(300);
  const loose = await previewPixels(page);
  expect(loose, "the line gap did not move the pixels").not.toBe(tight);
  await shot(page, "message-line-gap", info.project.name);

  // And it survives a save and a reload, as the stored value and as pixels.
  await page.getByTestId("save-version").click();
  await expect(page.getByTestId("version-rail")).toContainText("v2");

  await page.goto(`/dashboards/${id}/edit`);
  await page.getByTestId("module-message").click();
  await openFineTune(page);
  await expect(page.getByTestId("opt-body-lineSpacing")).toHaveValue("6");
  expect(await stablePixels(page)).toBe(loose);

  // Setting it back is a real change too, in the other direction.
  await page.getByTestId("opt-body-lineSpacing").selectOption("0");
  await page.waitForTimeout(300);
  expect(await previewPixels(page)).toBe(tight);
});

test("a tile that fits one line says so rather than feeling broken", async ({
  page,
}, info) => {
  // Three cells by one: the tile the original report was made from.
  await messageDashboard(page, `Inert gap ${info.project.name}`, { w: 3, h: 1 });

  await page.getByTestId("opt-body").fill(LONG);
  await openFineTune(page);
  // 27 px in the starter's one-row Message: the configuration the original
  // report was made from.
  await page.getByTestId("opt-body-size").selectOption("27");
  await page.waitForTimeout(300);

  const note = page.getByTestId("opt-body-lineSpacing-inert");
  await expect(note).toBeVisible();
  await expect(note).toContainText("fits one line at 27 px");
  await shot(page, "message-line-gap-inert", info.project.name);

  // Which is the truth: at this size the control really cannot move anything.
  await page.getByTestId("opt-body-lineSpacing").selectOption("0");
  await page.waitForTimeout(300);
  const tight = await previewPixels(page);
  await page.getByTestId("opt-body-lineSpacing").selectOption("12");
  await page.waitForTimeout(300);
  expect(await previewPixels(page)).toBe(tight);

  // Smaller type gives the second line somewhere to go, and the note goes away.
  await page.getByTestId("opt-body-size").selectOption("13");
  await page.waitForTimeout(300);
  await expect(page.getByTestId("opt-body-lineSpacing-inert")).toHaveCount(0);
  expect(await previewPixels(page)).not.toBe(tight);
});

test("an emoji reaches the panel instead of disappearing", async ({
  page,
}, info) => {
  await messageDashboard(page, `Emoji ${info.project.name}`, { w: 6, h: 2 });

  await page.getByTestId("opt-body").fill("Collect the parcel");
  await page.waitForTimeout(300);
  const plain = await previewPixels(page);

  await page.getByTestId("opt-body").fill("Collect the parcel 🥚");
  await page.waitForTimeout(300);
  expect(await previewPixels(page), "the egg was dropped").not.toBe(plain);
  // A supported symbol is drawn, so there is nothing to warn about.
  await expect(page.getByTestId("opt-body-symbols-note")).toHaveCount(0);
  await shot(page, "message-emoji", info.project.name);

  // The whole vocabulary is one disclosure away, with its limits in words. It
  // lives in the Fine-tune fold now, beside the type controls, rather than on
  // the words role.
  await openFineTune(page);
  await page.getByTestId("opt-body-symbols").click();
  await expect(page.getByTestId("module-inspector")).toContainText(
    "not an emoji font",
  );
});

test("an emoji the panel cannot draw is kept visible and named", async ({
  page,
}, info) => {
  await messageDashboard(page, `Emoji gap ${info.project.name}`, { w: 6, h: 2 });

  await page.getByTestId("opt-body").fill("Fête 🦄 demain");
  await page.waitForTimeout(300);

  const note = page.getByTestId("opt-body-symbols-note");
  await expect(note).toBeVisible();
  await expect(note).toContainText("🦄");
  await expect(note).toContainText("rather than dropped");
  // It is a note, not a block.
  await expect(page.getByTestId("save-version")).toBeEnabled();
  await shot(page, "message-emoji-unsupported", info.project.name);

  // Something is drawn where it was: the panel is not the same as without it.
  const withUnicorn = await previewPixels(page);
  await page.getByTestId("opt-body").fill("Fête demain");
  await page.waitForTimeout(300);
  expect(await previewPixels(page)).not.toBe(withUnicorn);
  await expect(page.getByTestId("opt-body-symbols-note")).toHaveCount(0);
});
