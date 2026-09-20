# Dashboard frame storage: what it guarantees, and what it does not

## The design

Two fixed records, `/spiffs/dash0.rec` and `/spiffs/dash1.rec`, on the existing
`assets` SPIFFS partition. Each holds one self-describing record:

```
offset  size  field
0       8     magic "N4CDASH1"
8       4     record version
12      4     sequence number
16      4     payload length (must be 30000)
20      4     flags (bit 0 = this payload was composed on the device)
24      32    SHA-256 of the payload
56      4     source timestamp from the pusher
60      4     CRC-32 over bytes 0..59
64      30000 payload
```

A write only ever targets the slot that is **not** currently being displayed,
and is confirmed by reading the slot back and re-validating it. On boot both
slots are validated independently and the valid one with the highest sequence
wins.

The header CRC is checked before any field inside the header is trusted, so a
corrupted `payload_len` cannot send the reader out of bounds.

### The one bit in the flags word

`flags` bit 0 (`dashboard::kFlagOriginLocal`) says the frame in this slot was
composed by the device rather than pushed by the tower. It is in the header
rather than inferred, because the tower asks "what is on the glass" after a deep
sleep — e-paper keeps its image across the reboot while nothing in RAM does — and
a device that had to guess would answer with the more flattering of the two.

**This is not a change to the record format.** The word was already reserved at
offset 20, already written as zero by every previous build, and already inside
the header CRC. So a record written by the hardware-validated build reads back
here as origin = tower, which is what it was, and a record written by this build
loads unchanged in the older one. Nothing else about the layout, the digest or
the A/B sequence moved: this store holds the panel, and the provenance report was
not worth a structural edit to it.

Two consequences worth stating. A flipped bit in that word invalidates the whole
record rather than silently reporting the other origin, because the CRC covers
it — which matters, since the arbitration rule in `autonomy_policy.h` acts on
this bit. And the stored origin describes *the glass* only while the frame that
completed a refresh is still the record the store holds; see
`DisplayedFrameIsStoredFrame()` and the `displayed_origin` field in
`DASHBOARD_API.md`, which report `unknown` rather than guessing.

## What this genuinely protects against

These are covered, and covered by tests that drive the real
`dashboard_slot.cc` (`tests/host/test_dashboard_slot.cc`):

- **A write interrupted partway through**, at the application level. The write
  is cut at 493 different byte offsets across the record and the next boot is
  simulated; every offset yields a complete frame, old or new, never a mixture.
- **A write that reports success but did not fully land.** Read-back
  verification catches it and the previous frame stays active.
- **A write that reports failure.** Previous frame stays active.
- **Corruption of the newer record.** One flipped payload bit invalidates that
  slot and boot falls back to the older complete frame.
- **Corruption of any header field.** Each is corrupted individually; all
  invalidate the slot.
- **Both records corrupt.** Reported as "no frame" rather than displaying
  garbage.
- **A record truncated below one header length.** Rejected without reading past
  the buffer (checked under ASan).

## The second store: the autonomy profile

`/spiffs/prof0.rec` and `/spiffs/prof1.rec`, on the same partition, with the
same header layout and the same A/B discipline — write the inactive slot, read
it back, let the checksum rather than the filesystem decide whether the record
is there. Two differences, both deliberate:

- the magic is `N4CPROF1`, so a frame in a profile slot (or the reverse) fails
  validation and reads as "nothing stored" instead of as a document of garbage.
  That is what makes it safe for both stores to share a filesystem;
- the payload is variable-length, bounded above by what the device will parse,
  because a profile is a JSON document rather than a fixed-size frame.

It is implemented in `main/common/record_slot.cc`, which is a **second**
implementation of the discipline above rather than a refactoring of the first.
`dashboard_slot.cc` holds the panel, is validated on hardware, and is not
something a profile feature should be editing; the price is that SHA-256, CRC-32
and the header layout exist twice in this firmware. Both copies are tested:
the frame's by `tests/host/test_dashboard_slot.cc`, the profile's by
`tests/host/test_autonomy_profile.cc`, which cuts a profile write at every byte
offset the same way and checks that no cut ever yields a torn document.

Everything in the section below applies to this store too: it is on the same
filesystem, in the same partition, with the same exposure to a SPIFFS-level
failure.

## The third store: the forecast cache

`/spiffs/wthr0.rec` and `/spiffs/wthr1.rec`, magic `N4CWTHR1`, a fixed 108-byte
payload. The same `record_slot.cc` implementation as the profile, with a
different spec — fixed-length, like the frame, because the encoder always writes
all twenty-four hour slots whether or not they are populated, so any other length
is a corrupt record rather than a shorter forecast.

It is persisted rather than held in RAM for two reasons, and the second is the
one that pays for it:

- a wake that finds no Wi-Fi, or finds Open-Meteo returning 503, still has to
  draw something, and the only honest something is the last forecast the device
  actually observed, labelled with its age;
- a wake with a cached forecast that is not yet due for a refetch never brings
  the radio up at all. That is the single largest energy saving the autonomy
  feature offers, and it is this record that enables it.

A corrupt or absent record reads as "no forecast", never as a forecast of zeroes.
A panel showing 0.0 °C for every hour of tomorrow is worse than a panel saying it
does not know. `tests/host/test_weather_cache.cc` drives it, including the
write-cut walk.

### Write volume, counted rather than assumed

A locally composed frame is persisted **only if it differs** from what is
displayed: the store returns `kDuplicate` for a payload it already holds and
writes nothing. Worst case at the 15-minute wake floor is ≤ 96 frame writes of
30 KB a day, about 2.9 MB/day against an 8 MB partition — acceptable, but
counted. The profile is written only on an accepted PUT, and the forecast at most
once per `min_fetch_interval_min` (30 minutes by default).

These are arithmetic, not measurements. Nothing here has been observed on a
device's flash wear counters.

## What this does NOT protect against

This section exists because the design is easy to oversell. It is an
application-level scheme on top of a filesystem that offers no transactional
guarantees, and the following are outside its reach:

**1. SPIFFS-level corruption.** Both records live on one SPIFFS filesystem in
one partition. The partition table is deliberately unchanged, so there is
nowhere else to put them. If SPIFFS's own metadata — its object index, page
headers, or free-page bookkeeping — is damaged by a power cut during a garbage
collection or index rewrite, both records can be lost together. Having two
records does not help when the thing that fails is the filesystem underneath
them both. The scheme protects against a torn *record*; it cannot protect
against a broken *filesystem*.

**2. Flash wear.** Each stored frame writes ~30 KB. SPIFFS wear-levels within
the partition, but nothing here tracks erase counts or detects a block that has
worn out. At one update per minute this writes roughly 43 MB/day into an 8 MB
partition; the endurance question is real and is not answered here. The read
path re-verifies checksums on every read, so a worn block is *detected* rather
than silently served — but detection is not prevention.

**3. Physical flash failure.** Read-back verification happens immediately after
writing, through the same cache and the same driver that just wrote the data.
It confirms the write path was self-consistent. It does not prove the bytes are
durably in the flash cells, and it cannot detect decay that happens later, at
rest.

**4. The tests model application-level truncation only.** `FakeSlotIo` cuts
writes short and flips bytes. That is a faithful model of "the process stopped
partway through a write" and of "a byte came back wrong". It is **not** a model
of SPIFFS internals, of a power cut during a SPIFFS garbage-collection pass, or
of flash cells degrading. No test here has ever run against real flash. The
sweep proves the *logic* is sound given a filesystem that behaves; it says
nothing about how this filesystem behaves in practice on this hardware.

**5. None of it has run on the device.** Everything above is reasoning plus
host tests. `fsync()` on SPIFFS via the ESP-IDF VFS is assumed to push data
through the cache; that assumption is untested here.

## Bounds

Storage is bounded at exactly two records, about 60 KB total, no matter how many
frames arrive. Verified by a 200-push test asserting both files stay at
`kRecordBytes`.

Before each write the layer checks `esp_spiffs_info()` for free space plus an
8 KB margin and refuses early if it will not fit, because `fopen(..., "wb")`
truncates the target immediately and discovering `ENOSPC` afterwards means the
rollback copy was destroyed for nothing. A write that fails partway has its
stump `unlink()`ed — the stump would fail its own checksum and be ignored, but
leaving 30 KB of garbage on a full filesystem would make the next attempt fail
too.

`GET /api/v1/dashboard/status` reports `storage.write_failures`,
`storage.read_failures`, `storage.spiffs_total` and `storage.spiffs_used`, so a
filesystem that is quietly failing is visible from the Mac rather than only on a
serial console nobody is watching.

## If durability matters more than it does today

The honest upgrade path, in order of cost:

1. Watch `storage.write_failures` and the SPIFFS used/total figures over time.
   Real data beats speculation about wear.
2. Keep a third copy in NVS (which has its own wear levelling and a different
   failure mode) as a last-resort fallback for the boot frame.
3. Give the dashboard its own small partition, so a SPIFFS failure in `assets`
   cannot take the boot frame with it. This changes the partition table, which
   this project deliberately does not do, and would require a full backup and a
   migration plan.
