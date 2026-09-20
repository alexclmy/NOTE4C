"use client";

import { useEffect, useState } from "react";
import { useRouter } from "next/navigation";
import "@/ui/app.css";
import { ApiError, apiGet, apiSend } from "@/ui/api";
import { Button, Card, ErrorNote } from "@/ui/components";

interface AuthState {
  passphraseSet: boolean;
  authenticated: boolean;
}

const MIN_LENGTH = 12;

export default function LoginPage() {
  const router = useRouter();
  const [state, setState] = useState<AuthState | null>(null);
  const [passphrase, setPassphrase] = useState("");
  const [confirmation, setConfirmation] = useState("");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    apiGet<AuthState>("/api/auth/state")
      .then((value) => {
        setState(value);
        if (value.authenticated) router.replace("/overview");
      })
      .catch(() => setState({ passphraseSet: true, authenticated: false }));
  }, [router]);

  async function submit(event: React.FormEvent): Promise<void> {
    event.preventDefault();
    setError("");

    if (!state) return;

    if (!state.passphraseSet) {
      if (passphrase.length < MIN_LENGTH) {
        setError(`The passphrase must be at least ${MIN_LENGTH} characters`);
        return;
      }
      if (passphrase !== confirmation) {
        setError("The two passphrases do not match");
        return;
      }
    }

    setBusy(true);
    try {
      await apiSend(
        state.passphraseSet ? "/api/auth/login" : "/api/auth/setup",
        "POST",
        { passphrase },
      );
      router.push("/overview");
      router.refresh();
    } catch (caught) {
      setError(
        caught instanceof ApiError ? caught.message : "The tower did not answer",
      );
    } finally {
      setBusy(false);
    }
  }

  if (!state) {
    return (
      <div className="login-wrap">
        <p>Loading.</p>
      </div>
    );
  }

  const firstRun = !state.passphraseSet;

  return (
    <div className="login-wrap">
      {/*
        The same mark the header carries, so the sign-in screen and the tower
        behind it are visibly one thing. It is a yellow square and two words:
        there is no logo file in this repository and no request for one.
      */}
      <div className="brand" style={{ marginBottom: "var(--pad-3)" }}>
        <span className="brand-mark" aria-hidden="true" />
        <span className="brand-name">NOTE4C</span>
        <span className="brand-sub">CONTROL&nbsp;TOWER</span>
      </div>
      <h1>{firstRun ? "Welcome" : "Sign in"}</h1>
      <Card variant="plain">
        {firstRun ? (
          <p>
            Set a passphrase for this tower. It protects the device token and
            every send, and it is required before the tower can ever listen on
            anything but loopback.
          </p>
        ) : (
          <p>Enter the tower passphrase to continue.</p>
        )}

        <form onSubmit={submit}>
          <label className="field">
            <span>Passphrase</span>
            <input
              type="password"
              value={passphrase}
              onChange={(event) => setPassphrase(event.target.value)}
              autoComplete={firstRun ? "new-password" : "current-password"}
              data-testid="passphrase"
              autoFocus
            />
          </label>

          {firstRun && (
            <label className="field">
              <span>Passphrase again</span>
              <input
                type="password"
                value={confirmation}
                onChange={(event) => setConfirmation(event.target.value)}
                autoComplete="new-password"
                data-testid="passphrase-confirm"
              />
              <span className="field-hint">
                At least {MIN_LENGTH} characters. It is hashed with scrypt and
                never stored in plain text.
              </span>
            </label>
          )}

          <Button type="submit" variant="primary" disabled={busy} testId="submit-auth">
            {busy ? "Working" : firstRun ? "Set passphrase and continue" : "Sign in"}
          </Button>
          <ErrorNote>{error}</ErrorNote>
        </form>
      </Card>
    </div>
  );
}
