import { afterEach, beforeEach, describe, expect, it } from "vitest";
import fs from "node:fs";
import path from "node:path";
import { z } from "zod";
import {
  DocumentCorruptError,
  appendJsonLine,
  readDocument,
  readJsonLines,
  writeDocument,
  writeFileAtomic,
  writeJsonAtomic,
} from "@/server/store/atomicFile";
import { ensureDataRoot, paths } from "@/server/store/paths";
import { modeOf, useTempDataRoot } from "./helpers/tempRoot";

let temp: ReturnType<typeof useTempDataRoot>;

beforeEach(() => {
  temp = useTempDataRoot();
});

afterEach(() => {
  temp.dispose();
});

describe("data root", () => {
  it("creates every subdirectory at 0700", () => {
    const root = ensureDataRoot();
    expect(modeOf(root)).toBe(0o700);
    for (const sub of ["dashboards", "ledger", "secrets", "audit", "backups"]) {
      expect(modeOf(path.join(root, sub))).toBe(0o700);
    }
  });
});

describe("writeFileAtomic", () => {
  it("writes files at 0600 by default", () => {
    const target = path.join(temp.root, "a.json");
    writeJsonAtomic(target, { hello: "world" });
    expect(modeOf(target)).toBe(0o600);
    expect(JSON.parse(fs.readFileSync(target, "utf8"))).toEqual({
      hello: "world",
    });
  });

  it("leaves the previous content intact when a write is interrupted", () => {
    const target = path.join(temp.root, "record.json");
    writeJsonAtomic(target, { generation: 1 });

    // Simulate a crash: a temp file exists in the directory but the rename
    // never happened. The reader must still see the committed generation.
    const orphan = path.join(temp.root, `.record.json.tmp-999-crash`);
    fs.writeFileSync(orphan, '{"generation":2,"torn":');

    expect(JSON.parse(fs.readFileSync(target, "utf8"))).toEqual({
      generation: 1,
    });
    expect(fs.existsSync(orphan)).toBe(true);

    // A subsequent successful write commits cleanly despite the orphan.
    writeJsonAtomic(target, { generation: 3 });
    expect(JSON.parse(fs.readFileSync(target, "utf8"))).toEqual({
      generation: 3,
    });
  });

  it("removes its own temp file when the write fails", () => {
    const target = path.join(temp.root, "nested", "deep.json");
    writeJsonAtomic(target, { ok: true });
    const leftovers = fs
      .readdirSync(path.join(temp.root, "nested"))
      .filter((name) => name.includes(".tmp-"));
    expect(leftovers).toEqual([]);
  });

  it("accepts raw buffers for binary payloads", () => {
    const target = path.join(temp.root, "frame.bin");
    const bytes = Buffer.from([0x00, 0xff, 0x55, 0xaa]);
    writeFileAtomic(target, bytes);
    expect(fs.readFileSync(target)).toEqual(bytes);
  });
});

const DocV2 = z.object({
  schema_version: z.literal(2),
  title: z.string(),
  tags: z.array(z.string()),
});
type DocV2 = z.infer<typeof DocV2>;

const spec = {
  schema: DocV2,
  version: 2,
  migrate: (raw: unknown): unknown => {
    const old = raw as { title?: string; tag?: string };
    return {
      schema_version: 2,
      title: old.title ?? "untitled",
      tags: old.tag ? [old.tag] : [],
    };
  },
};

describe("versioned documents", () => {
  it("round-trips a current document", () => {
    const target = path.join(temp.root, "doc.json");
    const value: DocV2 = { schema_version: 2, title: "Kitchen panel", tags: ["a"] };
    writeDocument(target, spec, value);
    expect(readDocument(target, spec)).toEqual(value);
  });

  it("returns null for a missing document", () => {
    expect(readDocument(path.join(temp.root, "absent.json"), spec)).toBeNull();
  });

  it("copies the old file into backups before migrating forward", () => {
    ensureDataRoot();
    const target = path.join(temp.root, "doc.json");
    writeJsonAtomic(target, { schema_version: 1, title: "Old", tag: "night" });

    const migrated = readDocument(target, spec);
    expect(migrated).toEqual({
      schema_version: 2,
      title: "Old",
      tags: ["night"],
    });

    const backups = fs.readdirSync(paths.backupsDir());
    expect(backups).toHaveLength(1);
    expect(backups[0]).toMatch(/^doc\.json\..*\.bak$/);
    const backedUp = JSON.parse(
      fs.readFileSync(path.join(paths.backupsDir(), backups[0] as string), "utf8"),
    );
    expect(backedUp).toEqual({ schema_version: 1, title: "Old", tag: "night" });

    // The migration is persisted, so the next read needs no backup.
    expect(readDocument(target, spec)).toEqual(migrated);
    expect(fs.readdirSync(paths.backupsDir())).toHaveLength(1);
  });

  it("refuses a document written by a newer build", () => {
    const target = path.join(temp.root, "doc.json");
    writeJsonAtomic(target, { schema_version: 99, title: "Future", tags: [] });
    expect(() => readDocument(target, spec)).toThrow(DocumentCorruptError);
  });

  it("refuses a document that fails its schema", () => {
    const target = path.join(temp.root, "doc.json");
    writeJsonAtomic(target, { schema_version: 2, title: 42, tags: [] });
    expect(() => readDocument(target, spec)).toThrow(DocumentCorruptError);
  });
});

const LedgerLine = z.object({ seq: z.number(), state: z.string() });

describe("append-only JSONL", () => {
  it("appends and reads back every line in order", () => {
    const target = path.join(temp.root, "ledger.jsonl");
    appendJsonLine(target, { seq: 1, state: "pending" });
    appendJsonLine(target, { seq: 1, state: "sent" });
    appendJsonLine(target, { seq: 1, state: "verified_displayed" });
    expect(readJsonLines(target, LedgerLine).map((l) => l.state)).toEqual([
      "pending",
      "sent",
      "verified_displayed",
    ]);
    expect(modeOf(target)).toBe(0o600);
  });

  it("skips a torn trailing line without losing earlier entries", () => {
    const target = path.join(temp.root, "ledger.jsonl");
    appendJsonLine(target, { seq: 1, state: "pending" });
    fs.appendFileSync(target, '{"seq":2,"state":"se');
    expect(readJsonLines(target, LedgerLine)).toEqual([
      { seq: 1, state: "pending" },
    ]);
  });

  it("returns an empty list when the ledger does not exist yet", () => {
    expect(readJsonLines(path.join(temp.root, "none.jsonl"), LedgerLine)).toEqual(
      [],
    );
  });
});
