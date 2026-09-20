import { expect, test } from "@playwright/test";
import { signIn } from "./helpers";

/**
 * Starting a composition.
 *
 * The gallery used to carry a title field and a Create button, which made one
 * layout — the starter — the only way in, and made "what will this look like"
 * something you found out after creating it. There are five ways in now, and
 * the thing that makes them worth having is that each thumbnail is the actual
 * renderer running over the actual document the button would create, at the
 * panel's four pigments. So this spec is mostly about that: the pictures are
 * real, and what you pick is what you get.
 */

test.beforeEach(async ({ page }) => {
  await signIn(page);
});

test("the picker offers ready-made layouts, each drawn by the renderer", async ({
  page,
}, info) => {
  await page.goto("/dashboards");
  await page.getByTestId("new-composition").click();

  const picker = page.getByTestId("template-picker");
  await expect(picker).toBeVisible();
  await expect(picker).toContainText("Everything stays editable");

  // The expressive starters, plus Blank. Each thumbnail is the real renderer
  // over the real document the button would create.
  const keys = [
    "fridge",
    "editorial",
    "weatherPoster",
    "skyAgenda",
    "photoDay",
    "blank",
  ];
  for (const key of keys) {
    await expect(page.getByTestId(`template-${key}`)).toBeVisible();
  }

  /*
   * Every thumbnail is a canvas that has actually painted something — except
   * Blank, which is an empty 400 x 300 and paints nothing by definition. A
   * hand-drawn approximation would drift from the template the first time a
   * module's defaults changed and nothing would catch it; this is what catches
   * it.
   */
  const inked = await page.evaluate((templateKeys: string[]) => {
    return templateKeys.map((key) => {
      const canvas = document
        .querySelector(`[data-testid="template-${key}"]`)
        ?.querySelector("canvas") as HTMLCanvasElement | null;
      if (!canvas) return -1;
      if (canvas.width !== 400 || canvas.height !== 300) return -2;
      const context = canvas.getContext("2d");
      if (!context) return -1;
      const { data } = context.getImageData(0, 0, canvas.width, canvas.height);
      let nonWhite = 0;
      for (let i = 0; i < data.length; i += 4) {
        if (data[i] !== 255 || data[i + 1] !== 255 || data[i + 2] !== 255) {
          nonWhite += 1;
        }
      }
      return nonWhite;
    });
  }, keys);

  for (let i = 0; i < keys.length; i += 1) {
    const key = keys[i] as string;
    const count = inked[i] ?? -1;
    expect(count, `${key} has no 400x300 canvas`).toBeGreaterThanOrEqual(0);
    if (key === "blank") {
      expect(count, "Blank drew something").toBe(0);
    } else {
      expect(count, `${key} drew nothing`).toBeGreaterThan(100);
    }
  }

  await page.screenshot({
    path: `test-results/qa/${info.project.name}-templates.png`,
  });
});

test("choosing one creates it, saves it, and opens the editor", async ({ page }) => {
  await page.goto("/dashboards");
  await page.getByTestId("new-composition").click();
  await page.getByTestId("template-editorial").click();

  await page.waitForURL(/\/dashboards\/d_[a-z0-9]+\/edit$/);
  await expect(page.getByTestId("designer-title")).toHaveText("Editorial note");
  await expect(page.getByTestId("toast")).toContainText("Started from");

  // The layout is real modules, not a picture of modules, and the editor knows
  // their names because they came out of the same registry the gallery does.
  await expect(page.getByTestId("module-headline")).toBeVisible();
  await expect(page.getByTestId("module-message")).toBeVisible();
  await expect(page.getByTestId("module-timestamp")).toBeVisible();

  // Saved, so there is nothing hanging unsaved from a button press, and the
  // empty canvas it began from is still in the history rather than tidied
  // away: this product never rewrites a version list.
  await expect(page.getByTestId("version-toggle")).toContainText("saved");
  await page.getByTestId("version-toggle").click();
  await expect(page.getByTestId("version-rail")).toContainText("v1");
  await expect(page.getByTestId("version-rail")).toContainText("v2");
});

test("every template produces a layout the validator accepts", async ({ page }) => {
  /*
   * The layouts are built in the browser against the module registry's own
   * span limits, and the server validates them again on the way in. A template
   * that ignored a limit — a full-panel message, say, when `message` caps at
   * 8x2 — would be refused by exactly the validator that refuses a bad drag.
   * Creating all five is the cheapest way to assert that none of them does.
   */
  for (const key of [
    "fridge",
    "editorial",
    "weatherPoster",
    "skyAgenda",
    "photoDay",
    "blank",
  ]) {
    await page.goto("/dashboards");
    await page.getByTestId("new-composition").click();
    await page.getByTestId(`template-${key}`).click();
    await page.waitForURL(/\/dashboards\/d_[a-z0-9]+\/edit$/);
    // No layout problems, and the preview rendered rather than erroring.
    await expect(page.getByTestId("layout-problems")).toHaveCount(0);
    await expect(page.getByTestId("designer-preview")).toBeVisible();
  }
});
