/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
  poweredByHeader: false,
  // The floating dev indicator sits over the left rail and over QA screenshots.
  devIndicators: false,
  eslint: { ignoreDuringBuilds: true },
  typescript: { ignoreBuildErrors: false },
  // Verifying a build while a production `next start` is serving this same
  // directory would overwrite the .next it is reading chunks out of, and break
  // the running server. NEXT_DIST_DIR lets a check build somewhere harmless.
  // Unset, this is exactly the previous behaviour.
  //
  // Do not set this by hand for a one-off check: `next build` also rewrites
  // next-env.d.ts and reformats tsconfig.json to point at whatever dist
  // directory it used, so a bare `NEXT_DIST_DIR=... next build` leaves two
  // tracked files referencing a scratch directory. Use `npm run build:check`,
  // which builds into a scratch tree and then puts both files back.
  distDir: process.env.NEXT_DIST_DIR ?? ".next",
};

export default nextConfig;
