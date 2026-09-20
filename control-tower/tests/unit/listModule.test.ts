import { describe, expect, it } from "vitest";
import { FrameBuffer, pack } from "@/core/frame";
import { BLACK, RED, WHITE } from "@/core/palette";
import { list, ListOptions, listedRows } from "@/core/render/modules/list";
import { moduleDefinition } from "@/core/render/modules";
import { cellsToPixels, type ModuleData } from "@/core/render/types";
import type { OverflowFact } from "@/core/render/text";
import { sha256HexSync } from "@/server/hash";
import { FIXTURE_CTX } from "./fixtures/render";

type Row = { text: string; visible?: boolean };

function render(
  options: Record<string, unknown>,
  span = { w: 4, h: 3 },
): { frame: FrameBuffer; overflows: OverflowFact[] } {
  const overflows: OverflowFact[] = [];
  const frame = new FrameBuffer(WHITE);
  list.render(
    frame,
    cellsToPixels(0, 0, span.w, span.h),
    { state: "ok" } as ModuleData<never>,
    ListOptions.parse(options),
    { ...FIXTURE_CTX, report: { overflow: (fact) => overflows.push(fact) } },
  );
  return { frame, overflows };
}

function digest(fb: FrameBuffer): string {
  return sha256HexSync(pack(fb));
}

function ink(fb: FrameBuffer, colour: number): number {
  let count = 0;
  for (const value of fb.pixels) if (value === colour) count += 1;
  return count;
}

const CHORES: Row[] = [
  { text: "Collect the parcel" },
  { text: "Take the bins out" },
  { text: "Water the plants" },
];

describe("list schema", () => {
  it("starts empty rather than with invented rows", () => {
    const defaults = ListOptions.parse({});
    expect(defaults.rows).toEqual([]);
    expect(defaults.marker).toBe("bullet");
    expect(defaults.maxVisibleRows).toBe(6);
  });

  it("keeps the order the rows were given in", () => {
    const options = ListOptions.parse({ rows: CHORES });
    expect(options.rows.map((row) => row.text)).toEqual([
      "Collect the parcel",
      "Take the bins out",
      "Water the plants",
    ]);
  });

  it("refuses more rows than it can hold", () => {
    const tooMany = Array.from({ length: 13 }, (_, i) => ({ text: `r${i}` }));
    expect(ListOptions.safeParse({ rows: tooMany }).success).toBe(false);
  });

  it("refuses a maximum of zero visible rows", () => {
    expect(ListOptions.safeParse({ maxVisibleRows: 0 }).success).toBe(false);
    expect(ListOptions.safeParse({ maxVisibleRows: 1 }).success).toBe(true);
  });

  it("only lists rows that are visible and have words", () => {
    const options = ListOptions.parse({
      rows: [
        { text: "Un" },
        { text: "Deux", visible: false },
        { text: "   " },
        { text: "Trois" },
      ],
    });
    expect(listedRows(options).map((row) => row.text)).toEqual(["Un", "Trois"]);
  });
});

describe("list rendering", () => {
  it("draws its empty state rather than nothing at all", () => {
    const { frame } = render({ rows: [] });
    expect(ink(frame, BLACK)).toBeGreaterThan(100);
    expect(digest(frame)).not.toBe(digest(new FrameBuffer(WHITE)));
  });

  it("an empty list is a fact, not an error, so it is not in the attention colour", () => {
    const { frame, overflows } = render({ rows: [] });
    expect(ink(frame, RED)).toBe(0);
    expect(overflows).toEqual([]);
  });

  it("draws every visible row and none of the hidden ones", () => {
    const shown = render({ rows: CHORES });
    const withHidden = render({
      rows: [...CHORES, { text: "Cachée", visible: false }],
    });
    expect(digest(withHidden.frame)).toBe(digest(shown.frame));
  });

  it("reordering the rows changes the panel", () => {
    const forward = render({ rows: CHORES });
    const backward = render({ rows: [...CHORES].reverse() });
    expect(digest(backward.frame)).not.toBe(digest(forward.frame));
  });

  it("removing a row changes the panel", () => {
    const three = render({ rows: CHORES });
    const two = render({ rows: CHORES.slice(0, 2) });
    expect(digest(two.frame)).not.toBe(digest(three.frame));
  });

  it("each marker mode is a different drawing", () => {
    const digests = new Set(
      (["bullet", "numbered", "checkbox"] as const).map(
        (marker) => digest(render({ rows: CHORES, marker }).frame),
      ),
    );
    expect(digests.size).toBe(3);
  });

  it("a checkbox is an empty box: nothing on the panel is ever ticked", () => {
    const { frame } = render({ rows: CHORES, marker: "checkbox" });
    // An outlined box has a white middle. A ticked box would not.
    const box = { x: 4, y: 4, w: 200, h: 150 };
    let interiorWhite = 0;
    for (let y = box.y; y < box.y + box.h; y += 1) {
      for (let x = box.x; x < box.x + box.w; x += 1) {
        if (frame.get(x, y) === WHITE) interiorWhite += 1;
      }
    }
    expect(interiorWhite).toBeGreaterThan(1000);
    expect(list.description).toMatch(/drawing only/);
  });

  it("paints the markers in the colour asked for, without moving the words", () => {
    const plain = render({ rows: CHORES, marker: "bullet" });
    const accented = render({
      rows: CHORES,
      marker: "bullet",
      markerColour: "accent",
    });
    expect(ink(accented.frame, RED)).toBeGreaterThan(ink(plain.frame, RED));
    expect(digest(accented.frame)).not.toBe(digest(plain.frame));
  });
});

describe("list overflow is honest", () => {
  const SEVEN = Array.from({ length: 7 }, (_, i) => ({ text: `Rangée ${i + 1}` }));

  it("says so on the panel and to the designer when rows are held back", () => {
    const { frame, overflows } = render({ rows: SEVEN, maxVisibleRows: 4 });
    expect(overflows).toHaveLength(1);
    expect(overflows[0]?.role).toBe("rows");
    expect(overflows[0]?.text).toContain("7");
    // The "+3 de plus" line is drawn in the attention colour.
    expect(ink(frame, RED)).toBeGreaterThan(20);
  });

  it("counts exactly the rows it did not draw", () => {
    const { frame } = render({ rows: SEVEN, maxVisibleRows: 4 });
    const expected = render({
      rows: SEVEN,
      maxVisibleRows: 4,
      moreText: { text: "+3 de plus" },
    });
    expect(digest(frame)).toBe(digest(expected.frame));
  });

  it("keeps quiet when every row fits", () => {
    // Four rows by three does not hold seven lines of 13 px type; four by four
    // does, and the difference is exactly what the warning is about.
    const { overflows, frame } = render(
      { rows: SEVEN, maxVisibleRows: 7 },
      { w: 4, h: 4 },
    );
    expect(overflows).toEqual([]);
    expect(ink(frame, RED)).toBe(0);
  });

  it("warns when the tile is the constraint, not the maximum", () => {
    const { overflows } = render({ rows: SEVEN, maxVisibleRows: 7 });
    expect(overflows).toHaveLength(1);
    expect(overflows[0]?.kind).toBe("height");
    expect(overflows[0]?.neededPx).toBeGreaterThan(
      overflows[0]?.availablePx ?? 0,
    );
  });

  it("stops at the bottom of its tile rather than drawing past it", () => {
    const { frame, overflows } = render(
      { rows: SEVEN, maxVisibleRows: 7 },
      { w: 4, h: 1 },
    );
    expect(overflows.length).toBeGreaterThan(0);
    // Nothing outside the tile.
    for (let y = 50; y < 300; y += 1) {
      for (let x = 0; x < 400; x += 1) {
        expect(frame.get(x, y)).toBe(WHITE);
      }
    }
  });

  it("marks the truncation even when there is no room to explain it", () => {
    const { frame } = render(
      { rows: SEVEN, maxVisibleRows: 7, title: { visible: false } },
      { w: 2, h: 1 },
    );
    expect(ink(frame, RED)).toBeGreaterThan(0);
  });
});

describe("list typography", () => {
  it("sets the title and the rows separately", () => {
    const base = render({ rows: CHORES });
    const biggerTitle = render({
      rows: CHORES,
      title: { style: { size: 22 } },
    });
    const biggerRows = render({ rows: CHORES, rowStyle: { size: 18 } });
    expect(digest(biggerTitle.frame)).not.toBe(digest(base.frame));
    expect(digest(biggerRows.frame)).not.toBe(digest(base.frame));
    expect(digest(biggerRows.frame)).not.toBe(digest(biggerTitle.frame));
  });

  it("draws emoji in a row through the same pictogram path", () => {
    const plain = render({ rows: [{ text: "Parcel" }] });
    const withEmoji = render({ rows: [{ text: "Parcel 🥚" }] });
    expect(digest(withEmoji.frame)).not.toBe(digest(plain.frame));
  });

  it("is a legal frame in every configuration", () => {
    for (const marker of ["bullet", "numbered", "checkbox"] as const) {
      for (const size of [11, 18, 27] as const) {
        const { frame } = render({
          rows: CHORES,
          marker,
          rowStyle: { size },
        });
        expect(pack(frame)).toHaveLength(30000);
      }
    }
  });
});

describe("list in a dashboard", () => {
  it("is offered by the registry with a usable default", () => {
    const definition = moduleDefinition("list");
    expect(definition.label).toBe("List");
    expect(definition.schema.parse(definition.defaultOptions)).toEqual(
      definition.defaultOptions,
    );
    expect(definition.sourceBinding).toBe("none");
  });

  it("renders the same bytes for the same options, every time", () => {
    expect(digest(render({ rows: CHORES }).frame)).toBe(
      digest(render({ rows: CHORES }).frame),
    );
  });
});
