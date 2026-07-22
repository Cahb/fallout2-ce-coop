# Account identity & dynamic membership — design ruling

**Status:** ruling of record (Fable design pass, adopted). Builds on
`PLAYER_SHEET_DESIGN.md` (per-actor sheets, the slot-encoded sheet pid) and the
committed N-actor disk save appendix (`player_sheet.cc`
`playerActorAppendixSave/Load`). Implementation guide + staging + trap list.

**Goal.** A player connects by ACCOUNT NAME and plays their own character —
their name, stats, leveled-up progression persisted across sessions — instead of
claiming a clone shell by slot index. The account name is the primary key,
dissolving both `F2_SERVER_PLAYERS=<count>` (accounts appear dynamically) and
`claim <slot>`-by-index (bind by name; slot is an internal handle).

**Account name vs character name are SEPARATE ROLES** (may collapse to the same
string). Account name = who the human is (+ ownership token); lives only in the
account table. Character in-game name = `gDudeName` / the per-slot
`gPlayerActorNames` row; shown in-world. At first login of a new name, initialize
character name := account name (else every clone shell greets as the host).

--------------------------------------------------------------------------------
## 1. Data model — name is the durable key; the SLOT is persisted, append-only

⚠ **The slot index cannot be ephemeral.** The sheet pid ENCODES the slot
(`playerActorSheetPid(slot) = 0x1FFFF00 + slot`), and that pid is baked into every
serialized extra body in the disk appendix. Sheets, perk rows, XP, and the netId
walk order are all slot-keyed. If a reload mapped a name to a different slot, the
reloaded body's pid would disagree with its slot and the only guard against
scribbling on another player's — or the host's — sheet is the §7.1 pid-dispatch
invariant. So:

| Layer | Key → value | Lifetime | Home |
|---|---|---|---|
| account table (NEW) | name → slot (+ token) | persisted in appendix; APPEND-ONLY | `src/server_accounts.{h,cc}`, f2_core |
| registry | slot → `Object*` | process; rebuilt at load in slot order | `src/server_players.cc` (unchanged) |
| bindings | sessionId → slot | connection | `src/server_control.cc` `gBindings` (unchanged) |

- **Slots allocated first-seen order; never reordered, recycled, or compacted** —
  even for a name that never returns. Update the "membership is FIXED after boot"
  header comment to "APPEND-ONLY; a slot, once a name's, is that name's forever
  within a save lineage."
- Reconnect: `login <name>` → table lookup → slot → bind if free; deny if bound to
  a live session (token policy may later allow takeover).
- Unknown name → allocate next slot, record name→slot, bind.
- ~~**Unknown names never get slot 0** (host / gDude).~~ **REVERSED (owner, live
  play):** the first unknown name DOES take slot 0. Host-only screens — worldmap
  travel above all — are gated on the host slot, so leaving it unbound means
  nobody can travel. Slot order is preference order, exactly as for bare `claim`
  (which stays as the host/dev affordance). Consequence: `create` must be able to
  land on slot 0, i.e. rewrite gDudeProto; what protects an established host
  character is the rule that an EXISTING account never carries a creation spec.
- `server_accounts` must be f2_core (serialized by `player_sheet.cc` / driven by
  `savegame.cc`) — same data-only precedent as `server_players`.

## 2. Save appendix v2 — stay slot-keyed, add a name table, bump the magic

Do NOT re-key rows by name (bodies carry slot-encoded pids; loader registers in
slot order before applying the sheet block — a reorder step there is exactly the
window a misapplied row scribbles on live `gDudeProto`).

- **v2 magic `'PAC2'`**: `extras` count, then a **name/token table** (slot 0..count
  incl. host so host ownership persists), then the unchanged body rows, then the
  unchanged `playerSheetBlockWrite(stream, 1)`.
- Loader accepts BOTH: `'PACT'` (v1) → empty name table → slots are "unowned",
  claimable by legacy `claim`, name-attachable on first `login`. `'PAC2'` → restore
  names. EOF → vanilla save, no appendix. **N==1 writes nothing at all** →
  byte-identity preserved.
- Load-time cross-check: registered actor's pid == `playerActorSheetPid(slot)` for
  every appendix slot (catches any slot remap; fail loud).

## 3. Mid-session spawn (STAGE 1) — a login-spawn LATCH, drained only at the safe point

A login-CREATED actor is a registry mutation. The serve loop's dialog/movie/combat
barriers run `pollInbound` but deliberately skip `acceptPending` because a
rebaseline there re-mints netIds and can free `gGameDialogSpeaker`. A `login`
arriving inside a barrier would smuggle that hazard through the control plane.

**Ruling:** `login` for an unknown name only RECORDS a pending-spawn intent (name,
sessionId). The latch drains at the main-phase beat tail (the existing
`acceptPending`/`serverRequestRebaseline` site), only when NOT in combat/dialog/
movie. Strict order:

1. Seed the new slot's sheet rows — **per-slot seed variants** (all six classes),
   from the host's CURRENT sheet (stage-1 semantics). NEVER the bulk seeders (trap 1).
2. `_obj_copy` from gDude → assert `OBJECT_NO_SAVE|OBJECT_NO_REMOVE`, clear HIDDEN,
   strip sid, place on free tile, `pid = playerActorSheetPid(slot)` LAST — the exact
   `serverSpawnExtraActors` order, extracted into shared `serverSpawnPlayerActor(slot)`.
3. `playerActorRegister` — never yield between register and step 4.
4. `serverRequestRebaseline()` → existing tail re-mints netIds, emits blob + baseline
   (slot-order walk includes it) + roster.
5. THEN `gBindings[slot] = sessionId`, greet. Binding AFTER the baseline guarantees
   the roster row's netId is the generation every viewer just loaded.

Known names skip 1–4 (actor exists) — that's today's claim + a table lookup.

## 4. Ownership token — CLIENT-supplied (reverses the earlier server-minted plan)

The wire is one broadcast buffer with no per-session channel past the connect
preamble; a server-minted token handed back would leak to every viewer. So:
`login <name> <token>` (token from `F2_PLAYER_TOKEN` / client-local file); server
stores it (hashed fine) in the account row, persists it, compares on later logins
**only when `F2_REQUIRE_TOKEN=1`** (default OFF = first-claimer-wins). Field exists +
persists from day one; enforcement is one `if`. Server-minting remains possible
ONLY via the per-session preamble (the one private window) if ever required.
⚠ This reverses the prior "server-minted" ruling in the coop-character-identity
notes — deferred owner decision; tokens are stage-3, default OFF.

## 5. Staging (each independently gate-checkable; run check.sh every stage)

- **Stage 0 — name-keyed binding over PRE-SPAWNED slots.** Keep `F2_SERVER_PLAYERS`
  boot spawn. `login <name>`: known→its slot; unknown→next unnamed pre-spawned slot,
  record. Account table + appendix v2. Ships stable identity, reconnect-by-name,
  persistence with ZERO new spawn-timing risk (no mid-session registry mutation, so
  none of §3 yet). Live-only; goldens/N==1 untouched.
- **Stage 1 — dynamic spawn at login** (§3 latch + per-slot seeds). `F2_SERVER_PLAYERS`
  becomes optional but KEPT WORKING (gate scripts + wire-combat checks depend on
  pre-spawn — do not delete).
- **Stage 2 — creation content** replaces the seed source in §3 step 1.

--------------------------------------------------------------------------------
## 6. Trap list (ranked)

1. **HIGH — bulk seeders re-run at login wipe live players.** `protoPlayerActorSheetsSeed`
   / `perkPlayerActorSeedRanks` / … seed ALL slots from the host; on the dynamic path
   they reset every already-diverged extra to the host's values. Silent, total
   progression loss, no gate catches it (goldens N==1). Stage 1 REQUIRES per-slot seed
   variants — all six classes in one op, asserted `slot > 0` ("seed all six or chimera").
2. **HIGH — login-spawn inside a barrier phase** frees `gGameDialogSpeaker` / mutates the
   world under a combat session. The latch-and-defer (§3) is mandatory. Test: scripted
   `login` via `F2_SERVER_ACTIONS` at a tick inside a dialog.
3. **HIGH — any slot remap across save/load** desyncs slot-encoded pids from registry
   slots → §7.1 pid-check aborts (best case) or a row lands on `gDudeProto` (worst).
   Never compact/sort slots. Guard: comment + load-time pid cross-check (§2).
4. **MED-HIGH — N==1/golden drift via boot-path changes.** Registration must fire only on
   a live named connect; the account table stays empty on probe/golden paths. Degeneracy
   is not the gate — run check.sh every stage.
5. **MED — `claim <name>` parser aliasing.** `sscanf("%31s %d")` makes `claim bob` behave
   as bare `claim` (binds arbitrary slot, looks successful, wrong identity). Use the new
   `login` verb; never teach `claim` strings. Keep the greppable
   `"control claimed by session %d (slot %d)"` stderr line on the login path or
   `check_control.sh` / `check_wire_combat.sh` go dark.
6. **MED — token on the broadcast wire** (see §4).
7. **MED — `_obj_load_player_actor` strips `OBJECT_NO_REMOVE`.** Re-asserted in
   `playerActorAppendixLoad`; stage-1 spawn uses `_obj_copy` (safe). But stage-2+ "load a
   returning character's body from a per-account file" will reuse that loader and
   reintroduce the dangling-pointer-on-map-change bug. Put the re-assert in ONE shared
   "adopt server player actor" helper so no call site can forget it.
8. **LOW-MED — slot exhaustion is permanent per lineage** (append-only → 8 names ever;
   `bob` vs `Bob` forks a character and burns a slot). Loud stderr on allocation; consider
   case-normalizing; do NOT solve with recycling (trap 3). Widening the cap keeps
   `kPlayerActorSheetPidBase` past every critters.lst length.
9. **LOW — greeting/roster identity.** `serverGreetClaimant` prints the character name;
   once account≠character, join/leave logs should use the account name, in-world text the
   character name. Decide once at stage 0 so gate-script log lines are stable.

## Critical files
- `src/server_control.cc` — `login` verb, bindings, greet/roster
- `src/server_players.{h,cc}` — append-only registry contract
- `src/server_accounts.{h,cc}` — NEW: name→slot(+token) table
- `src/player_sheet.cc` — appendix v2 (magic bump + name/token table)
- `src/server_boot.cc` — extract `serverSpawnPlayerActor(slot)`; per-slot seeds
- `src/server_main.cc` — login-spawn latch drain at the acceptPending/rebaseline site
- `src/main.cc` (~1110) — viewer reads `F2_PLAYER_NAME` → `login <name>` else `claim`
