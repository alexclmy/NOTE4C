"use client";

import { useCallback, useEffect, useState } from "react";
import Link from "next/link";
import { ApiError, apiGet, apiSend } from "./api";
import { Badge, Button, Card, ErrorNote, Row } from "./components";
import { ConfirmDialog } from "./Dialog";
import { REAL_MODE_CONFIRMATION } from "@/core/gates";

interface StatePayload {
  deviceMode: "mock" | "real";
  deviceAddress: string;
  deviceTokenSet: boolean;
  bridgeTokenAvailable: boolean;
  /** Server-side configuration, so the consent dialog can name the file. */
  bridgeTokenPath: string;
}

type DialogKind = "none" | "import" | "real";

/**
 * The real-device path, off by default.
 *
 * Two separate gates, because they are two separate decisions: taking a copy
 * of an existing device credential, and pointing the tower at hardware. The
 * real-device procedure is the second half of QUICKSTART.md.
 */
export function RealDeviceCard() {
  const [state, setState] = useState<StatePayload | null>(null);
  const [dialog, setDialog] = useState<DialogKind>("none");
  const [address, setAddress] = useState("");
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const [busy, setBusy] = useState(false);

  const load = useCallback(async () => {
    try {
      const payload = await apiGet<StatePayload>("/api/state");
      setState(payload);
      setAddress(payload.deviceAddress);
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : "The tower did not answer");
    }
  }, []);

  useEffect(() => {
    void load();
  }, [load]);

  async function run(work: () => Promise<string>): Promise<void> {
    setBusy(true);
    setError("");
    setNotice("");
    try {
      setNotice(await work());
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : "The action failed");
    } finally {
      setBusy(false);
      setDialog("none");
      await load();
    }
  }

  if (!state) return null;

  return (
    <Card title="Connection" variant="plain" testId="connection-card">
      <Row label="Pointed at">
        {state.deviceMode === "mock" ? (
          <Badge kind="simulated" />
        ) : (
          <Badge kind="neutral">real panel</Badge>
        )}
      </Row>

      <Row label="Address" hint="Private IPv4 only, port 80, no hostnames">
        <input
          value={address}
          onChange={(event) => setAddress(event.target.value)}
          onBlur={() =>
            void run(async () => {
              await apiSend("/api/state", "PATCH", { deviceAddress: address.trim() });
              return `Device address set to ${address.trim()}`;
            })
          }
          placeholder="192.168.x.x"
          style={{ width: 150, textAlign: "right" }}
          data-testid="device-address"
        />
      </Row>

      {/*
        "Stored — never shown", and it is literally true: the token is written
        to the tower's own secrets directory at mode 0600 and no route returns
        it. Saying so where the value would otherwise be is what stops somebody
        looking for a field to read it out of.
      */}
      <Row label="Access token">
        {state.deviceTokenSet ? (
          <span>stored — never shown</span>
        ) : (
          <span className="gated">not configured</span>
        )}
      </Row>

      <div className="card-actions" style={{ marginTop: "var(--pad-2)" }}>
        {/* Only when there is a configured file to import from. An
            installation that never set NOTE4C_BRIDGE_TOKEN_PATH has no such
            credential, and a disabled button about one is furniture. */}
        {state.bridgeTokenPath.length > 0 && (
          <Button
            disabled={busy || !state.bridgeTokenAvailable}
            onClick={() => setDialog("import")}
            testId="import-token"
            title={
              state.bridgeTokenAvailable
                ? undefined
                : `No token was found at ${state.bridgeTokenPath}`
            }
          >
            Import the bridge token
          </Button>
        )}

        {state.deviceMode === "mock" ? (
          <Button
            variant="danger"
            disabled={busy || !state.deviceTokenSet}
            onClick={() => setDialog("real")}
            testId="switch-real"
          >
            Switch to the real panel
          </Button>
        ) : (
          <Button
            disabled={busy}
            onClick={() =>
              void run(async () => {
                await apiSend("/api/device/mode", "POST", {
                  action: "set-mode",
                  mode: "mock",
                });
                return "Back on the simulated device";
              })
            }
            testId="switch-mock"
          >
            Back to the mock device
          </Button>
        )}

        {state.deviceTokenSet && (
          <Button
            variant="quiet"
            disabled={busy}
            onClick={() =>
              void run(async () => {
                await apiSend("/api/device/mode", "POST", { action: "forget-token" });
                return "The tower's copy of the device token was removed";
              })
            }
            testId="forget-token"
          >
            Forget the token
          </Button>
        )}
      </div>

      {notice && <p className="field-hint">{notice}</p>}
      <ErrorNote>{error}</ErrorNote>

      <p className="mono-note" style={{ marginTop: "var(--pad-2)" }}>
        Pairing happens on the device: it issues its own token during a window
        opened from its settings menu, and nothing on the network can open that
        window. The tower copies in the token the device already issued rather
        than asking for a new one, because re-pairing would invalidate the
        credential anything else on this machine is using.
      </p>

      <div className="card-actions" style={{ marginTop: "var(--pad-2)" }}>
        <Link className="btn" href="/device/setup" data-testid="open-setup">
          Pairing &amp; setup steps →
        </Link>
      </div>

      <p className="field-hint" style={{ marginTop: "var(--pad-2)" }}>
        Switching to the real panel performs one read-only status read first. If
        the device does not answer, the tower stays on the mock rather than
        claiming a connection it does not have. The single-send procedure is in
        QUICKSTART.md.
      </p>

      <ConfirmDialog
        open={dialog === "import"}
        title="Copy the bridge token into the tower"
        confirmLabel="Copy the token"
        body={
          <>
            <p>The tower will read this exact file and copy it into its own secrets directory at mode 0600:</p>
            <p>
              <code>{state.bridgeTokenPath}</code>
            </p>
            <p>
              The value is never displayed, logged, or returned by any route.
              Pairing again instead would mint a new token and invalidate the
              composer bridge&apos;s credential, which is why importing is the
              default.
            </p>
          </>
        }
        onConfirm={() =>
          void run(async () => {
            await apiSend("/api/device/mode", "POST", {
              action: "import-bridge-token",
              consentPath: state.bridgeTokenPath,
            });
            return "The device token is now configured in the tower";
          })
        }
        onCancel={() => setDialog("none")}
      />

      <ConfirmDialog
        open={dialog === "real"}
        title="Point the tower at the real panel"
        confirmWord={REAL_MODE_CONFIRMATION}
        confirmLabel="Switch to the real panel"
        body={
          <>
            <p>
              From this point every push writes to the panel at {address}. A
              push costs a full refresh cycle, which is tens of seconds of
              active panel time and real e-paper wear.
            </p>
            <p>
              The tower will first read the device status, which is a read-only
              call. If that fails, nothing changes.
            </p>
          </>
        }
        onConfirm={() =>
          void run(async () => {
            const result = await apiSend<{ status: { firmware: string; api: number } }>(
              "/api/device/mode",
              "POST",
              {
                action: "set-mode",
                mode: "real",
                confirm: REAL_MODE_CONFIRMATION,
              },
            );
            return `Connected: firmware ${result.status.firmware}, api ${result.status.api}`;
          })
        }
        onCancel={() => setDialog("none")}
      />
    </Card>
  );
}
