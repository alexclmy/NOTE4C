"use client";

import { useCallback, useEffect, useState } from "react";
import { ApiError, apiGet, apiSend } from "@/ui/api";
import {
  Badge,
  Banner,
  Button,
  Card,
  ErrorNote,
  Row,
} from "@/ui/components";
import { Disclosure } from "@/ui/Disclosure";
import { PageLoading } from "@/ui/PageState";
import {
  CAP_CONFIG_V2,
  CAP_VOICE_PTT_V1,
  hasCapability,
  negotiate,
  type DeviceCapabilities,
} from "@/core/registry";

interface ConfigPayload {
  supported: boolean;
  revision?: number;
  detail?: string;
  config?: {
    voice: { muted: boolean; hub_url: string; hub_token_set: boolean };
  };
}

interface StatusPayload {
  status?: { api: number; capabilities?: string[] };
}

/**
 * The Voice page states only what the evidence supports.
 *
 * The distinction it exists to keep is between three different kinds of "no
 * audio": the capture path is not in the binary, the software mute is on, and
 * there is no hub to send anything to. Those need three different actions from
 * the user, so they get three different rows.
 */
export default function VoicePage() {
  const [url, setUrl] = useState("");
  const [token, setToken] = useState("");
  const [config, setConfig] = useState<ConfigPayload | null>(null);
  const [device, setDevice] = useState<DeviceCapabilities>({
    api: 1,
    capabilities: [],
  });
  const [result, setResult] = useState<{ url: string | null; tokenSet: boolean } | null>(
    null,
  );
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const [busy, setBusy] = useState(false);

  const load = useCallback(async () => {
    try {
      const [status, configPayload] = await Promise.all([
        apiGet<StatusPayload>("/api/device/status"),
        apiGet<ConfigPayload>("/api/device/config"),
      ]);
      setDevice(negotiate(status.status ?? { api: 1 }));
      setConfig(configPayload);
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : "The tower did not answer");
    }
  }, []);

  useEffect(() => {
    void load();
  }, [load]);

  const configLive = config?.supported === true && hasCapability(device, CAP_CONFIG_V2);
  const pttCompiled = hasCapability(device, CAP_VOICE_PTT_V1);
  const muted = config?.config?.voice.muted;
  const hubUrl = config?.config?.voice.hub_url ?? "";
  const hubTokenSet = config?.config?.voice.hub_token_set ?? false;

  async function setMuted(next: boolean): Promise<void> {
    if (config?.revision === undefined) return;
    setBusy(true);
    setError("");
    setNotice("");
    try {
      const response = await apiSend<{ revision: number; config: { voice: { muted: boolean; hub_url: string; hub_token_set: boolean } } }>(
        "/api/device/config",
        "PATCH",
        { expectedRevision: config.revision, set: { "voice.muted": next } },
      );
      setConfig({
        ...config,
        revision: response.revision,
        config: { voice: response.config.voice },
      });
      setNotice(
        next
          ? "The device confirmed the mute. Nothing will be recorded."
          : "The device confirmed the unmute. Recording still needs the capture path compiled in, Wi-Fi up and a hub configured.",
      );
    } catch (caught) {
      if (caught instanceof ApiError && caught.status === 409) {
        setError(
          "The device configuration changed since it was read, so nothing was written. Reading again.",
        );
        await load();
      } else {
        setError(caught instanceof ApiError ? caught.message : "The write failed");
      }
    } finally {
      setBusy(false);
    }
  }

  async function submit(event: React.FormEvent): Promise<void> {
    event.preventDefault();
    setBusy(true);
    setError("");
    setNotice("");
    try {
      const response = await apiSend<{ url: string | null; tokenSet: boolean }>(
        "/api/voice/hub",
        "POST",
        { url: url.trim(), token },
      );
      setResult(response);
      // The token is never echoed anywhere, so clear it from the page too.
      setToken("");
      await load();
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : "The write failed");
    } finally {
      setBusy(false);
    }
  }

  const plaintext = (url.trim() || hubUrl).toLowerCase().startsWith("http://");

  return (
    <>
      <div className="page-head">
        <h1>Voice</h1>
        <p>What the firmware actually does today, and nothing more.</p>
      </div>

      <ErrorNote>{error}</ErrorNote>
      {config === null && error === "" && (
        <PageLoading label="Reading what this firmware can do." rows={2} />
      )}
      {notice && (
        <Banner tone="info" testId="voice-notice">
          {notice}
        </Banner>
      )}

      <Card title="Hardware capability">
        <Row
          label="Push to talk"
          hint={
            pttCompiled
              ? "Reported as the voice.ptt.v1 capability. The capture path is in this build. That is not the same as working: no audio path on this board has been validated on hardware."
              : "Reported as the voice.ptt.v1 capability. The device does not advertise it, so the capture path is not in this build. The microphone cannot open regardless of the mute setting."
          }
        >
          {pttCompiled ? (
            <span>compiled in</span>
          ) : (
            <span className="gated" data-testid="ptt-absent">
              not compiled in
            </span>
          )}
        </Row>
        <Row
          label="Wake word"
          hint="An abstract interface exists in the firmware but no implementation is ever instantiated. The device advertises no wake word capability."
        >
          <span className="gated">not implemented</span>
        </Row>
        <p className="field-hint" data-testid="wake-word-copy">
          Not implemented in firmware. Off. No ambient listening exists or is
          claimed.
        </p>
      </Card>

      <Card title="Software mute">
        {configLive ? (
          <>
            <Row
              label="State"
              hint="Written to NVS before it is applied, so a device that loses power while muted comes back muted."
            >
              <label className="inline-edit">
                <input
                  type="checkbox"
                  checked={muted === true}
                  disabled={busy || muted === undefined}
                  onChange={(event) => void setMuted(event.target.checked)}
                  data-testid="voice-mute"
                />
                <span data-testid="voice-mute-state">
                  {muted === undefined ? "not reported" : muted ? "muted" : "not muted"}
                </span>
              </label>
            </Row>
            <p className="field-hint">
              It is a software mute, and the firmware label says so. Nothing here
              cuts power to the codec. It defaults to muted, and unmuting does
              not start a recording: the mic gate also needs Wi-Fi connected and
              a hub configured.
            </p>
          </>
        ) : (
          <>
            <Row
              label="State"
              hint="This firmware does not report a configuration API. Change it on the device: Settings > Mute voice."
            >
              <span className="gated" data-testid="mute-unknown">
                unknown remotely
              </span>
            </Row>
            <p className="field-hint">
              {config?.detail ??
                "This firmware does not expose the mute state. Change it on the device: Settings > Mute voice."}
            </p>
          </>
        )}
      </Card>

      <Card title="Hub configuration">
        <p className="field-hint">
          The hub is <code>note4c-firmware/hub/terminal_hub.py</code>, which has
          its own token and reports <code>real_integrations: false</code> in its
          own status.
        </p>

        <Row label="Configured hub">
          <span className="num" data-testid="hub-url-value">
            {hubUrl.length > 0 ? hubUrl : "not configured"}
          </span>
        </Row>
        {/* Named apart from the write-only field below it: this row is what
            the device reports, that one is what you send. */}
        <Row
          label="Token stored on the device"
          hint="A boolean, and the only thing any route reports about the token. Its value is never returned by the device, never stored by the tower, and never written to the audit log."
        >
          <Badge kind="neutral">{hubTokenSet ? "yes" : "no"}</Badge>
        </Row>

        <form onSubmit={submit}>
          <label className="field">
            <span>Hub URL</span>
            <input
              value={url}
              onChange={(event) => setUrl(event.target.value)}
              placeholder="http://<hub-ip>:8770"
              data-testid="hub-url"
            />
            <span className="field-hint">
              Must be a private LAN address. The device uploads audio to it as
              plaintext HTTP.
            </span>
          </label>

          {/* Against the field it is about, not at the bottom of the card:
              the warning is about the value being typed. */}
          {plaintext && (
            <Banner tone="attention" testId="plaintext-warning">
              Audio to the hub travels as plaintext HTTP on the LAN.
            </Banner>
          )}

          <label className="field">
            <span>Hub token</span>
            <input
              type="password"
              value={token}
              onChange={(event) => setToken(event.target.value)}
              placeholder="write only, never displayed"
              autoComplete="new-password"
              data-testid="hub-token"
            />
            <span className="field-hint">
              Write only. The device never echoes it back, this field is never
              populated from the server, and the audit log records only that a
              value was supplied.
            </span>
          </label>

          <Button
            type="submit"
            variant="primary"
            disabled={busy || url.trim().length === 0 || token.length === 0}
            testId="save-hub"
          >
            Write hub configuration
          </Button>
        </form>

        {result && (
          <Banner tone="info" testId="hub-result">
            <Badge kind="neutral">{result.tokenSet ? "configured" : "not configured"}</Badge>
            <span>
              The device accepted the write for {result.url ?? "the given URL"}.
              Configuring the hub does not unmute.
            </span>
          </Banner>
        )}

      </Card>

      <Card title="Later">
        <Row label="Transcription display">
          <span className="gated">not built</span>
        </Row>
        <Row label="Assistant integration">
          <span className="gated">not built</span>
        </Row>
        <p className="field-hint">
          Nothing routes captured audio anywhere except the hub configured
          above, and there is no assistant behind it in this build.
        </p>
      </Card>
    </>
  );
}
