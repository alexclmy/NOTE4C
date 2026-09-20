import type { Metadata, Viewport } from "next";
import type { ReactNode } from "react";
import "@/ui/tokens.css";
import "@/ui/base.css";
import { fontVariables } from "@/ui/fonts";

export const metadata: Metadata = {
  title: "NOTE4C Control Tower",
  description: "Local control tower for the NOTE4C e-paper panel",
};

export const viewport: Viewport = {
  width: "device-width",
  initialScale: 1,
  // The interface is a tool, not a document: the bottom tab bar and the sticky
  // action bar both need the browser chrome to stop resizing under them.
  viewportFit: "cover",
};

export default function RootLayout({ children }: { children: ReactNode }) {
  // The font classes carry --font-title-local and --font-mono-local, which
  // tokens.css names first in its two stacks. Set on <html> so they are in
  // scope for anything, including an overlay rendered outside the shell.
  return (
    <html lang="en" className={fontVariables}>
      <body>{children}</body>
    </html>
  );
}
