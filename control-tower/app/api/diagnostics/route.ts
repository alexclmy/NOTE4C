import { NextResponse } from "next/server";
import { TOWER_VERSION } from "@/core/version";
import { guarded } from "@/server/auth/guard";
import { checkProcessBind } from "@/server/auth/bindStartup";
import { readAudit } from "@/server/audit";
import { deviceContext } from "@/server/device/context";
import { describeDeviceFailure } from "@/server/device/failure";
import { readPushes } from "@/server/device/ledger";
import { readComposerFeed } from "@/server/sources/composerFeed";
import { haConfigured } from "@/server/sources/haSensor";
import { readCalendarSnapshot } from "@/server/sources/calendarSnapshot";
import { e2eCalendar } from "@/server/sources/e2eFixtures";
import { paths } from "@/server/store/paths";
import { readState } from "@/server/store/state";
import {
  availableAtlases,
  ATLAS_RASTER,
  FONT_FAMILY_META,
} from "@/core/render/fonts";
import { supportedPictograms } from "@/core/render/pictograms";
import {
  DEFAULT_REMINDERS_LIST,
  READ_ONLY_SUBCOMMANDS,
  remindctlPath,
} from "@/server/sources/appleReminders";

export const dynamic = "force-dynamic";

export const GET = guarded({}, async () => {
  const state = readState();
  const { client, simulated } = await deviceContext();

  const [composer, deviceStatus] = await Promise.all([
    readComposerFeed(),
    client
      .status()
      .then((status) => ({ reachable: true as const, status }))
      // The device card's whole job is to say what is wrong. `failure` is the
      // classification — absent, transport, refused, contract, uncertain — and
      // `detail` is the sentence the card already renders, which now names the
      // cause instead of always reading "The device could not be reached".
      .catch((error: unknown) => {
        const failure = describeDeviceFailure(error);
        return { reachable: false as const, detail: failure.detail, failure };
      }),
  ]);

  const calendar = e2eCalendar() ?? readCalendarSnapshot();

  return NextResponse.json({
    tower: {
      dataRoot: paths.root(),
      deviceMode: state.deviceMode,
      simulated,
      deviceOrigin: simulated ? client.origin : `http://${state.deviceAddress}:80`,
      atlases: availableAtlases(),
      atlasRaster: ATLAS_RASTER,
      // Where every glyph on the panel comes from, and under what licence.
      // This is the provenance that belongs in diagnostics rather than being
      // forced onto a 400x300 e-paper panel.
      fontFamilies: Object.entries(FONT_FAMILY_META).map(([id, meta]) => ({
        id,
        label: meta.label,
        licence: meta.licence,
        copyright: meta.copyright,
        upstream: meta.upstream,
        vendoredFrom: meta.vendored_from,
        licenceFile: meta.licence_file,
      })),
      // The panel's symbol vocabulary. Drawn in this repository, in the four
      // pigments the device prints: no emoji font is shipped, downloaded or
      // depended on, and there is nothing here to redistribute.
      pictograms: {
        raster: "hand-drawn 12x12 sprites in src/core/render/pictograms.ts",
        count: supportedPictograms().reduce(
          (sum, group) => sum + group.entries.length,
          0,
        ),
        categories: supportedPictograms().map((group) => ({
          category: group.category,
          symbols: group.entries.map((entry) => entry.emoji).join(" "),
        })),
      },
      node: process.version,
      /**
       * Whether the bind guard actually ran here, and what it saw.
       *
       * Reported rather than assumed, and "undetermined" is a real value: the
       * launcher (tools/tower-serve.ts) checks the host before Next starts, but
       * a server somebody started another way can only be judged from inside,
       * and not every Next topology puts the listening socket in the process
       * that runs instrumentation. Saying "loopback" about a bind this process
       * never observed would be the same dishonesty the guard exists to avoid.
       */
      bind: checkProcessBind(),
      /**
       * What this build calls itself. It goes in the Tower card because the
       * first question on any issue is "which version", and the answer should
       * be copyable from the interface rather than guessed from a git clone.
       */
      version: TOWER_VERSION,
    },
    device: deviceStatus,
    pushes: readPushes().slice(0, 50),
    audit: readAudit().slice(0, 100),
    composer,
    sources: {
      calendar: { state: calendar.state, detail: calendar.detail ?? null },
      homeAssistant: { configured: haConfigured() },
      /*
       * Apple Reminders is described here, never read here. This endpoint runs
       * on every visit to Diagnostics, and reading somebody's Reminders to
       * decorate a page would be collecting private data for no reason. The
       * adapter runs only when a dashboard binds to it.
       */
      reminders: {
        binary: remindctlPath() || "not configured",
        defaultList: DEFAULT_REMINDERS_LIST,
        readOnlySubcommands: [...READ_ONLY_SUBCOMMANDS],
        note: "Read only. The tower never adds, edits, completes, renames or deletes a reminder, and reads the list only when a dashboard asks for it.",
      },
    },
  });
});
