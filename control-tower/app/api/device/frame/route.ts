import { NextResponse } from "next/server";
import { PACKED_BYTES } from "@/core/palette";
import { guarded, jsonError } from "@/server/auth/guard";
import { deviceContext } from "@/server/device/context";
import { framePng } from "@/server/png";

export const dynamic = "force-dynamic";

/**
 * The frame the device is actually storing, read back from the device and
 * rendered as a PNG. This is a genuine read-back, not an echo of what the
 * tower last sent, which is the whole point of showing it.
 */
export const GET = guarded({}, async () => {
  const { client } = await deviceContext();
  try {
    const { bytes } = await client.getFrame();
    if (bytes.length !== PACKED_BYTES) {
      return jsonError(
        502,
        "bad_frame",
        `The device returned ${bytes.length} bytes, not ${PACKED_BYTES}`,
      );
    }
    const png = framePng(bytes);
    return new NextResponse(new Uint8Array(png), {
      status: 200,
      headers: {
        "content-type": "image/png",
        "content-length": String(png.length),
        "cache-control": "no-store",
      },
    });
  } catch (error) {
    return jsonError(
      404,
      "no_frame",
      error instanceof Error ? error.message : "No frame is stored on the device",
    );
  }
});
