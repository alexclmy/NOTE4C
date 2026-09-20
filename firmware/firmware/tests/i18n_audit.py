#!/usr/bin/env python3
"""Audit non-English text in the firmware's user-visible surfaces.

Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
SPDX-License-Identifier: MIT

The goal is an English-only UI. That is a claim, so it gets checked rather than
asserted, and it gets checked against what the device can actually reach.

How reachability is decided
---------------------------
Not by a hand-maintained list of "in scope" files, which is just a way of
declaring the answer. Instead:

  * Files excluded from the build by DASHBOARD_MINIMAL_UI are read out of
    main/CMakeLists.txt (LEGACY_PAGE_SOURCES). Their strings are not in the
    binary, and the audit says so by reading the build config, not by trusting
    a comment.

  * Pages reachable at runtime are derived by scanning for live SwitchPage() /
    SetCurrentPageWithoutRender() calls with `#if 0` blocks stripped, so a page
    whose only navigation is commented out is not counted as reachable.

  * Everything else that is compiled and user-visible is REACHABLE and gates
    the audit. Compiled-but-unreachable files must be listed in
    COMPILED_UNREACHABLE with a stated reason; they are reported every run
    rather than hidden.

What is scanned
---------------
Ordinary string literals, **raw string literals** (an earlier version of this
script missed the 16 KB embedded LAN web page because of that), **byte escapes
inside literals** (a later version reported PASS while four Chinese strings sat
in main/rawdraw/components/voice_wakeup.cc written as "\xe5\xbd\x95...", which
is pure ASCII in the source and Chinese in the binary), and .html assets in
full.

Both of those misses had the same shape: the scanner looked at the source the
way a reader would, and the compiler does not. Each is now covered by a probe
in main() so a scanner that stops working cannot report PASS.

What is deliberately not flagged
--------------------------------
  * Icon font glyphs in the Private Use Area (U+E000-U+F8FF). They are
    pictograms with no language. See the note on NON_ASCII: the class used to
    cover them by accident.
  * Font glyph tables. The Chinese glyphs are the point: user photo titles in
    Chinese must still render.
  * User content. Photo titles, captions and calendar entries belong to the
    user and are never translated by this project.
  * Comments.
  * Log messages, which are not user-visible on the panel. Counted separately
    and never fatal.
  * Multilingual assets that ship a full translation table including English.
    For those the question is not "is there Chinese in the file" but "what does
    the user get by default", so the default language is asserted instead.

Exit status is 1 if any reachable surface contains non-English user-visible
text, or if a multilingual asset does not default to English.

Usage:  python3 tests/i18n_audit.py [-v] [--json]
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path

FW_DIR = Path(__file__).resolve().parent.parent

# Ranges are written as explicit \u escapes on purpose. They used to be pasted
# characters, and one of them did not survive the paste: the entry commented
# "compatibility ideographs" started at U+8C48 (the canonical ideograph) rather
# than U+F900 (the compatibility form), which silently widened the class to
# U+8C48-U+FAFF. That span swallows the whole Private Use Area, where this
# firmware's icon font lives. It went unnoticed only because icon glyphs are
# written as byte escapes, which this scanner could not decode until now.
#
# The Private Use Area is deliberately NOT included: U+E000-U+F8FF carries the
# FontAwesome and Zectrix glyphs (main/components/78__xiaozhi-fonts), which are
# pictograms with no language at all. Flagging them would mean either a
# permanent list of exceptions or a permanently failing audit.
NON_ASCII = re.compile(
    "["
    "\u3000-\u303f"    # CJK symbols and punctuation
    "\u3040-\u30ff"    # Hiragana and Katakana
    "\u3400-\u4dbf"    # CJK extension A
    "\u4e00-\u9fff"    # CJK unified ideographs
    "\uac00-\ud7af"    # Hangul syllables
    "\uf900-\ufaff"    # CJK compatibility ideographs
    "\ufe30-\ufe4f"    # CJK compatibility forms
    "\uff00-\uffef"    # halfwidth and fullwidth forms
    "]"
)

STRING_LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')


def decode_byte_escapes(literal: str) -> str:
    """Render \\xHH and \\NNN escapes as the text they actually encode.

    A string written as "\\xe5\\xbd\\x95\\xe9\\x9f\\xb3" is pure ASCII in the
    source and invisible to a character-class scan, but it is Chinese in the
    binary. Four such strings sat in main/rawdraw/components/voice_wakeup.cc
    while this audit reported PASS. Scanning only what a human would notice is
    the same mistake the raw-string blind spot was.
    """
    if "\\" not in literal:
        return literal
    out = bytearray()
    i = 0
    n = len(literal)
    hex_digits = "0123456789abcdefABCDEF"
    while i < n:
        ch = literal[i]
        if ch == "\\" and i + 1 < n:
            nxt = literal[i + 1]
            if nxt in "xX" and i + 4 <= n - 1 + 1 and all(
                    c in hex_digits for c in literal[i + 2:i + 4]) and len(literal[i + 2:i + 4]) == 2:
                out.append(int(literal[i + 2:i + 4], 16))
                i += 4
                continue
            if nxt in "01234567":
                j = i + 1
                digits = ""
                while j < n and len(digits) < 3 and literal[j] in "01234567":
                    digits += literal[j]
                    j += 1
                out.append(int(digits, 8) & 0xFF)
                i = j
                continue
            out.extend(literal[i:i + 2].encode("utf-8"))
            i += 2
            continue
        out.extend(ch.encode("utf-8"))
        i += 1
    return out.decode("utf-8", errors="replace")
RAW_STRING_LITERAL = re.compile(r'R"([A-Za-z_]*)\((.*?)\)\1"', re.S)
LOG_CALL = re.compile(r"\bESP_LOG[EWIDV]\s*\(")

# Never scanned: glyph data and build output.
EXCLUDE = ("main/components/78__xiaozhi-fonts", "managed_components", "build")

# Compiled into the binary but not reachable by navigation. Every entry needs a
# reason, and the reason is printed on every run.
COMPILED_UNREACHABLE = {
    "main/boards/zectrix-s3-epaper-4.2/FT/factory_test_service.cc":
        "production self-test. Verified dead on this board: "
        "zectrix-s3-epaper-4.2.cc IsFactoryTestMode() returns a hard-coded "
        "false, and EnterFactoryTestFlow() - the only entry point to "
        "FactoryTestService::StartFlow() - has no callers anywhere in the tree. "
        "Left compiled so a future jig build can use it.",
}

# Assets that ship a translation table containing English. For these the audit
# asserts the default language rather than counting foreign-language entries.
MULTILINGUAL_ASSETS = {
    "main/components/78__esp-wifi-connect/assets/wifi_configuration.html": {
        "must_contain": "'en-US';",
        "why": "Wi-Fi provisioning page. Ships ~30 languages including English; "
               "this build forces the default to en-US instead of following the "
               "browser locale.",
    },
}


def strip_if0(text: str) -> str:
    """Remove `#if 0 ... #endif` blocks so dead navigation is not counted."""
    out, depth = [], 0
    for line in text.split("\n"):
        s = line.strip()
        if re.match(r"#if\s+0\b", s):
            depth += 1
            continue
        if depth:
            if re.match(r"#if", s):
                depth += 1
            elif re.match(r"#endif", s):
                depth -= 1
            continue
        out.append(line)
    return "\n".join(out)


def built_sources() -> set:
    """The .cc files actually compiled, read from main/CMakeLists.txt SOURCES.

    Walking the filesystem instead would flag the LVGL page tree, which is
    present on disk but not in SOURCES and therefore not in the binary.
    """
    cmake = (FW_DIR / "main/CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"set\(SOURCES(.*?)^\)", cmake, re.S | re.M)
    if not m:
        raise SystemExit("could not parse SOURCES from main/CMakeLists.txt")
    return {f"main/{p}" for p in re.findall(r'"([^"]+)"', m.group(1))}


def built_headers(sources: set) -> set:
    """Headers transitively included by the compiled sources.

    A header with literals only matters if something compiled pulls it in.
    Resolution is by basename across main/, which over-approximates slightly;
    over-approximating is the safe direction for an audit.
    """
    by_name = {}
    for path in (FW_DIR / "main").rglob("*.h"):
        rel = str(path.relative_to(FW_DIR))
        if any(rel.startswith(d) for d in EXCLUDE):
            continue
        by_name.setdefault(path.name, []).append(rel)

    seen, queue = set(), list(sources)
    while queue:
        rel = queue.pop()
        path = FW_DIR / rel
        if not path.exists():
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for inc in re.findall(r'#include\s+"([^"]+)"', text):
            for cand in by_name.get(Path(inc).name, []):
                if cand not in seen:
                    seen.add(cand)
                    queue.append(cand)
    return seen


def legacy_sources() -> set:
    """Files excluded from the build by DASHBOARD_MINIMAL_UI, per CMakeLists."""
    cmake = (FW_DIR / "main/CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"set\(LEGACY_PAGE_SOURCES(.*?)\)", cmake, re.S)
    if not m:
        return set()
    return {f"main/{p}" for p in re.findall(r'"([^"]+)"', m.group(1))}


def minimal_ui_default() -> bool:
    cmake = (FW_DIR / "main/CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r'option\(DASHBOARD_MINIMAL_UI\s+"[^"]*"\s+(\w+)\)', cmake)
    return bool(m and m.group(1).upper() == "ON")


def reachable_pages() -> set:
    """Pages targeted by a live SwitchPage / SetCurrentPageWithoutRender call."""
    pages = set()
    for path in (FW_DIR / "main").rglob("*.cc"):
        rel = str(path.relative_to(FW_DIR))
        if any(rel.startswith(d) for d in EXCLUDE):
            continue
        text = strip_if0(path.read_text(encoding="utf-8", errors="ignore"))
        for m in re.finditer(
            r"(?:SwitchPage|SetCurrentPageWithoutRender)\s*\(\s*(?:ui::)?RawDrawPageId::(\w+)",
            text,
        ):
            pages.add(m.group(1))
    return pages


def scan_source(path: Path):
    """Yield (line_no, text, is_log) for non-English literals, raw ones included."""
    try:
        text = path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        return

    # Raw string literals first; report by the line the literal starts on.
    for m in RAW_STRING_LITERAL.finditer(text):
        body = m.group(2)
        if NON_ASCII.search(body):
            line_no = text.count("\n", 0, m.start()) + 1
            for frag in sorted(set(re.findall(
                    r"[^<>\"'`={};()\[\],\n]*"
                    + NON_ASCII.pattern +
                    r"[^<>\"'`={};()\[\],\n]*", body))):
                yield line_no, frag.strip(), False

    # Blank out raw literals so the ordinary scan cannot double-report them.
    masked = RAW_STRING_LITERAL.sub(lambda m: '""', text)
    for line_no, line in enumerate(masked.split("\n"), start=1):
        s = line.lstrip()
        if s.startswith("//") or s.startswith("*"):
            continue
        # Drop trailing comments, but only when the "//" is not inside a string
        # (so "http://..." survives). Counting quotes before it is enough here.
        idx = line.find("//")
        while idx != -1:
            if line.count('"', 0, idx) % 2 == 0:
                line = line[:idx]
                break
            idx = line.find("//", idx + 2)
        for m in STRING_LITERAL.finditer(line):
            raw = m.group(1)
            decoded = decode_byte_escapes(raw)
            if NON_ASCII.search(decoded):
                # Report the decoded form: "\xe5\xbd\x95" tells a reviewer
                # nothing, and the point of the report is to be readable.
                yield line_no, decoded, bool(LOG_CALL.search(line))
            elif NON_ASCII.search(raw):
                yield line_no, raw, bool(LOG_CALL.search(line))


def scan_html(path: Path):
    try:
        text = path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        return
    for line_no, line in enumerate(text.split("\n"), start=1):
        if NON_ASCII.search(line):
            yield line_no, line.strip()[:110], False


def check_binary(excluded_files: set) -> dict:
    """Verify the claim against the built artifact, not just the sources.

    Source analysis says these strings are not compiled. The binary is where
    that is either true or not. Skipped silently when no build is present.
    """
    # The artifact this audit is about is whichever one is being proposed for a
    # flash, and that is not always `build/`. NOTE4C_AUDIT_BIN names it; the
    # default stays `build/xiaozhi.bin` so nothing that ran this before
    # behaves differently.
    override = os.environ.get("NOTE4C_AUDIT_BIN")
    binary = Path(override) if override else FW_DIR / "build/xiaozhi.bin"
    if not binary.exists():
        return {"checked": False}

    data = binary.read_bytes()
    leaked, sampled = [], 0
    for rel in sorted(excluded_files):
        path = FW_DIR / rel
        if not path.exists() or path.suffix not in (".cc", ".c"):
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        for m in STRING_LITERAL.finditer(text):
            lit = m.group(1)
            # Short strings can collide with unrelated bytes; require enough
            # length that a hit means something.
            if NON_ASCII.search(lit) and len(lit) >= 3:
                sampled += 1
                if lit.encode("utf-8") in data:
                    leaked.append((rel, lit))
                break

    # A control: if the scan cannot find text we know is in the binary, the
    # scan is broken and its "absent" results mean nothing.
    control = "Pair dashboard"
    control_found = control.encode("utf-8") in data

    return {
        "checked": True,
        "sampled": sampled,
        "leaked": leaked,
        "control_found": control_found,
        "binary": str(binary.relative_to(FW_DIR)),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    # Self-check, in the same spirit as the binary control string: an "absent"
    # result must not be able to come from a scanner that stopped working.
    # Every one of these is a non-English string a C++ file could legally hold.
    for probe in (r"\xe5\xbd\x95\xe9\x9f\xb3", r"\345\275\225\351\237\263", "\u5f55\u97f3"):
        if not NON_ASCII.search(decode_byte_escapes(probe)):
            print(f"ERROR: the scanner failed its own probe {probe!r}; "
                  "a PASS from it would mean nothing.", file=sys.stderr)
            return 2
    # And the other direction: an icon glyph is not non-English text, and a
    # class that flags one is a class that will be worked around rather than
    # trusted.
    for icon in (r"\xef\x80\x93", r"\xee\xa4\x93", r"\xef\x8a\x93"):
        if NON_ASCII.search(decode_byte_escapes(icon)):
            print(f"ERROR: the scanner flags the icon font glyph {icon!r} as "
                  "text; the character class is too wide.", file=sys.stderr)
            return 2

    excluded = legacy_sources()
    minimal = minimal_ui_default()
    pages = reachable_pages()
    sources = built_sources()
    if minimal:
        sources -= excluded
    headers = built_headers(sources)
    compiled = sources | headers

    buckets = {"reachable": {}, "not_built": {}, "compiled_unreachable": {}}
    multilingual = {}

    candidates = []
    for path in (FW_DIR / "main").rglob("*"):
        rel = str(path.relative_to(FW_DIR))
        if any(rel.startswith(d) or f"/{d}/" in f"/{rel}" for d in EXCLUDE):
            continue
        if path.suffix in (".cc", ".c", ".h"):
            candidates.append((rel, path, scan_source))
        elif path.suffix == ".html":
            candidates.append((rel, path, scan_html))

    for rel, path, scanner in candidates:
        if rel in MULTILINGUAL_ASSETS:
            spec = MULTILINGUAL_ASSETS[rel]
            text = path.read_text(encoding="utf-8", errors="ignore")
            multilingual[rel] = {
                "ok": spec["must_contain"] in text,
                "why": spec["why"],
                "expected": spec["must_contain"],
            }
            continue

        hits = list(scanner(path))
        if not hits:
            continue
        # .html assets are served by compiled code, so they are always in play.
        is_built = rel.endswith(".html") or rel in compiled
        if not is_built:
            buckets["not_built"][rel] = hits
        elif rel in COMPILED_UNREACHABLE:
            buckets["compiled_unreachable"][rel] = hits
        else:
            buckets["reachable"][rel] = hits

    def ui_hits(hits):
        return [h for h in hits if not h[2]]

    reachable_ui = sum(len(ui_hits(h)) for h in buckets["reachable"].values())
    unreachable_ui = sum(len(ui_hits(h)) for h in buckets["compiled_unreachable"].values())
    notbuilt_ui = sum(len(ui_hits(h)) for h in buckets["not_built"].values())
    multi_bad = [k for k, v in multilingual.items() if not v["ok"]]

    if args.json:
        print(json.dumps({
            "minimal_ui": minimal,
            "reachable_pages": sorted(pages),
            "reachable": {k: len(ui_hits(v)) for k, v in buckets["reachable"].items()},
            "compiled_unreachable": {k: len(ui_hits(v)) for k, v in buckets["compiled_unreachable"].items()},
            "not_built": {k: len(ui_hits(v)) for k, v in buckets["not_built"].items()},
            "multilingual": multilingual,
        }, indent=2))
    else:
        print(f"DASHBOARD_MINIMAL_UI default: {'ON' if minimal else 'OFF'}")
        print(f"Pages reachable via live navigation: {', '.join(sorted(pages)) or 'none'}")

        print("\n=== REACHABLE (gates this audit) ===")
        if not buckets["reachable"]:
            print("  no non-English text found")
        for rel, hits in sorted(buckets["reachable"].items()):
            ui, logs = ui_hits(hits), [h for h in hits if h[2]]
            print(f"  {rel}: {len(ui)} user-visible, {len(logs)} log-only")
            for line_no, literal, _ in ui[:40]:
                print(f"      {line_no}: {literal[:90]}")

        print("\n=== MULTILINGUAL ASSETS (English default asserted) ===")
        for rel, info in sorted(multilingual.items()):
            print(f"  [{'ok' if info['ok'] else 'FAIL'}] {rel}")
            print(f"      {info['why']}")
            if not info["ok"]:
                print(f"      expected to find: {info['expected']}")

        print("\n=== COMPILED BUT UNREACHABLE (reported, not gating) ===")
        for rel, hits in sorted(buckets["compiled_unreachable"].items()):
            print(f"  {rel}: {len(ui_hits(hits))} user-visible")
            print(f"      reason: {COMPILED_UNREACHABLE[rel]}")

        print("\n=== NOT BUILT (not in CMakeLists SOURCES for this configuration) ===")
        for rel, hits in sorted(buckets["not_built"].items()):
            print(f"  {rel}: {len(ui_hits(hits))} literals, not compiled into the binary")
            if args.verbose:
                for line_no, literal, _ in ui_hits(hits)[:10]:
                    print(f"      {line_no}: {literal[:80]}")

        print(f"\nReachable user-visible non-English literals : {reachable_ui}")
        print(f"Compiled-but-unreachable (documented)       : {unreachable_ui}")
        print(f"Excluded from the build entirely            : {notbuilt_ui}")

    binary_result = check_binary(excluded if minimal else set())
    binary_bad = False
    if not args.json:
        print("\n=== BUILT ARTIFACT ===")
        if not binary_result["checked"]:
            print("  no build/xiaozhi.bin present; source-level result only")
        elif not binary_result["control_found"]:
            print("  FAIL: control string not found in the binary, so the "
                  "absence results below cannot be trusted")
            binary_bad = True
        elif binary_result["leaked"]:
            print(f"  FAIL: {len(binary_result['leaked'])} excluded strings are "
                  f"present in {binary_result['binary']}")
            for rel, lit in binary_result["leaked"][:10]:
                print(f"      {rel}: {lit[:60]}")
            binary_bad = True
        else:
            print(f"  {binary_result['binary']}: {binary_result['sampled']} sampled "
                  f"strings from excluded files, none present; control string found")

    if reachable_ui or multi_bad or binary_bad:
        if multi_bad:
            print(f"\nFAIL: multilingual asset does not default to English: {multi_bad}")
        if reachable_ui:
            print("\nFAIL: reachable UI still contains non-English text.")
        return 1
    print("\nPASS: every reachable user-visible surface is English.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
