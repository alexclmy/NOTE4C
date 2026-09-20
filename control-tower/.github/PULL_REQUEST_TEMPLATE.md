## What this changes

<!-- One paragraph. The reason, not just the diff. -->

## Why

<!-- What was wrong, or what could not be done before. If it fixes a defect,
     say what the defect let through. -->

## What I ran

- [ ] `npm run typecheck`
- [ ] `npm run test`
- [ ] `npm run build:check`
- [ ] `npm run test:e2e`

## Hardware

- [ ] This change does not touch the real-device path.
- [ ] It does, and I executed the following on a physical panel:

<!-- Be exact. "Should work on the device" is a fine thing to say; writing it
     as though it had been tested is not. -->

## Checklist

- [ ] No new runtime dependency, or the pull request argues for it.
- [ ] Nothing is fetched from a third party at runtime.
- [ ] No value is rendered that the tower cannot actually know.
- [ ] Red is still only destructive/failed; yellow is still only
      pending/stale/uncertain.
- [ ] Existing `data-testid` attributes are preserved, or their tests moved in
      the same commit.
- [ ] Comments explain the *why* of anything non-obvious.
