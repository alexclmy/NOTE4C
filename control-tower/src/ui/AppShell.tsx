"use client";

import Link from "next/link";
import { usePathname, useRouter } from "next/navigation";
import { useEffect, useRef, useState, type ReactNode } from "react";
import { apiSend } from "./api";
import { DeviceChip } from "./DeviceChip";
import { PageDeviceStateProvider, usePageDeviceStatePresence } from "./pageDeviceState";
import { ToastProvider } from "./Toast";
import { useDeviceState } from "./useDeviceState";

/**
 * Four entries, and there are five routes.
 *
 * Voice is deliberately not one of them. It is an experiment that has never
 * been validated on hardware and configures nothing by default, so a permanent
 * quarter of the navigation is more than it has earned; it is reached from the
 * card that describes it, on Device, where the sentence explaining what it is
 * can sit next to the link. The route is unchanged and remains linkable.
 *
 * "Advanced" rather than "Diagnostics" because that is what it is from the
 * outside — a place you go when something is wrong — and it is drawn quieter
 * than its three siblings for the same reason.
 */
const NAV = [
  { href: "/overview", label: "Overview", quiet: false },
  { href: "/dashboards", label: "Compositions", quiet: false },
  { href: "/device", label: "Device", quiet: false },
  { href: "/diagnostics", label: "Advanced", quiet: true },
] as const;

export function AppShell({
  children,
  simulated,
  deviceLabel,
  density,
}: {
  children: ReactNode;
  simulated: boolean;
  deviceLabel: string;
  density: "comfortable" | "compact";
}) {
  const pathname = usePathname();
  const router = useRouter();
  const [densityValue, setDensityValue] = useState(density);
  const bannerRef = useRef<HTMLDivElement>(null);

  /*
   * Publish the banner's real height as a CSS variable.
   *
   * The header is `position: sticky; top: var(--banner-height)`, and the
   * SIMULATED banner sits above it and is sticky too. Without the offset the
   * header would stick to the top of the viewport and slide *under* the banner
   * as soon as the page scrolled, leaving the banner's bottom border cutting
   * across the nav row. Guessing a fixed height would be wrong the moment the
   * sentence wraps, which it does between 760 px and about 1100 px, so it is
   * measured.
   */
  useEffect(() => {
    const node = bannerRef.current;
    const root = document.documentElement;
    if (!node) {
      root.style.removeProperty("--banner-height");
      return;
    }
    const publish = (): void => {
      root.style.setProperty(
        "--banner-height",
        `${Math.round(node.getBoundingClientRect().height)}px`,
      );
    };
    publish();
    const observer = new ResizeObserver(publish);
    observer.observe(node);
    return () => {
      observer.disconnect();
      root.style.removeProperty("--banner-height");
    };
  }, [simulated]);

  // Shared across the shell and every page: one read, one word, one place it
  // is decided. See src/ui/useDeviceState.ts.
  const device = useDeviceState();

  // Kept so a page can still say whether it is carrying the device state
  // itself. Nothing in the shell duplicates a page's controls any more — the
  // rail that used to is gone — but the registration is what Overview and
  // Device use to avoid stacking two readings of their own.
  const pageState = usePageDeviceStatePresence();

  /*
   * "Compositions" is current for the editor too.
   *
   * `/dashboards/<id>/edit` is not a fifth place; it is what you are doing
   * inside the gallery, and a nav that highlighted nothing while a person was
   * editing left them with no indication of where they were. Prefix matching
   * rather than an exact compare, and it is exactly why `current` takes the
   * href rather than comparing against `pathname` at each call site.
   */
  const current = (href: string): "page" | undefined =>
    pathname === href || pathname.startsWith(`${href}/`) ? "page" : undefined;

  async function signOut(): Promise<void> {
    try {
      await apiSend("/api/auth/logout", "POST");
    } finally {
      router.push("/login");
      router.refresh();
    }
  }

  async function toggleDensity(): Promise<void> {
    const next = densityValue === "compact" ? "comfortable" : "compact";
    setDensityValue(next);
    try {
      await apiSend("/api/state", "PATCH", { density: next });
    } catch {
      // The preference is cosmetic and already applied locally; a tower that
      // refused to store it is not worth an error banner over.
    }
  }

  const utilities = (
    <>
      <button
        className="btn btn-quiet"
        onClick={() => void toggleDensity()}
        data-testid="density-toggle"
        aria-pressed={densityValue === "compact"}
      >
        {densityValue === "compact" ? "Comfortable spacing" : "Compact spacing"}
      </button>
      <button className="btn btn-quiet" onClick={signOut} data-testid="sign-out">
        Sign out
      </button>
    </>
  );

  return (
    <ToastProvider>
      <a className="skip-link" href="#main">
        Skip to content
      </a>

      {simulated && (
        <div
          className="simulated-banner"
          data-testid="simulated-banner"
          ref={bannerRef}
        >
          <span className="banner-word">SIMULATED&nbsp;DEVICE</span>
          {/*
            One sentence at every width, which it did not used to be: the old
            wording ran to three lines on a 375 px screen and pushed the thing
            it is a label for — the panel — off the first screenful, so there
            was a long version and a short one chosen by a media query. This
            one is short enough to need neither, and it keeps the load-bearing
            words: not the simulator in general, the mock that is in this
            repository, and not your panel.
          */}
          <span>Actions here touch the in-repo mock, not your panel.</span>
        </div>
      )}

      <div className="shell" data-density={densityValue}>
        <header className="app-header">
          <div className="header-inner">
            <Link className="brand" href="/overview">
              <span className="brand-mark" aria-hidden="true" />
              <span className="brand-name">NOTE4C</span>
              <span className="brand-sub">CONTROL&nbsp;TOWER</span>
            </Link>

            {/*
              The chip, and only the chip.
              The header holds the two things that are true of every page: what
              this is, and what the device is doing. Sign out and the density
              toggle are neither — they are things you do once and on the way
              out — so they live at the foot of the page, once, at every width.
              The rail this replaces kept them at the top and then hid them
              below 760 px, which meant the only way to sign out of the tower
              on a phone was to make the window wider.
            */}
            {device.input && (
              <DeviceChip
                reading={{
                  input: device.input,
                  observed: device.observed ?? true,
                  nextWakeLabel: device.nextWakeLabel ?? null,
                  address: device.address ?? null,
                  simulated: device.simulated ?? simulated,
                }}
              />
            )}
          </div>

          {/*
            The nav row. A `<nav aria-label="Main">` here and another on the tab
            bar, and exactly one of the two is in the accessibility tree at any
            width because the other is `display: none` — which getByRole and a
            screen reader both honour. Labelling one container that is always
            present would mean a landmark called "Main navigation" containing no
            links on the width where the links are elsewhere.
          */}
          <nav className="top-nav" aria-label="Main">
            {NAV.map((item) => (
              <Link
                key={item.href}
                href={item.href}
                aria-current={current(item.href)}
                data-quiet={item.quiet ? "true" : undefined}
              >
                {item.label}
              </Link>
            ))}
          </nav>
        </header>

        <main className="main" id="main">
          <PageDeviceStateProvider value={pageState.register}>
            {children}
          </PageDeviceStateProvider>

          {/*
            One copy of each, at every width.
            The address is a label rather than a control, and it is on Device
            and in Advanced either way; it is here because the foot of a page
            is where a person looks for "what is this thing pointed at".
          */}
          <div className="page-foot">
            <span title="The address the tower would talk to">{deviceLabel}</span>
            {utilities}
          </div>
        </main>
      </div>

      <nav className="tabs" aria-label="Main">
        {NAV.map((item) => (
          <Link
            key={item.href}
            href={item.href}
            aria-current={current(item.href)}
            data-quiet={item.quiet ? "true" : undefined}
          >
            <span className="tab-dot" aria-hidden="true" />
            {item.label}
          </Link>
        ))}
      </nav>
    </ToastProvider>
  );
}
