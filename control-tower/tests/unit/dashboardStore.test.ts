import { afterEach, beforeEach, describe, expect, it } from "vitest";
import fs from "node:fs";
import {
  DashboardNotFoundError,
  VersionNotFoundError,
  archiveDashboard,
  createDashboard,
  duplicateDashboard,
  listDashboards,
  listVersions,
  readRecord,
  readVersion,
  restoreDashboard,
  rollbackTo,
  saveVersion,
  selectDashboard,
  updateDashboard,
  STORE_SCHEMA_VERSION,
} from "@/server/store/dashboards";
import { readState, updateState } from "@/server/store/state";
import { paths } from "@/server/store/paths";
import { DASHBOARD_SCHEMA_VERSION, newModuleId } from "@/core/model";
import { modeOf, useTempDataRoot } from "./helpers/tempRoot";

let temp: ReturnType<typeof useTempDataRoot>;

beforeEach(() => {
  temp = useTempDataRoot();
});

afterEach(() => {
  temp.dispose();
});

describe("create", () => {
  it("creates a starter dashboard with version 1", () => {
    const record = createDashboard("Kitchen panel");
    expect(record.doc.title).toBe("Kitchen panel");
    expect(record.doc.status).toBe("active");
    expect(record.latestVersion).toBe(1);
    expect(record.versions).toHaveLength(1);
    expect(record.versions[0]?.note).toBe("Created");
    expect(record.doc.modules).toHaveLength(6);
  });

  it("can create an empty dashboard", () => {
    const record = createDashboard("Blank", { starter: false });
    expect(record.doc.modules).toEqual([]);
    expect(record.latestVersion).toBe(1);
  });

  it("writes the record at 0600 inside a 0700 directory", () => {
    const record = createDashboard("Kitchen panel");
    expect(modeOf(paths.dashboardRecord(record.doc.id))).toBe(0o600);
    expect(modeOf(paths.dashboardDir(record.doc.id))).toBe(0o700);
    expect(modeOf(paths.dashboardVersionsDir(record.doc.id))).toBe(0o700);
  });
});

describe("persistence across restarts", () => {
  it("reads back exactly what was written, from disk", () => {
    const created = createDashboard("Kitchen panel");
    // A fresh read goes to the filesystem: there is no in-process cache to
    // hide a serialisation bug behind.
    const reloaded = readRecord(created.doc.id);
    expect(reloaded).toEqual(created);
  });

  it("lists dashboards newest first", () => {
    const a = createDashboard("Alpha", { now: new Date("2026-09-01T00:00:00Z") });
    const b = createDashboard("Beta", { now: new Date("2026-09-05T00:00:00Z") });
    expect(listDashboards().map((r) => r.doc.id)).toEqual([b.doc.id, a.doc.id]);
  });

  it("hides archived dashboards unless asked", () => {
    const record = createDashboard("Kitchen panel");
    archiveDashboard(record.doc.id);
    expect(listDashboards()).toHaveLength(0);
    expect(listDashboards({ includeArchived: true })).toHaveLength(1);
  });

  it("skips a corrupt record instead of failing the whole list", () => {
    const good = createDashboard("Good");
    const bad = createDashboard("Bad");
    fs.writeFileSync(paths.dashboardRecord(bad.doc.id), "{ not json");
    expect(listDashboards().map((r) => r.doc.id)).toEqual([good.doc.id]);
  });
});

describe("versions", () => {
  it("appends a version on each save and never rewrites an old one", () => {
    const created = createDashboard("Kitchen panel");
    const id = created.doc.id;

    const v2 = saveVersion(id, { ...created.doc, title: "Kitchen panel" }, "Moved the sensor");
    expect(v2.latestVersion).toBe(2);

    const first = readVersion(id, 1);
    expect(first.version).toBe(1);
    expect(first.doc.modules).toHaveLength(6);

    // Version 1 on disk is untouched by the later save.
    const raw = JSON.parse(fs.readFileSync(paths.dashboardVersion(id, 1), "utf8"));
    expect(raw.note).toBe("Created");
  });

  it("lists versions newest first", () => {
    const created = createDashboard("Kitchen panel");
    saveVersion(created.doc.id, created.doc, "Second");
    saveVersion(created.doc.id, created.doc, "Third");
    expect(listVersions(created.doc.id).map((v) => v.version)).toEqual([3, 2, 1]);
  });

  it("refuses to overwrite an existing version file", () => {
    const created = createDashboard("Kitchen panel");
    // Force the record to point at an earlier latestVersion than the files.
    const record = readRecord(created.doc.id);
    if (!record) throw new Error("missing");
    fs.writeFileSync(
      paths.dashboardRecord(created.doc.id),
      JSON.stringify({ ...record, latestVersion: 0 }),
    );
    expect(() => saveVersion(created.doc.id, created.doc, "clash")).toThrow(
      /already exists/,
    );
  });

  it("reports a missing version rather than returning nothing", () => {
    const created = createDashboard("Kitchen panel");
    expect(() => readVersion(created.doc.id, 99)).toThrow(VersionNotFoundError);
    expect(() => readVersion(created.doc.id, 0)).toThrow(VersionNotFoundError);
  });
});

describe("rollback", () => {
  it("copies an old version forward as a new version", () => {
    const created = createDashboard("Kitchen panel");
    const id = created.doc.id;

    const trimmed = { ...created.doc, modules: created.doc.modules.slice(0, 2) };
    saveVersion(id, trimmed, "Trimmed to two modules");
    expect(readRecord(id)?.doc.modules).toHaveLength(2);

    const rolled = rollbackTo(id, 1);
    expect(rolled.latestVersion).toBe(3);
    expect(rolled.doc.modules).toHaveLength(6);
    expect(rolled.versions.at(-1)?.rolledBackFrom).toBe(1);
    expect(rolled.versions.at(-1)?.note).toBe("Rolled back to version 1");

    // History is intact: version 2 still holds the trimmed layout.
    expect(readVersion(id, 2).doc.modules).toHaveLength(2);
    expect(readVersion(id, 1).doc.modules).toHaveLength(6);
  });

  it("refuses to roll back to a version that does not exist", () => {
    const created = createDashboard("Kitchen panel");
    expect(() => rollbackTo(created.doc.id, 7)).toThrow(VersionNotFoundError);
  });
});

describe("update", () => {
  it("changes the working document without creating a version", () => {
    const created = createDashboard("Kitchen panel");
    const id = created.doc.id;
    const edited = {
      ...created.doc,
      modules: created.doc.modules.filter((m) => m.type !== "message"),
    };
    const updated = updateDashboard(id, edited);
    expect(updated.doc.modules).toHaveLength(5);
    expect(updated.latestVersion).toBe(1);
    expect(listVersions(id)).toHaveLength(1);
  });

  it("preserves createdAt and refreshes updatedAt", () => {
    const created = createDashboard("Kitchen panel", {
      now: new Date("2026-09-01T00:00:00Z"),
    });
    const updated = updateDashboard(created.doc.id, created.doc, {
      now: new Date("2026-09-11T00:00:00Z"),
    });
    expect(updated.doc.createdAt).toBe(created.doc.createdAt);
    expect(updated.doc.updatedAt).toBe("2026-09-11T00:00:00.000Z");
  });

  it("rejects an invalid layout", () => {
    const created = createDashboard("Kitchen panel");
    const broken = {
      ...created.doc,
      modules: [
        {
          id: newModuleId(),
          type: "haSensor",
          x: 6,
          y: 0,
          w: 3,
          h: 1,
          hidden: false,
          options: {},
        },
      ],
    };
    expect(() => updateDashboard(created.doc.id, broken)).toThrow(
      /runs past the 8x6 grid/,
    );
  });

  it("rejects an id mismatch between path and body", () => {
    const a = createDashboard("A");
    const b = createDashboard("B");
    expect(() => updateDashboard(a.doc.id, b.doc)).toThrow(/does not match/);
  });
});

describe("duplicate", () => {
  it("copies the layout with fresh ids and its own version 1", () => {
    const source = createDashboard("Kitchen panel");
    const copy = duplicateDashboard(source.doc.id);

    expect(copy.doc.id).not.toBe(source.doc.id);
    expect(copy.doc.title).toBe("Kitchen panel copy");
    expect(copy.latestVersion).toBe(1);
    expect(copy.doc.modules.map((m) => m.type)).toEqual(
      source.doc.modules.map((m) => m.type),
    );

    const sourceIds = new Set(source.doc.modules.map((m) => m.id));
    for (const module of copy.doc.modules) {
      expect(sourceIds.has(module.id)).toBe(false);
    }
  });

  it("accepts an explicit title", () => {
    const source = createDashboard("Kitchen panel");
    expect(duplicateDashboard(source.doc.id, "Night mode").doc.title).toBe(
      "Night mode",
    );
  });

  it("restores an archived dashboard as active when duplicated", () => {
    const source = createDashboard("Kitchen panel");
    archiveDashboard(source.doc.id);
    expect(duplicateDashboard(source.doc.id).doc.status).toBe("active");
  });
});

describe("archive and selection", () => {
  it("round-trips archive and restore", () => {
    const record = createDashboard("Kitchen panel");
    expect(archiveDashboard(record.doc.id).doc.status).toBe("archived");
    expect(restoreDashboard(record.doc.id).doc.status).toBe("active");
  });

  it("clears the selection when the selected dashboard is archived", () => {
    const record = createDashboard("Kitchen panel");
    selectDashboard(record.doc.id);
    expect(readState().selectedDashboardId).toBe(record.doc.id);
    archiveDashboard(record.doc.id);
    expect(readState().selectedDashboardId).toBeNull();
  });

  it("refuses to select an archived dashboard", () => {
    const record = createDashboard("Kitchen panel");
    archiveDashboard(record.doc.id);
    expect(() => selectDashboard(record.doc.id)).toThrow(/archived/);
  });
});

describe("unknown ids", () => {
  it("reports a missing dashboard", () => {
    expect(readRecord("d_missing")).toBeNull();
    expect(() => listVersions("d_missing")).toThrow(DashboardNotFoundError);
  });

  it("refuses a traversal attempt in the id", () => {
    expect(() => readRecord("../../etc/passwd")).toThrow(
      DashboardNotFoundError,
    );
    expect(() => readRecord("a/b")).toThrow(DashboardNotFoundError);
  });
});

describe("tower state", () => {
  it("defaults to the mock device and no selection", () => {
    const state = readState();
    expect(state.deviceMode).toBe("mock");
    expect(state.selectedDashboardId).toBeNull();
    // No address at all until somebody sets one, rather than a LAN address
    // written into the source. NOTE4C_DEVICE_ADDRESS can seed it.
    expect(state.deviceAddress).toBe("");
    expect(state.firstRunCompletedAt).toBeNull();
  });

  it("persists updates", () => {
    updateState({ density: "compact", deviceMode: "real" });
    const reread = readState();
    expect(reread.density).toBe("compact");
    expect(reread.deviceMode).toBe("real");
  });
});

/**
 * A real schema 1 dashboard, of the shape this store wrote before typography
 * existed, straight onto disk. The point is to exercise the path a dashboard
 * the owner saved last week actually takes: read, back up, migrate, rewrite.
 */
function writeLegacyDashboard(id: string): void {
  const legacyDoc = {
    schema_version: 1,
    id,
    title: "Kitchen panel",
    status: "active",
    grid: { cols: 8, rows: 6 },
    modules: [
      {
        id: "m0",
        type: "weather24h",
        x: 0,
        y: 0,
        w: 6,
        h: 3,
        options: {
          heading: "LA MÉTÉO",
          showRange: true,
          // The line the owner rejected on the panel, on, as v1 defaulted it.
          showLocation: true,
        },
      },
      {
        id: "m1",
        type: "message",
        x: 0,
        y: 5,
        w: 6,
        h: 1,
        options: { text: "Collect the parcel", expiresAt: null },
      },
      {
        id: "m2",
        type: "timestamp",
        x: 6,
        y: 5,
        w: 2,
        h: 1,
        options: { prefix: "MAJ", align: "right" },
      },
    ],
    createdAt: "2026-09-01T00:00:00.000Z",
    updatedAt: "2026-09-02T00:00:00.000Z",
  };

  fs.mkdirSync(paths.dashboardVersionsDir(id), { recursive: true });
  fs.writeFileSync(
    paths.dashboardRecord(id),
    JSON.stringify({
      schema_version: 1,
      doc: legacyDoc,
      latestVersion: 2,
      versions: [
        { version: 1, savedAt: "2026-09-01T00:00:00.000Z", note: "Created", rolledBackFrom: null },
        { version: 2, savedAt: "2026-09-02T00:00:00.000Z", note: "Tweak", rolledBackFrom: null },
      ],
    }),
  );
  for (const version of [1, 2]) {
    fs.writeFileSync(
      paths.dashboardVersion(id, version),
      JSON.stringify({
        schema_version: 1,
        version,
        savedAt: `2026-09-0${version}T00:00:00.000Z`,
        note: version === 1 ? "Created" : "Tweak",
        rolledBackFrom: null,
        doc: legacyDoc,
      }),
    );
  }
}

describe("migrating a schema 1 dashboard off disk", () => {
  it("reads it, and keeps every word that was written in it", () => {
    writeLegacyDashboard("d_legacy");
    const record = readRecord("d_legacy");
    expect(record).not.toBeNull();
    if (!record) return;

    expect(record.schema_version).toBe(STORE_SCHEMA_VERSION);
    expect(record.doc.schema_version).toBe(DASHBOARD_SCHEMA_VERSION);
    expect(record.doc.title).toBe("Kitchen panel");
    expect(record.doc.createdAt).toBe("2026-09-01T00:00:00.000Z");

    const weather = record.doc.modules.find((m) => m.id === "m0");
    const heading = (weather?.options as { heading: { text: string } }).heading;
    expect(heading.text).toBe("LA MÉTÉO");

    const message = record.doc.modules.find((m) => m.id === "m1");
    expect((message?.options as { body: { text: string } }).body.text).toBe(
      "Collect the parcel",
    );

    const stamp = record.doc.modules.find((m) => m.id === "m2");
    expect((stamp?.options as { label: { text: string } }).label.text).toBe(
      "MAJ {stamp}",
    );
  });

  it("drops the weather provenance line even though v1 had it on", () => {
    writeLegacyDashboard("d_legacy2");
    const record = readRecord("d_legacy2");
    const weather = record?.doc.modules.find((m) => m.id === "m0");
    expect(
      (weather?.options as { showProvenance: boolean }).showProvenance,
    ).toBe(false);
  });

  it("gives every module the hidden flag, defaulted to visible", () => {
    writeLegacyDashboard("d_legacy3");
    const record = readRecord("d_legacy3");
    for (const module of record?.doc.modules ?? []) {
      expect(module.hidden).toBe(false);
    }
  });

  it("preserves every version, its number, its time and its note", () => {
    writeLegacyDashboard("d_legacy4");
    // Reading the record migrates it; the versions migrate as they are read.
    readRecord("d_legacy4");

    const versions = listVersions("d_legacy4");
    expect(versions.map((v) => v.version)).toEqual([2, 1]);
    expect(versions.map((v) => v.note)).toEqual(["Tweak", "Created"]);

    for (const version of [1, 2]) {
      const stored = readVersion("d_legacy4", version);
      expect(stored.version).toBe(version);
      expect(stored.savedAt).toBe(`2026-09-0${version}T00:00:00.000Z`);
      expect(stored.rolledBackFrom).toBeNull();
      // The snapshot itself is now readable by this build.
      expect(stored.doc.schema_version).toBe(DASHBOARD_SCHEMA_VERSION);
      expect(stored.doc.modules).toHaveLength(3);
    }
  });

  it("backs the original up before rewriting it", () => {
    writeLegacyDashboard("d_legacy5");
    readRecord("d_legacy5");

    const backups = fs.readdirSync(paths.backupsDir());
    expect(backups.length).toBeGreaterThan(0);
    const original = JSON.parse(
      fs.readFileSync(
        `${paths.backupsDir()}/${backups.find((f) => f.startsWith("dashboard.json")) ?? backups[0]}`,
        "utf8",
      ),
    ) as { schema_version: number };
    expect(original.schema_version).toBe(1);
  });

  it("is idempotent: a second read changes nothing further", () => {
    writeLegacyDashboard("d_legacy6");
    const first = readRecord("d_legacy6");
    const onDisk = fs.readFileSync(paths.dashboardRecord("d_legacy6"), "utf8");
    const second = readRecord("d_legacy6");
    expect(second).toEqual(first);
    expect(fs.readFileSync(paths.dashboardRecord("d_legacy6"), "utf8")).toBe(onDisk);
  });

  it("still renders, and can still be rolled back", () => {
    writeLegacyDashboard("d_legacy7");
    readRecord("d_legacy7");
    const rolled = rollbackTo("d_legacy7", 1);
    expect(rolled.latestVersion).toBe(3);
    expect(rolled.versions.at(-1)?.rolledBackFrom).toBe(1);
    // History is not rewritten: versions 1 and 2 are still exactly where they were.
    expect(readVersion("d_legacy7", 1).savedAt).toBe("2026-09-01T00:00:00.000Z");
  });
});

// Regression (2026-09-19): the doc schema was bumped v4->v5 (expression theme)
// but STORE_SCHEMA_VERSION lagged at 4, so migrateStoredDoc never ran for a v4
// record on disk and the read threw "expected 5". A real dashboard became
// unreadable. STORE_SCHEMA_VERSION must track the doc version.
describe("store schema version tracks the doc schema version", () => {
  it("STORE_SCHEMA_VERSION equals the current dashboard doc version", () => {
    expect(STORE_SCHEMA_VERSION).toBe(DASHBOARD_SCHEMA_VERSION);
  });
});
