"use client";

import { useCallback, useEffect, useState, type ReactNode } from "react";
import Link from "next/link";
import { REAL_MODE_CONFIRMATION } from "@/core/gates";
import { ApiError, apiGet, apiSend } from "@/ui/api";
import { Button, ErrorNote } from "@/ui/components";
import { ConfirmDialog } from "@/ui/Dialog";
import { PageLoading } from "@/ui/PageState";
import { useToast } from "@/ui/Toast";

interface AuthState {
  passphraseSet: boolean;
}

interface TowerState {
  deviceMode: "mock" | "real";
  deviceAddress: string;
  deviceTokenSet: boolean;
  bridgeTokenAvailable: boolean;
  bridgeTokenPath: string;
}

/**
 * Four steps, once, and every one of them is a thing the tower can actually do.
 *
 * The step that decides the shape of this page is the third, and it is worth
 * being explicit about what it is not. The device does have a pairing window —
 * `POST /api/v1/dashboard/pair` claims a token during a 120 second window that
 * somebody opens by pressing buttons on the device — but **this tower has no
 * client for it**. The registry says so in as many words ("The tower never
 * initiates it"), and re-pairing would mint a new token and invalidate the
 * credential the composer bridge is already using.
 *
 * So the third step describes the mechanism that exists: the token the device
 * already issued is copied into the tower's own secrets directory, and then the
 * tower is pointed at the panel behind the typed `REAL DEVICE` confirmation.
 * Writing the prototype's "a 2-minute window opens, press pair now" would have
 * been a button for an operation with nothing behind it.
 *
 * Nothing on this page is a progress bar over a wizard's own memory: which step
 * you are on is derived, every time, from what the tower reports — whether a
 * passphrase exists, whether an address is set, whether a token is stored,
 * whether the mode is real. Reload it halfway through and it resumes where the
 * *tower* is, not where the page thought you were.
 */
export default function SetupPage() {
  const toast = useToast();
  const [auth, setAuth] = useState<AuthState | null>(null);
  const [state, setState] = useState<TowerState | null>(null);
  const [address, setAddress] = useState("");
  const [checking, setChecking] = useState(false);
  const [addressNote, setAddressNote] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [dialog, setDialog] = useState<"none" | "import" | "real">("none");

  const load = useCallback(async () => {
    try {
      const [authState, towerState] = await Promise.all([
        apiGet<AuthState>("/api/auth/state"),
        apiGet<TowerState>("/api/state"),
      ]);
      setAuth(authState);
      setState(towerState);
      setAddress((current) => (current === "" ? towerState.deviceAddress : current));
      setError("");
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
    try {
      toast(await work());
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : "That step did not complete");
    } finally {
      setBusy(false);
      setDialog("none");
      await load();
    }
  }

  async function checkAddress(): Promise<void> {
    setChecking(true);
    setAddressNote("");
    try {
      await apiSend("/api/state", "PATCH", { deviceAddress: address.trim() });
      /*
       * A status read, which is the only honest way to answer "does this
       * address answer". It is read-only and it is the same call the rest of
       * the product makes; a device that is asleep will not answer it, and the
       * note below says so rather than reporting a wrong address.
       */
      const status = await apiGet<{ reachable: boolean; detail?: string }>(
        "/api/device/status",
      );
      setAddressNote(
        status.reachable
          ? `${address.trim()} answered.`
          : `${address.trim()} did not answer. That is what a sleeping device looks like too, so it is not proof the address is wrong — press the button on the device and check again if you want certainty. ${status.detail ?? ""}`,
      );
      await load();
    } catch (caught) {
      setAddressNote(
        caught instanceof ApiError ? caught.message : "The tower did not answer",
      );
    } finally {
      setChecking(false);
    }
  }

  if (!auth || !state) {
    return (
      <div className="setup-wrap">
        {error ? <ErrorNote>{error}</ErrorNote> : <PageLoading label="Reading the tower." />}
      </div>
    );
  }

  /*
   * Which step is live, derived from the tower rather than remembered.
   *
   * Each step is done when the *thing it achieves* is true, so a person who
   * already imported a token months ago sees three ticks and one live step
   * instead of being walked through work they have finished.
   */
  const done = [
    auth.passphraseSet,
    state.deviceAddress.length > 0,
    state.deviceTokenSet,
    state.deviceMode === "real",
  ];
  const activeIndex = done.findIndex((value) => !value);
  const complete = activeIndex === -1;

  const steps: ReadonlyArray<{
    title: string;
    body: ReactNode;
    doneLine: string;
  }> = [
    {
      title: "Protect the Tower",
      body: (
        <>
          <p>
            A passphrase guards the device token and every send. It never leaves
            this computer: it is stretched with scrypt and only the derived key
            is stored.
          </p>
          <p className="mono-note">
            This one is set on the sign-in screen the first time the tower is
            opened, which you have already done to be reading this.
          </p>
        </>
      ),
      doneLine: "passphrase set",
    },
    {
      title: "Point at the device",
      body: (
        <>
          <p>
            The address the device shows on its own screen, on your home
            network.
          </p>
          <label className="field">
            <span>Device address</span>
            <input
              value={address}
              onChange={(event) => setAddress(event.target.value)}
              placeholder="192.168.x.x"
              data-testid="setup-address"
              autoComplete="off"
            />
          </label>
          <p className="mono-note">
            Only private local addresses are accepted — the panel is never
            exposed to the internet, and the tower refuses anything that is not
            a private IPv4 on port 80.
          </p>
          <div className="card-actions" style={{ marginTop: "var(--pad-2)" }}>
            <Button
              variant="primary"
              onClick={() => void checkAddress()}
              disabled={checking || address.trim().length === 0}
              testId="setup-check-address"
            >
              {checking ? "Checking…" : "Check address"}
            </Button>
          </div>
        </>
      ),
      doneLine: `${state.deviceAddress || "address"} · saved`,
    },
    {
      title: "Bring in the token, with the device in hand",
      body: (
        <>
          <p>
            Pairing happens on the device, not from here: it issues its own
            token during a window you open from its settings menu, and nothing
            on the network can open that window. This Tower never asks for a new
            one — re-pairing would mint a fresh token and invalidate the
            credential anything else on this machine is already using.
          </p>
          <p>
            What it does instead is copy the token the device already issued
            into its own secrets directory, at mode 0600, where no route ever
            reads it back out.
          </p>
          {state.bridgeTokenPath.length > 0 ? (
            <>
              <p className="mono-note">
                Looking in {state.bridgeTokenPath} —{" "}
                {state.bridgeTokenAvailable ? "a token is there" : "nothing there yet"}.
              </p>
              <div className="card-actions" style={{ marginTop: "var(--pad-2)" }}>
                <Button
                  variant="primary"
                  disabled={busy || !state.bridgeTokenAvailable}
                  onClick={() => setDialog("import")}
                  testId="setup-import-token"
                >
                  Copy the token in
                </Button>
              </div>
            </>
          ) : (
            <p className="mono-note" data-testid="setup-no-token-path">
              No token path is configured, so there is nothing for the tower to
              copy. Set NOTE4C_BRIDGE_TOKEN_PATH to the file the device&rsquo;s
              own pairing wrote, then come back. The tower will not invent a
              credential.
            </p>
          )}
        </>
      ),
      doneLine: "token stored",
    },
    {
      title: "Point the Tower at the panel",
      body: (
        <>
          <p>
            Until this step every send goes to the in-repo simulator. After it,
            a send writes to the panel at {state.deviceAddress || "the address above"} —
            a full refresh cycle, tens of seconds of active panel time, and real
            e-paper wear each time.
          </p>
          <p className="mono-note">
            The tower reads the device status first, which is a read-only call.
            If it does not answer, nothing changes and you stay on the
            simulator.
          </p>
          <div className="card-actions" style={{ marginTop: "var(--pad-2)" }}>
            <Button
              variant="primary"
              disabled={busy || !state.deviceTokenSet}
              onClick={() => setDialog("real")}
              testId="setup-switch-real"
            >
              Switch to the real panel
            </Button>
          </div>
        </>
      ),
      doneLine: "pointed at the real panel",
    },
  ];

  return (
    <div className="setup-wrap">
      <Link className="btn" href="/device" style={{ marginBottom: "var(--pad-3)" }}>
        ← Back to Device
      </Link>
      <h1>Set up the connection</h1>
      <p style={{ color: "var(--ink-soft)", lineHeight: 1.6, marginBottom: "var(--pad-3)" }}>
        Four steps, once. The third one needs the device in your hand.
      </p>

      {/*
        The one thing this page must say whichever step is live.
        The device does have a pairing window, and this tower cannot use it:
        `POST /api/v1/dashboard/pair` claims a token during 120 seconds that
        somebody opens by pressing buttons on the device, and re-pairing would
        mint a new token and invalidate the credential anything else on this
        machine is already using. Leaving that inside step three meant it was
        collapsed — and therefore unsaid — for anybody not standing on step
        three, which is everybody until they get there.
      */}
      <p className="mono-note" style={{ marginBottom: "var(--pad-4)" }}>
        Pairing happens on the device, not from here. The Tower never asks for a
        new token; it copies in the one the device already issued.
      </p>

      <ErrorNote>{error}</ErrorNote>

      <div className="setup-steps" data-testid="setup-steps">
        {steps.map((step, index) => {
          const stepState = done[index]
            ? "done"
            : index === activeIndex
              ? "active"
              : "todo";
          return (
            <div
              className="setup-step"
              data-state={stepState}
              data-testid={`setup-step-${index + 1}`}
              key={step.title}
            >
              <div className="setup-step-head">
                <span className="setup-num" aria-hidden="true">
                  {done[index] ? "✓" : index + 1}
                </span>
                <strong>{step.title}</strong>
              </div>
              {stepState === "active" && <div className="setup-body">{step.body}</div>}
              {stepState === "done" && (
                <p className="setup-done-line">{step.doneLine} ✓</p>
              )}
            </div>
          );
        })}
      </div>

      {/*
        Outside the steps, deliberately. Saving an address completes step two,
        which unmounts its body — so a note rendered inside it reported the
        result of the check for exactly as long as it took the check to finish.
      */}
      {addressNote && (
        <p
          className="mono-note"
          role="status"
          data-testid="setup-address-note"
          style={{ marginTop: "var(--pad-3)" }}
        >
          {addressNote}
        </p>
      )}

      {complete && (
        <div className="setup-complete" data-testid="setup-complete">
          <strong>Paired. The token is stored — you&rsquo;ll never need to see it.</strong>
          <p style={{ margin: "0 0 14px" }}>
            From now on, only this Tower can change what the panel shows.
          </p>
          <Link className="btn btn-dark" href="/overview">
            Go to Overview →
          </Link>
        </div>
      )}

      <ConfirmDialog
        open={dialog === "import"}
        title="Copy the device token into the tower"
        confirmLabel="Copy the token"
        body={
          <>
            <p>The tower will read this exact file and copy it into its own secrets directory at mode 0600:</p>
            <p>
              <code>{state.bridgeTokenPath}</code>
            </p>
            <p>
              The value is never displayed, logged, or returned by any route.
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
              From this point every send writes to the panel at{" "}
              {state.deviceAddress}. A send costs a full refresh cycle, which is
              tens of seconds of active panel time and real e-paper wear.
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
    </div>
  );
}
