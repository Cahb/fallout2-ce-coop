# Combat / character-math `gDude` audit (co-op per-actor correctness)

**What this is.** A complete sweep of every combat / character *calculation* that reaches for
`gDude` (slot 0 / host) instead of the acting or affected player. Companion to
`PLAYER_SHEET_DESIGN.md` §8 (which covered perks/XP/stats/traits/names). This one covers the
**math**: to-hit, damage dealt/taken, armor, crits, hit-mode, heal, poison/rad, sequence.

Classify against the A/B/C/D taxonomy (`PLAYER_SHEET_DESIGN.md` / memory
`per-player-character-sheet`):
- **A** local-player (camera/HUD/inventory screen) → `gDude` correct, leave.
- **B** host / slot-0 by ruling → keep, express as a host predicate.
- **C** the **acting/affected** player (singular, whoever did it) → thread the subject
  (`attacker`/`defender`/`critter` param, or `gContextActor`/`scriptContextDude`). **NOT
  `playerActorIs`** — that is true for *all* players and fires N times.
- **D** per-actor property → `playerActorIs` + subject.
- The dangerous misclass is **C→D**.

## ►► The governing mechanism — read this first, it shrinks the problem

`ServerActorScope` (`server_players.cc:197-233`) **rebinds the global `gDude` pointer itself** to
the acting player-actor for the lifetime of the scope. Scopes are held during:
- a player's own **combat turn beat** (`combat.cc:3874-3882`), and
- the entire **server-control verb dispatch** (`server_control.cc:1763`).

**Consequence:** any `== gDude` / bare-`gDude` read that executes *inside* one of those scopes is
**already correct** for whichever player is acting — host or extra. This is a *documented
stopgap*, not a parameter thread ("threading an actor parameter through all of that is the
post-v1 refactor" — `combat.cc:3878`). So the real bugs are **not** the 1295 `gDude` refs; they
are the sites that run **outside** any scope:

1. **Script opcodes** (fire from the interpreter, no combat scope).
2. **Trait-modifier math** (`trait.cc` — pure functions handed a `subject` they ignore).
3. **AI / monster-turn defender-and-attacker math** — during an AI turn **no scope is held**,
   `gDude` sits at the passive-sim anchor (nearest player / host), so a monster attacking or
   critting a player reads the *anchor's* traits/tables, not the real defender's/attacker's.
4. **UI-construction code with no subject param** (e.g. hit-mode builders).

Everything below is ranked by how *broken-regardless* it is vs. merely *rescued-today-but-fragile*
(correct only because of the scope swap, breaks on any AI-path or future refactor).

## Master findings — worst first

| # | site | domain | computes | now | bucket | fix | scope-rescued today? | impact |
|---|---|---|---|---|---|---|---|---|
| 1 | `combat.cc:4984` `attackComputeCriticalHit` | DEF | crit-effect table (player vs monster) on `defender==gDude` | `==gDude` | C | `playerActorIs(defender)` | **No** (fails on AI crit vs extra) | Every crit on an extra scored against the **monster** table — wrong cripple/instakill odds + messages. |
| 2 | `combat.cc:6893,6897` `unarmedGetHitModeInRange` | OFF | which advanced unarmed move unlocks (→ its dmg/AP/crit/penetration) from gDude skill+level+stats | `gDude`, **no subject param** | C | thread `Object* subject` through `unarmedGetPunchHitMode`/`KickHitMode` | **No** (UI-construction code) | Extra's advanced-unarmed swing resolves off the host's sheet. The originally-reported unarmed bug. |
| 3 | `interpreter_extra.cc:1741` `opWieldItem` (`wield_obj_critter`) | DEF | applies worn-armor AC/DR/DT (`_adjust_ac`) on script-equip | `critter==gDude` | C | `playerActorIs(critter)` — mirror fixed sibling `opMoveObjectInventoryToObject` (:4666) | **No** (script opcode) | Script-equipped armor on an extra: **visibly worn, zero protection** until re-equipped via inventory. |
| 4 | `critter.cc:436-648` poison/rad (`critterAdjustPoison`/`Radiation`/`_critter_check_rads`) | VIT | poison+rad accumulation, resist, damage ticks | `!=gDude → return` (hard no-op) | C | remove gate; `_critter_check_rads` iterate all actors; thread queue owner | **No** | **Non-host players immune to poison + radiation entirely.** Vanilla-rooted ("only works on dude"), now a live co-op gap. |
| 5 | `skill.cc:675` `skillUse` | VIT | First Aid / Doctor **Healer-perk** heal bonus | `obj==gDude` | C | drop gate; `perkGetRank(obj, PERK_HEALER)` | **No** | Extra with Healer heals as if **no perk** (Doctor ~4×r+4…10×r+10 → 4…10). Every use. |
| 6 | `item.cc:1639` `weaponGetRange` | OFF | throw range + `PERK_HEAVE_HO` | `critter==gDude` | C | `playerActorIs(critter)` — sibling `weaponGetActionPointCost` (:1717) already fixed | partial | Extra's Heave-Ho throw capped at base range; `attackCompute` (`:4680`) hard-rejects over-range → valid throw becomes **impossible**, not just weaker. |
| 7 | `item.cc:3142` addiction (`dudeSetAddiction`/`IsAddicted`) | VIT | per-drug "am I addicted" | shared `gGameGlobalVars[drug->gvar]` | C→D | per-actor owner (sheet appendix), not a bare gvar | n/a | Host's addiction blocks extra's roll; antidotes misfire across players. **→ per-player-gvar thread.** |
| 8 | `combat.cc:4654` `attackComputeEnhancedKnockout` | DEF | `PERK_WEAPON_ENHANCED_KNOCKOUT` forces `DAM_KNOCKED_OUT` | `defender!=gDude` | C | `playerActorIs(attack->defender)` | partial | Host vs extra defender treated asymmetrically for knockout. |
| 9 | `combat.cc:5497` `attackComputeDamage` (Stonewall) | DEF | `PERK_STONEWALL` knockback negate/halve | `critter==gDude` | C | `playerActorIs(critter)` | rescued/fragile | Non-host defender **never** gets Stonewall knockback resistance. |
| 10 | `trait.cc:374` `traitGetStatModifier` KAMIKAZE | DEF | zero subject's Armor Class | reads `gDude`, ignores `subject` | C | `critterGetBaseStat(subject, …)` | **No** | Extra Kamikaze pick: AC offset by the *host's* base AC, ends up wrong. Reachable via `attackDetermineToHit` AC read. |
| 11 | `trait.cc:409,414` FAST_METABOLISM | DEF | zero rad + poison resistance | reads `gDude`, ignores `subject` | C | use `subject` | **No** | Same class as #10 for rad/poison resist. |
| 12 | `combat.cc:5482-5494` `attackComputeDamage` | OFF | `PERK_LIVING_ANATOMY` (+5) / `PERK_PYROMANIAC` (+5 fire) | `attacker==gDude` | C | `playerActorIs(attack->attacker)` | rescued/fragile | Inconsistent with neighbors (:5404/:5410 already use `playerActorIs`). Both perks no-op for extras off the player-turn path. |
| 13 | `combat.cc:4713` `attackCompute` JINXED | OFF | `TRAIT_JINXED` crit-failure conversion | `traitIsSelected(TRAIT_JINXED)`, no subject → gDude | **group** | ✅ FIXED: `anyPlayerActorJinxed()` — JINXED is an AURA, so it fires if **any** player is Jinxed (whole-fight, like an SP party). Byte-identical SP. | n/a | Was: only the HOST's Jinxed ever counted. Now any player's Jinxed radiates to the whole party fight, per the group-effects ruling below. |

**►► GROUP-EFFECTS RULING (owner, 2026-07-24):** co-op players are an SP-style party, so **positive but especially NEGATIVE / aura effects apply to the whole group whenever possible** — the same reach they'd have from a party in single-player. JINXED is the first instance (any player Jinxed → the whole fight fumbles). This is a THIRD generalization axis alongside the A/B/C/D buckets: an effect that was an *aura* in vanilla generalizes to **group-wide keyed on "any player"**, not to the acting player (C) or per-actor (D). When an audited `gDude` site turns out to be an aura, prefer the group form. See memory `coop-group-effects-like-party`.
| 14 | `combat_ai.cc:2596` `_cai_attackWouldIntersect` | OFF | friendly-fire pre-check hit-mode/weapon | `attacker==gDude` else hardcode PRIMARY | C | `playerActorIs(attacker)` / pass hitMode | **No** (AI code) | Wrong hit-mode when judging a non-host shot's safety near a friendly. |
| 15 | `combat.cc:5232,5311` `attackDetermineToHit` | OFF | ranged-perception `-2` quirk + darkness to-hit penalty | `attacker==gDude` | C | `playerActorIs(attacker)` | rescued/fragile | **Core to-hit fn.** Correct today only via scope; systemic silent accuracy divergence if attacker≠bound-gDude. |
| 16 | `skill.cc:786,947,1121` `skillUse` | VIT | game-clock cost of First Aid/Doctor/Repair | `obj==gDude` | C | thread `obj` | rescued/fragile | Non-host skill use consumes no game time. |
| 17 | `combat.cc:5050` `attackComputeCriticalFailure` | OFF | first-6-hours newbie crit-failure grace | `attacker==gDude` | C | `playerActorIs(attack->attacker)` | rescued/fragile | Grace protects only the bound actor's identity, not every PC early-game. |
| 18 | `party_member.cc:837` `_partyMemberRestingHeal` | VIT | rest-heal amount/duration | bare `gDude` | **owner ruling** | **rest heals ALL player actors** (owner 2026-07-24: "everyone's HP moves") — iterate registered actors, apply each on their own healing-rate/HP; drop the single-`gDude` assumption | n/a | Deliberate co-op divergence: resting advances the shared clock and heals every player, not just the resting one's body. |

### Non-bug / keep (verified)
- `combat.cc:5342,5417` combat-difficulty modifier keys on **team**, not identity — correct while
  all players share team 0 (**B**). Revisit only for PvP/split teams.
- `combat.cc:4731,4778` `PERK_SILENT_DEATH` `attacker==gDude` — **deliberate** host-only until
  `DUDE_STATE_SNEAKING` is per-actor (generalizing without sneak = free x4 backstab off host's
  sneak). Blocked on sneak, not a fresh bug.

## Already-correct (do NOT re-audit)
The bulk of combat math already threads the subject. Representative, verified:
- **Stat/derived:** `critterGetStat` (AC/DT/DR/resist perk switch), `critterGetBaseStatWithTraitModifier`,
  `critterUpdateDerivedStats` (maxHP/healing-rate/sequence/crit-chance-from-LUCK/poison+rad-resist) —
  all `critter` + `playerActorIs`.
- **Offense perks/traits already `playerActorIs(attacker)`:** Slayer, Sniper, Sharpshooter,
  One-Hander, Weapon Handling, Finesse, Bonus Ranged Damage, Bloody Mess; `weaponGetDamage`
  (Bonus HtH Damage), `weaponGetActionPointCost` (Fast Shot / Bonus HtH Attacks / RoF),
  `critterCanAim` (Fast Shot). `weaponGetDamage`/`weaponGetSkillValue`/base to-hit
  (`combat.cc:5190`) fully parameterized on the attacker.
- **Defense:** defender AC read (`:5291`), defender DT/DR read (`:5390`), armor accessors
  (`item.cc:2119`, take the armor Object), `_adjust_ac` impl (`inventory.cc:86`),
  `opMoveObjectInventoryToObject` (already fixed), combat-roster turn order.
- **Vitals:** `critterAdjustHitPoints`, `_perform_drug_effect`/`_insert_drug_effect`
  (stimpak/drug HP threaded on `critter`, feedback via `playerActorIs`), XP/level
  (`pcAddExperienceWithOptions` earner pattern), `skillRoll`/crit-chance in `skillUse`.
- ►► **Stimpak heal is NOT luck-dependent** and already computed on the target critter (myth busted).
- ►► **Arroyo unarmed *training*** (`critter_mod_skill`, opcode `0x813C` `opCritterModifySkill:4339`)
  already honors the target (`playerActorIs(critter)` + `skillAddForce(critter,…)`); gate is the
  player's own skill value → each player trains independently. **Not a bug.** (Only the *attack-time*
  advanced-move selection, #2, is wrong.)
- ►► **Queued events owned by extras** (drug/poison/rad/withdrawal timers) across save/load —
  **already fixed** (`queue.cc` `queueEventBelongsToExtra` + co-op appendix). The old
  `COOP_SEAM_DESIGNS.md §1` / `coop-open-issues` note is **stale**.

## Adjacent: the gvar policy — smaller than it looks

The fear is "690 gvars each need a per-player decision." They don't. The **default — shared,
server-authoritative, not streamed — is already correct for the overwhelming majority** (quest
flags, world switches). The server does NOT need "a list of every gvar"; it needs a small
**opt-in exception table**, the same shape as `SCRIPT_OPCODE_MAP.md`'s subject-axis (only ~6 of
181 opcodes needed anything). Three buckets, and only the last two ever get a table row:

1. **Shared, server-only (DEFAULT — no work).** Quest/world state. Server is ground truth; clients
   never read it live. This is ~all of the 690. Do nothing.
2. **Shared value, client-visible EFFECT (a BROADCAST list).** The value stays one-per-game, but a
   *change* must reach viewers' Pipboy/sheet. Karma, town reputation, rep titles. This is a
   **streaming** job (emit a delta on change), NOT namespacing — the value isn't per-player, only
   its *display* was host-only. This is the "list of gvars whose effect is broadcast" you described.
3. **Per-actor status wrongly in a shared gvar (a NAMESPACE list).** The value should differ per
   player. Confirmed: **#7 addiction**. The one hard part — *"which player?"* — is already answered
   by `scriptContextDude(program)`; storage moves to the per-player sheet appendix.

**Build the table by symptom, not up front.** Each per-player bug we hit adds exactly one row to
list (2) or (3); everything unlisted stays bucket (1). No 690-way classification pass.

Calibration — **not every shared gvar is a bug:** thug aggro `global_var(447)&16384` (`DCThug`) is
*correct as shared* (one player provokes the gang → all players are in danger). It stays bucket (1).

Mechanical prerequisite for (3) only: the ~7 files that bypass `gameGetGlobalVar` with direct
`gGameGlobalVars[]` writes must route through the wrapper so a namespaced read has a choke point
(memory `gvar-namespacing-landscape`). Bucket (2) needs no such refactor — it hooks the setter's
broadcast, not the storage.

## op_critter_damage — the STATE is already decoupled (stale-doc correction, 2026-07-24)
`op_critter_damage` (`0x80EF`, `opCritterDamage:2472`) → `actionDamage`. `SCRIPT_OPCODE_MAP.md`
marks it `NOT-STARTED`, and the scripts recon relayed "scripted damage/death silently dropped
headless" — **both are STALE.** Reading `actions.cc:2506-2591`: the `serverLoopActive()` branch
applies the damage server-authoritatively (`_report_dmg` → `_apply_damage`, then `critterKill` for
a dead defender, with explicit ordering comments so `destroy_p_proc` fires). So **scripted /
environmental damage DOES land on players on the dedicated server** — Toxic Caves goo, traps, script
kills all apply. (Second stale "already fixed" doc case this session, after the queued-events bug.)
- The rubber-boots *scoping* is also correct (spatial `spatial_p_proc` keyed to `source_obj`,
  carried-boots check, damages `source_obj`).
- **Remaining gap is PRESENTATION only:** viewers see the hit-react / blood / death animation for
  scripted damage only when `F2_SERVER_PRES_RECORD` records the anim leaves (the `recording` branch);
  without it the state applies but no visual streams. That's the presentation-record track
  (`pres-record-build-track`), not a broken-state bug.
- ►► LIVE-VERIFY: confirm a player actually loses HP stepping in Toxic Caves goo on the server.
  Fix `SCRIPT_OPCODE_MAP.md`'s 0x80EF row if its `NOT-STARTED` refers to state rather than pres-record.

## Suggested fix batching
- **Batch 1 — trivial C-misclass one-liners** (subject already in scope; `==gDude` →
  `playerActorIs(subject)` / pass the subject): #1, #5, #6, #8, #9, #10, #11, #12, #13, #14, #15,
  #16, #17. High value, low risk, but **no headless N>1 oracle** → adversarial review + owner
  live-verify mandatory (`anim-decouple-verification`). Batch, don't per-commit-gate.
- **Batch 2** — ✅ #3 (opWieldItem) SHIPPED. #2 (unarmed hit-mode) = **NON-BUG dropped**: its only
  callers are `interface.cc`/`inventory_ui.cc` (f2_client, client-only), where `gDude` is the local
  player's own actor → hit-mode selection is already per-client-correct; the server uses the intent's
  `hitMode`. (Offense agent assumed server-side execution.)
- **Batch 3** — ✅ #4 (poison/rad, `playerActorIs` + `obj`/per-slot proto + `_critter_check_rads`
  iterates actors) and #18 (rest-heal iterates player actors, owner ruling) SHIPPED. **#7 addiction
  DEFERRED**: it's the bucket-3 per-player-gvar case → blocked on the gvar wrapper-funnel prerequisite
  (route the ~direct `gGameGlobalVars[]` sites through `gameGet/SetGlobalVar`, then per-player storage).
  Not a one-liner; see `gvar-namespacing-landscape`.
- **op_critter_damage** — state already decoupled (above); only the pres-record visual remains.
