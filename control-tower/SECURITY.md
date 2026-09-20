# Security

## What this program is, in security terms

A local web application. It binds `localhost`, stores everything under a
0700 directory in your home, and talks to one device on your LAN. It has no
cloud component, no accounts, no remote access and no auto-update. If your
machine is compromised, so is this; it defends the things it reasonably can
and does not pretend otherwise.

## What it defends

- **Local binding, enforced rather than assumed.** Loopback only. Binding
  another interface requires both a configured passphrase and
  `NOTE4C_TOWER_ALLOW_LAN=1`; binding all interfaces is refused outright even
  when both of those hold. The policy lives in `src/server/auth/bindGuard.ts`
  and is applied in two places. `tools/tower-serve.ts` — which is what
  `npm start` and `npm run dev` run — applies it before Next starts, so a
  refused host means no socket is ever opened. `src/server/auth/bindStartup.ts`,
  called from `instrumentation.ts`, then looks at what the process is actually
  listening on and exits rather than serving a host the policy refuses; that is
  what stops a hand-run `next start` bound to every interface. The second check
  reports its own confidence on the Diagnostics page, and `undetermined` is a
  value it will show: not every process topology lets it see the socket, and a
  row that said "loopback" about a bind it had not observed would be worth less
  than no row at all.
- **One passphrase, hashed.** Set on first run. It gates every page and every
  mutation. There is no recovery: it is a local tool, and a reset means
  deleting the data directory.
- **CSRF.** Every mutating request must echo a cookie-borne token. A page on
  another origin cannot read that cookie, which is the whole mechanism.
- **SSRF, structurally.** The device address must be a private RFC1918 IPv4
  *literal*. Hostnames are refused, so DNS rebinding is not in the model. The
  port is pinned to 80, the reachable path set is a closed allowlist — the
  exact list is `ALLOWED_PATHS` in `src/server/device/address.ts`, and it is a
  fixed set rather than a prefix rule because the device serves legacy
  unauthenticated gallery routes on the same port — and redirects are never
  followed.
- **Secrets are write-only.** The device token, the voice hub token and any
  Home Assistant token are stored server-side at mode 0600, never returned by
  an API route, never rendered, and never written to the ledger. The audit
  writer redacts any parameter whose name looks like a credential.
- **Destructive actions are typed-gated.** Anything that reaches hardware —
  pushing to the real panel, restarting it, putting it to sleep, pointing the
  tower at it — requires typing an exact word, and the word is shared between
  the dialog and the server route so they cannot drift apart.

## What it does not defend

- **Anyone with your user account.** The data directory is 0700 and the tokens
  are 0600. That is a defence against other users, not against you or against
  code running as you.
- **The LAN link to the device.** The device speaks plain HTTP on port 80. So
  does the optional voice hub, and the interface says so in as many words. On a
  network you do not trust, neither is confidential.
- **The panel's own physical security.** Anything on the screen is readable by
  anyone in the room, and e-paper keeps its image with the power off.

## Reporting something

Please report privately first, and give a reasonable window before disclosing.

**Contact: `<SECURITY_CONTACT_PLACEHOLDER>`** — the maintainer must replace
this with a real address or a GitHub private vulnerability reporting link
before publication. It is left as a placeholder on purpose: publishing somebody
else's personal email address is not a decision this file gets to make.

If no contact is filled in, open a GitHub issue that describes the *impact*
without the exploitable detail, and ask for a private channel.

Please include:

- What an attacker can do, and what they need in order to do it.
- The version from the Diagnostics page's Tower card.
- Steps to reproduce, ideally against the in-repo mock.

### Scope

In scope: anything that lets a request from outside loopback act on the tower,
any path that leaks a stored token, any way to bypass a typed confirmation for
a hardware action, and any way to make the interface report an outcome the
device did not confirm — that last one is a correctness bug and a trust bug at
the same time, and it is treated as seriously here as a memory-safety issue
would be elsewhere.

Out of scope: attacks that require an already-compromised user account,
physical access to an unlocked machine, or a malicious browser extension.

### No bug bounty

There is no money. This is a hobby project given away under an MIT licence. You
will be credited in the release notes if you would like to be, and thanked
sincerely either way.
