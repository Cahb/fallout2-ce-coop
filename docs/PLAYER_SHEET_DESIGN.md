# Per-actor character sheets — design ruling

**Status:** ruling of record. Supersedes MP_PROPOSAL.md Ch 15's "shared pool, host sheet,
extras never level", which stays valid only as the description of what shipped BEFORE this.

**Owner ruling (2026-07-20):** per-player sheets are REQUIRED, not post-v1. Verbatim intent:
running an extra player is "too boring" if it is a tail that never grows HP/AP/perks. This
lands BEFORE dedicated-server save/load so the save format is born N-actor-correct instead
of needing a migration.

**What this is NOT.** HP, AP, position, rotation, inventory, equipment and combat state are
already per-actor — they live on the `Object`. That is why M3 combat worked per-actor the
moment the turn machine was pointed at the registry. It is specifically **the SHEET** that
is shared. Do not let this grow into re-deriving object-side state.

--------------------------------------------------------------------------------
## 1. What is shared today (the whole problem, in three storage classes)

| Class | Storage | Resolver | Why extras collide |
|---|---|---|---|
| skills, base/bonus SPECIAL | the dude PROTO, by pid | `protoGetProto(obj->pid)` [proto.cc:2125] | every player actor has pid `0x1000000` |
| perks | `gPartyMemberPerkRanks`, pid/gDude-keyed | `perkGetRankData` [perk.cc:285] | extras match NEITHER branch |
| XP, level, karma, unspent SP, traits | bare globals `gPcStatValues` [stat.cc:100], `gSelectedTraits` [trait.cc:35] | none — no subject at all | one global, N actors |

⚠ **The perk case is a LIVE BUG, not just a gap.** An extra is not `gDude`, and its pid is
the dude pid, which the scan loop skips (it starts at index 1). It falls through the
`debugPrint` at perk.cc:297 and **silently returns the HOST's perk row**. Extras read AND
WRITE P1's perks today. It looks harmless only because the rows are currently identical —
the first perk granted to one actor corrupts the host's sheet.

**Measured blast radius.** `critterGetStat` 231 call sites, `perkGetRank` 80,
`protoGetProto` 161 — none of which change, because they already take the subject as a
parameter. Only the RESOLVER BODIES change. `skillGetBaseValue` 2, `skillAdd` 2,
`perkAddForce` 2, `pcAddExperience` 7, `gDudeProto` 10.

--------------------------------------------------------------------------------
## 2. RULING — representation is a hybrid, matched per storage class

The right representation for each class is the one that lets its existing resolver body stay
recognizable.

**Skills + base/bonus stats → a per-actor `CritterProto` row, reached by a reserved pid
range.** Extras get pid `kPlayerActorSheetPidBase + slot` [server_players.h]. This is not a
new concept: `critterSave` [critter.cc:219] already serializes the PC sheet as one whole
`CritterProtoData` blob, so a per-actor row generalizes the unit the engine ALREADY uses.
`PID_TYPE(value) = value >> 24` [obj_types.h:33], so the range keeps type byte 1 (critter)
and is intercepted before the generic `_protoLists` scan, the way `0x1000000` is today.

⚠ **THE RANGE MUST NOT BEGIN AT THE DUDE PID.** `0x1000000` is special only because vanilla
reserved critters.lst **index 0** for the player. `0x1000001..0x1000007` are ORDINARY
CRITTERS — `_proto_load_pid` [proto.cc:1958] loads line `pid & 0xFFFFFF` of the list on
demand. Reserving them hands seven real critter protos a blank sheet, silently, because the
intercept fires before the loader ever runs. This is not theoretical: the first cut of the
range did exactly that, and it moved `arvillag_restfight` (a critter came back all zeros →
different combat RNG → different outcome). The base is therefore an index far past any .lst
length. **Any future widening of `kMaxPlayerActors` must keep it there.**

**Perks → their own slot-keyed array.** NOT annexed into `gPartyMemberPerkRanks`: that table
is malloc'd to `gPartyMemberDescriptionsLength` [perk.cc:198] for REAL COMPANIONS (Sulik,
Cassidy, …), and treating a player actor as a party-roster row directly contradicts the
locked principle that party membership is not actor identity. A future patch "helpfully"
merging the two tables is a named risk — see §7.

**XP / level / karma / unspent SP / traits → slot-keyed side records.** These have no proto
or object home at all today; they are bare globals with no subject.

**REJECTED — a sheet struct on `Object`.** `ObjectData` is ONE union shared by every object
in the game [obj_types.h:252]. A sheet is a few hundred bytes (`baseStats[35] +
bonusStats[35] + skills[18]` alone [proto_types.h:337]), so putting it there grows
`sizeof(Object)` for every NPC, item and scenery instance in every map to serve ≤8 actors.
It also buys nothing architecturally — it satisfies "resolvers change, call sites don't"
no better than the ruling above.

### The degeneracy argument (why N==1 is byte-identical)

Slot 0's proto row must **literally be `gDudeProto`** — same static struct, same address,
same pid — never a copy. `protoGetProto`'s existing `if (pid == 0x1000000)` branch
[proto.cc:2133] stays VERBATIM; the reserved range is checked around it, not instead of it.
`perkGetRankData`'s existing `if (critter == gDude)` branch stays verbatim too; slot-keyed
storage is consulted only for slots > 0, which do not exist when the registry is empty.

With an empty registry (client, headless golden probe, single-player) every new branch is
unreachable. This is the same argument that let the actor registry land without moving a
single golden byte.

⚠ **THE ARGUMENT IS NOT A SUBSTITUTE FOR THE GATE.** It already failed once, exactly as
above: "no object carries such a pid" was true of the objects and false of the pid SPACE, so
a branch reasoned to be dead was reached on the first golden case that touched one of the
seven stolen critters. Unreachability claims here rest on a range being free, which is a
property of the DATA, not of the code — run `check.sh`.

--------------------------------------------------------------------------------
## 3. RULING — the resolver seam

There is no single mint point (unlike `opGetDude` for `dude_obj`), because the three storage
classes have three different existing choke points. Each is singular WITHIN its class:

| Function | Site | Rule |
|---|---|---|
| `protoGetProto` | proto.cc:2125 | pid in `[0x1000000, 0x1000000+kMaxPlayerActors)` → that slot's row; slot 0 stays `&gDudeProto` |
| `perkGetRankData` | perk.cc:285 | check `playerActorSlotOf(critter)` BEFORE the pid scan; slot > 0 → slot-keyed array; slot 0 falls to the unchanged `gDude` branch |
| `pcGetStat` / `pcSetStat` / `pcAddExperience` | stat.cc:608,628,784 | resolve by the subject now passed in (§4); no subject → today's globals verbatim |
| `critterGetBaseStatWithTraitModifier` | stat.cc:394 | `critter == gDude` → `playerActorIs(critter)`, and forward the subject into the trait modifiers |
| `traitGetStatModifier` / `traitGetSkillModifier` | trait.h:22 | read the subject's trait row instead of the bare global |

Fixing `perkGetRankData` **closes the live bug in §1 for free**: once extras have distinct
pids, the slot check intercepts them before the silent host-row fallthrough is ever reached.

--------------------------------------------------------------------------------
## 4. RULING — XP subject comes from the CALL SITE, not from geometry

⚠ `scriptContextDude()` is the resolver for SCRIPT-OPCODE sites only. It is NOT a general
answer to "who did this". Three of the four XP sites already have the real actor in hand,
and using nearest-player geometry there would mis-attribute kills.

| Site | Subject | Note |
|---|---|---|
| combat kill XP [combat.cc:5652] | `whoHitMe` | already a live pointer at the accrual site |
| skill-use XP [skill.cc:518] | `obj` | already a parameter — the function just ignores it |
| steal XP [item.cc:3727] | `looter` | already a parameter — likewise ignored |
| `opGiveExpPoints` [interpreter_extra.cc:468] | `scriptContextDude(program)` | bare opcode, no object in scope — the ONE legitimate site |

`pcAddExperience` / `pcGetStat` / `pcSetStat` gain an explicit `Object* subject`
(null → today's `gDude`-implicit behavior, so unconverted callers keep working).

`_combat_exps` is a single scalar today [combat.cc:1918] accrued at :5662 and paid out at
:2833. It becomes per-slot, accrued by `playerActorSlotOf(whoHitMe)`.

**Policy call, decided:** when the killer is an ally/companion that is NOT a registered
player actor, credit **slot 0**. That is exactly today's behavior for that case, so it is a
disclosed default rather than new guesswork. Never index with -1 — assert, do not clamp.

--------------------------------------------------------------------------------
## 5. RULING — save format

**The handlers are already core.** `critterSave`, `perksSave`, `statsSave`, `traitsSave`,
`skillsSave`, `killsSave`, `queueSave`, `partyMembersSave` live in `perk.cc` / `skill.cc` /
`stat.cc` / `trait.cc` / `proto.cc`, all in the `f2_core` block of CMakeLists.txt. Only the
27-entry DISPATCH TABLE in `loadsave.cc` [loadsave.cc:211 save / :242 load] is f2_client and
unreachable from `f2_server`. The dedicated-server lift is therefore moving the TABLE DRIVER,
not the handlers — mechanical, and sequenced after this ruling so the table is born
N-actor-shaped.

**Versioning.** Each touched handler grows from one row to `playerActorCount()` rows behind
an explicit count prefix. Reuse the join blob's existing idiom (`u16 actorCount`, where 1 is
every pre-co-op blob, MP_PROPOSAL Ch 5) rather than inventing a second convention.

**The version field is REAL — experiment done.** `loadsave.cc:1803-1814` writes
`VERSION_MAJOR/MINOR/RELEASE` in the header ahead of the handler stream, and the loader
hard-rejects anything that is not exactly `1.2R` (`loadsave.cc:1930` →
`SLOT_STATE_UNSUPPORTED_VERSION`). So there IS a real field to gate on and no byte-shape
inference is needed. **Ruling:** write N-actor saves with a bumped RELEASE byte so vanilla
and pre-co-op builds reject them cleanly as "old version" instead of misparsing; keep `1.2R`
for the N==1 case, which writes no block at all and therefore stays byte-identical and
vanilla-loadable.

**The row format now EXISTS and is proven on the wire** — `player_sheet.{h,cc}`,
`playerSheetBlockWrite/Read`. One serializer, both consumers, because the blob IS the save
pipeline (`mapSaveToStream`): the "two PRs" this section predicted collapse into one format
with two call sites. `firstSlot` is the only difference — 0 for the blob (nothing else
carries the host's sheet), 1 for the disk save (slot 0 is already in the legacy handlers).
An empty range writes ZERO bytes, which is what keeps N==1 byte-identical.

⚠ **`fileWriteInt32` is BIG-ENDIAN** (Fallout file format). Anything grepping a blob for a
sentinel must search big-endian bytes — the magic reads `PSHT` on disk, not `THSP`.

--------------------------------------------------------------------------------
## 6. Staging (each stage independently gate-checkable)

1. **Storage + resolvers.** New per-slot storage, the §3 resolver rules. Gate: `check.sh`
   byte-identical — with an empty registry every new branch is dead code.
   **Does not move goldens.**
2. **Extras get distinct pids + a spawned proto row.** `serverSpawnExtraActors` assigns
   `pid = playerActorSheetPid(slot)` and seeds the row from `gDudeProto`; the viewer seeds
   the same rows from the join blob's actor appendix. First OBSERVABLE change: `obj_pid()`
   on an extra no longer equals the dude pid — audit script content testing
   `obj_pid(x) == 16777216`. Closes the Ch 4.3 shared-proto wart (`set_critter_stat` on one
   actor no longer bleeds to all). Still N==1 identical.

   ⚠ **SEED ALL FOUR OR THE ACTOR IS A CHIMERA.** `protoPlayerActorSheetsSeed`
   (skills + SPECIAL), `perkPlayerActorSeedRanks`, `pcPlayerActorSeedStats` (XP, level,
   karma, unspent SP) and `traitsPlayerActorSeed` are ONE operation — an extra joins AS the host's character and
   diverges from there. Seeding skills but not level is not cosmetic: the level a player
   stands at drives the next level-up's HP award. Before the perk slot check, extras
   read the host's perk row by accident; after it they read their own. If nothing copies the
   host's ranks in, a host who already has perks silently loses Toughness / Bonus HtH Damage
   / Awareness off every extra's combat math.
3. **XP + trait/stat subject-threading.** ✅ DONE. §4, including per-slot `_combat_exps`. Riskiest —
   combat-machine touch with no headless N>1 oracle. Mandatory adversarial review. Carries
   `critterGetBaseStatWithTraitModifier` and the trait modifiers, which the §3 table lists
   under stage 1: they belong HERE, because threading a subject through
   `traitGetStatModifier`/`traitGetSkillModifier` breaks stage 1's whole invariant that only
   resolver BODIES change.

   Also here: `skillGetValue`'s PC block is per-actor for perks but still reads the TAGGED
   SKILLS and TRAITS globals [skill.cc:262]. Correct for v1 (one authored character), wrong
   the moment per-actor character creation lands — those two are the next to grow a subject.
   Same for `protoGetName`, where every actor deliberately answers `gDudeName`.
4. **Save table N-actor-ization.** §5. Depends only on stage 1, not on 2/3. The row format
   and the block writer/reader are DONE (§5); what is left here is calling
   `playerSheetBlockWrite(stream, 1)` / `...Read` from the disk handlers behind the bumped
   release byte. Blocked on nothing but the table driver below, since `f2_server` cannot
   reach the dispatch table at all.
5. **Join-blob PC-data section.** ✅ DONE. The blob's appendix carries sheets from slot 0,
   applied by the viewer after the actor registry is populated. Closes the "viewer seeds
   sheets from its own gDudeProto" staleness — a joiner now sees the host's ACTUAL
   character, not the character as it was at boot.
6. *(deferred)* loadsave dispatch table into core (`f2_server` save/load).

--------------------------------------------------------------------------------
## 7. Risks, ranked

1. **HIGH — an extra's write reaching the host's sheet.** Because slot 0 aliases the literal
   `gDudeProto`, any pid-dispatch bug that lets an extra fall through to the `0x1000000`
   branch mutates **the host's live character**. Silent and asymmetric: extra-side code
   corrupts the host. Mitigation: assert `slot == 0 ⟺ pid == 0x1000000` at the one dispatch
   site, and derive the slot ONLY from the pid — pid arithmetic is stateless and cannot go
   stale, unlike a cached `Proto*` (the id-plus-stale-pointer antipattern this tree has been
   bitten by before). The dispatch site checks that the row it hands back carries the pid it
   was asked for, and aborts otherwise — an unseeded row and an off-by-one slot both land
   there. ⚠ It is a `fprintf` + `abort()`, NOT `assert()` and NOT `debugPrint`: check.sh
   builds RelWithDebInfo (NDEBUG), and `f2_server` registers no debug print proc, so both of
   those compile to silence in every build this project actually runs.
2. **HIGH — `_combat_exps` restructuring corrupts XP for everyone.** Accrual and payout must
   change together; a slot-index bug silently zeroes or misattributes XP with no headless
   oracle. Assert, never clamp.
3. **MED — save version ambiguity.** See the experiment owed in §5.
4. **MED — someone merges the perk table into `gPartyMemberPerkRanks` later.** Guarded by a
   comment at the new array's declaration explaining why they are separate.
5. **LOW — the trait-modifier gate [stat.cc:398] gets missed.** Not in the headline
   skills/stats/perks/XP list, so easy to overlook; extras would get correct base stats but
   wrong trait-adjusted ones.

--------------------------------------------------------------------------------
## 8. What is STILL shared, and what unblocks it

**Tagged skills** (`gTaggedSkills` [skill.cc]) — the last shared piece of the sheet. One
PC-global, no subject. Correct while co-op v1 is one authored character; the next thing to
grow a subject when per-actor character creation lands.

**The player's NAME** (`gDudeName` [critter.cc]) — also a bare global, and NOT part of the
sheet: `protoGetName` hands every actor the host's name deliberately. Stopgap for a live
session: an `F2_PLAYER_NAME` per client.

### Perks: the three mechanisms, and the LEFTOVER list

A perk reaches the sim one of three ways, and only one of them ever needed fixing:

1. **Data-driven (most perks).** `perkAddEffect(critter, perk)` writes bonus stats onto
   that critter. Bonus stats live on the per-actor row, so these are per-actor **by
   construction** — there is no code to change, which is why the breakage list is short.
2. **Hardcoded engine checks** — `if (perkGetRank(x, PERK_Y))` in combat/item code, behind
   an `x == gDude` gate. This is the class that was broken. The combat cluster is DONE
   (Bonus Move, Sharpshooter, Weapon Handling, Bonus Ranged Damage, Sniper, Slayer, Bonus
   HtH Damage, Bonus HtH Attacks, Bonus Rate of Fire, HtH Evade, Pyromaniac, Quick Recovery).
3. **Script-queried** — `has_trait`'s `PERK` case does `perkGetRank(object, param)` and has
   always honoured its subject. It was the sibling `TRAIT` case that dropped it (fixed).

**LEFTOVER — perks NOT yet per-actor, with the reason each is still open:**

| Perk(s) | Site | Why still open |
|---|---|---|
| Awareness, Comprehension, Demolition Expert | proto_instance.cc:304/790/903, queue.cc:481 | ⚠ **UNAUDITED — the real candidates.** Nobody has opened these |
| ~~Silent Running~~ | animation.cc:653/736 | **DONE 2026-07-25** — sneak is per-actor (`dudeHasState`/`dudeIsSneaking` take a subject), so both sites read the moving owner. Note animation.cc is f2_client-only; the server's mover is server_anim.cc |
| ~~Silent Death~~ | combat.cc, both sites | **DONE 2026-07-25** — per-attacker, unblocked by the same change. The old note was right to hold it: generalising the perk alone would have handed an extra a x4 backstab whenever the HOST sneaked |
| Educated | stat.cc:689/705 | inside `pcLevelUpApply`, blocked on per-player level-up |
| Master Trader | item.cc:3782 | barter is stubbed for extras anyway |
| Night Vision | light.cc:51 | bucket A — global light / camera, not a sheet read |
| Empathy, Smooth Talker | game_dialog.cc, interpreter_extra.cc:3884 | host-only by the dialog ruling |
| Explorer, Ranger, Scout, Pathfinder, Fortune Finder, Cautious Nature | worldmap.cc | host-only by the worldmap ruling |

⚠ **JINXED is deliberately untouched** (combat.cc:4692). It is NOT gated on the attacker at
all — it applies to every failed roll in the game, including NPC vs NPC, reading the host's
trait/perk. That is vanilla behaviour for a world-affecting perk; under N players "whose
Jinxed?" is a design question, not a bug. Decide before touching it.

### The level-up announcement (owner ruling, 2026-07-20)

Level-up and XP presentation is currently gated to the host's screen. **That is a missing
capability, not a design decision**, and the ruling is to fix it the simple way: make the
level-up a BROADCAST world event — *"<PLAYER> went up a level"* — seen by everyone, the way
a kill is. That removes the need for per-client message addressing for this class of
message entirely; two message ids (vanilla 600 "You have gone up a level" for your own
actor, a new one for others).

⚠ **Blocked ONLY on per-actor names** — today every actor reads `gDudeName`, so it would
print the same name for all of them.

**Where it hooks:** `pcAddExperienceWithOptions`'s level loop, which is the SINGLE FUNNEL
every XP source converges on (combat kill / skill-use / steal / `give_exp_points`), and
which already does the "prev XP + new XP crosses the threshold" test per actor. Written once
there, every source gets it. Note the opcode layer is NOT that funnel — `give_exp_points` is
one of four inbound edges, and the other three never touch the interpreter.

**Keep self-only:** the "you earn %d exp. points" line. Broadcasting every XP tick from N
players is console spam.

### ►► The perk award is SCREEN-TRIGGERED, and the flag is shared (found 2026-07-20)

Levelling does not award anything by itself. The award lives in
`characterEditorUpdateLevel()` (character_editor.cc, `static`), whose ONLY caller is
`characterEditorShow` — so HP, skill points and the perk pick are applied lazily, the next
time that player OPENS the character screen:

```
level = pcGetStat(PC_STAT_LEVEL);
if (level != gCharacterEditorLastLevel && level <= PC_LEVEL_MAX)
    if (pcLevelUpApply(gCharacterEditorLastLevel, level))
        gCharacterEditorHasFreePerk = 1;
if (gCharacterEditorHasFreePerk) perkDialogShow();
gCharacterEditorLastLevel = level;
```

`gCharacterEditorLastLevel` is the level the award was last reconciled against;
`gCharacterEditorHasFreePerk` is an owed pick that survives closing the dialog. Both now
live in `character_editor_state.cc` (f2_core) because they round-trip through the savegame.

**Two consequences for per-player level-up, and they are why #3 is not just a UI job:**

1. **Lazy, not event-driven.** An actor whose player never opens the sheet never gets the
   HP or the perk at all. A broadcast announcement (above) would fire while the actual
   award had not happened.
2. **One shared pair for N players — a C→D misclassification (§ the `== gDude` taxonomy).**
   `pcGetStat(PC_STAT_LEVEL)` is read for whoever is acting, but `lastLevel`/`hasFreePerk`
   are single PC-globals. P1 opening their sheet stamps `lastLevel` to the current level and
   consumes the flag, so P2's owed perk silently disappears. Symptom reads as a content bug.

**So the award must move off the screen-open trigger and onto the sheet, per actor**, driven
from the same `pcAddExperienceWithOptions` level loop the announcement hooks. The perk
DIALOG stays client-side and per-player; only the owed-pick bookkeeping moves.

---

## 9. FINISHING THE SHEET — audited state + the granular edit protocol (2026-07-25)

►► **STATUS: BUILT 2026-07-25.** Owner-specified functionality, not a defect — nothing
regressed, this was never built. Owner: *"clients gotta send intents i wanna bump (if allowed /
if have points to spare cos of levelup and stuff) skills or perks xyz; it's not a gap, it's
missing functionality i just explicitly said should exist."*

What shipped, and where it lives:
- `src/sheet_intent.{h,cc}` — the ruling layer: entitlement, range checks, the undo baseline,
  and the `ServerActorScope` that makes the host-only skill leaves work for any actor. Its
  header holds the four reasons the layer exists; read that before changing a leaf.
- Wire verbs `sheetopen`/`sheetclose`/`skillup`/`skilldown`/`perkpick`/`tagpick`/`mutpick`
  (`server_control.cc`), exempt from the dead-actor and busy gates via one shared predicate.
- Admin verbs `sheet`/`sp`/`skillup`/`skilldown`/`perkpick` (`server_admin.cc`) — the same
  rulings, addressed by slot, so the path is testable without two machines.
- The client editor emits intents instead of mutating (`character_editor.cc`), and repaints
  when the row lands. **The unconditional rollback is gone** — see §9.5.
- `scripts/check_sheet.sh` (in `check.sh`) — a TWO-SEAT headless gate over the rulings. The
  only gate that runs N>1, so the only one that can catch a spend landing in the wrong row.

►► **THE ONE THING THAT WAS NOT IN THIS SECTION'S PLAN, and had to be built:** nobody was
ever AWARDED anything to spend. `pcLevelUpApply` was called only by `characterEditorUpdateLevel`,
i.e. lazily, the next time somebody opened the character screen — vanilla awards HP in the XP
funnel but skill points on the screen. A dedicated server has no screen, and every player's
screen is server-owned, so the intents would have been inert. The award moved into
`pcAddExperienceWithOptions`' level loop, per earner, and `pcLevelUpApply` now takes a subject
(it read gDude's INT / Educated / traits for whoever levelled). The owed perk moved off the
shared `gCharacterEditorHasFreePerk` PC-global onto a per-actor flag (`perkOwedPickGet/Set`,
perk.cc) that rides the sheet row, which also closes §8's "P1 opening their sheet consumes P2's
perk" bug. Two goldens were re-blessed for the timing change (see run_golden.sh's notes on
`arvillag_actions` / `arvillag_perks`): a level-13 character now HOLDS unspent points instead of
being handed them by a screen.

⚠ **Live-unverified**: the character SCREEN half (the +/- buttons, the perk dialog, the
Tag!/Mutate! follow-ups, the repaint-on-delta) has no headless oracle. The rulings underneath it
are gated.

Owner: *"do we carry OK / respect: perks learnt, (negative traits on char creations), skill
bumps ... the verbs protocol gotta be granular: i skilled a single point into GUNS or UNARMED;
or i learnt this <perk> ... this should round trip from srv back to client ... + absolutely
carry out in saves ... every single negative trait, every single perk and every single skill
there is; it's time we finish char sheet right; 100% no excuses"*.

Audited before planning. **Most of it already works** — the gap is narrower and more specific
than "the sheet is unfinished", so this section records what NOT to rebuild.

### 9.1 ALREADY DONE — per-actor storage, and it is all in the save
`playerSheetRowWrite` (player_sheet.cc) writes SIX rows per slot into the PAC2 appendix, and
`playerSheetRowRead` restores them. Everything the owner listed is already covered:

| row writer | carries |
|---|---|
| `protoPlayerActorRowWrite` | SPECIAL **and every skill's base value** (`proto->critter.data.skills[]` — the dude's skills live in its proto, and each actor has its own proto row; slot 0's row IS gDudeProto) |
| `perkPlayerActorRowWrite` | **every perk rank** (`perkPlayerActorRow(slot)` / `perkGetRankData(critter)`) |
| `pcPlayerActorRowWrite` | XP, LEVEL, **unspent skill points** |
| `traitsPlayerActorRowWrite` | **both trait slots, negatives included** (`traitRow(subject)`) |
| `skillsPlayerActorTaggedRowWrite` | the tagged skills |
| `critterPlayerActorNameRowWrite` | the name |

So: perks, traits (including negatives), skill values and unspent points are per-actor and
persisted **today**. Do not add storage or a save section for them.

### 9.2 ALREADY DONE — the result round-trip
`perkAdd` ends with `playerSheetMarkDirty(critter)`, and the sheet delta channel
(`EVENT_PLAYER_SHEET`, one slot + a one-slot PSHT block) streams the WHOLE row to that actor's
client. The owner's "the result of it essentially should round trip from srv back to client" is
therefore **already the mechanism** — an applied edit re-streams the authoritative row, and the
client's next sheet read shows it. Nothing new is needed on the return path.

### 9.3 THE ACTUAL GAP — three items, all small
1. ►► **No player can edit the sheet at all.** `characterEditorShowViewOnly()` is
   `characterEditorShow(0)` followed by `characterEditorRestorePlayer()` — it deliberately
   DISCARDS edits ("local edits are drift the next rebaseline reverts", main.cc). Every co-op
   player is a viewer, so **nobody can spend a skill point or take a perk in co-op right now**,
   host included. That is the whole reported problem.
2. ►► **`skillAdd` / `skillAddForce` are HOST-ONLY and read the wrong budget.** `skill.cc:400`
   opens with `if (obj != gDude) return -5;`, and inside, `pcGetStat(PC_STAT_UNSPENT_SKILL_POINTS)`
   and the matching `pcSetStat` are called **with no subject**, i.e. against gDude's row. The
   skill WRITE itself is already per-actor (it indexes `obj->pid`'s proto), so only the budget
   and the guard are wrong.
   ►► **Fix by running the spend inside a `ServerActorScope(actor)`** rather than by rewriting
   the leaves: the scope rebinds gDude to the acting actor, which makes `obj != gDude` pass AND
   makes both pc-stat calls resolve to that actor's row. Same trick the per-reader narration and
   the in-turn combat paths use. Rewriting the leaves to take a subject is the tidier long-term
   shape but is NOT needed to ship this, and it touches vanilla signatures.
3. **No granular verbs.** Needed, exactly as the owner framed them — one point, one perk, one
   intent:
   - `skillup <skillId>` — spend ONE point in one skill. Server: resolve the session's actor,
     `ServerActorScope`, `skillAdd(actor, skill)`, honour its return (-4 not enough points, -3
     at the 300%% cap, -5 invalid) and refuse with that reason.
   - `perkpick <perkId>` — `perkAdd(actor, perk)`, which already validates prerequisites via
     `perkCanAdd` and already marks the sheet dirty.
   Both are INTENTS: the client sends them and renders whatever row comes back, never its own
   optimistic edit. Validate the id against range + the real allow-lists (an untrusted index
   into `gPerkDescriptions` / `gSkillDescriptions` is an out-of-bounds read on the one shared
   sim — the same discipline as the `cattack` hitLocation clamp).

### 9.4 Deferred spending comes free
*"you can bump few skills and close exp / char view for later"* needs no work: unspent points
live in the per-actor pc row (§9.1) and are already saved, so a partial spend persists across
closing the screen, a rebaseline, and a save/load. It falls out of doing §9.3 correctly and
should be verified rather than built.

### 9.5 Client side — BUILT
`characterEditorShowViewOnly` now brackets the screen with `sheetopen`/`sheetclose` and its
spends emit verbs; `characterEditorSheetIsServerOwned()` (= `clientViewerActive()`) is the one
predicate every converted leaf branches on. Three notes worth keeping:
- ►► **The rollback had to GO, not just be bypassed.** It restored the snapshot taken when the
  screen OPENED, so leaving it in place would have wiped the rows the server streamed in while
  the screen was up. "Done" and "Cancel" now both just close; every spend was already committed
  one at a time on the authority that owns it.
- **No optimistic display and no hold-to-repeat.** A press sends one intent; the number moves
  when the row comes back (`clientViewerConsumeSheetDirty` in the editor's main loop). Auto-repeat
  would fire a verb per frame into a per-beat line cap and the extra presses would vanish
  silently.
- **The follow-up dialogs stay local** (which skill, which trait is a UI question) but commit
  through `tagpick`/`mutpick` **by id, never by line number** — the perk dialog sorts
  alphabetically, so a line index would make the protocol depend on the client's render order
  and on the language of its message file. Cancel sends `-1`.

~~Still to do: the level-up availability flag...~~ **The FLAG landed 2026-07-25.**
`DUDE_STATE_LEVEL_UP_AVAILABLE` is per-actor (the whole `dude*State` trio takes a subject and marks
the row dirty, so it streams), and — importantly — it is **re-derived**, not event-set:
`pcLevelUpBadgeRefresh` computes it from unspent points + an owed perk pick, and the sheet intents
call it after every spend. Vanilla's only clear path is "the player closed the character screen",
which an authority without a screen never does, so an event-set badge would have lit forever
([[no-re-derivation-path-bug-class]]). The client re-derives its indicator bar from each arriving
sheet row (`client_net.cc onPlayerSheet` → `indicatorBarRefresh`).

Still host-only, and correctly so: the interface REPAINTS in `pcAddExperienceWithOptions` (they act
on this process's single interface bar) and the `levelup` chime, which has no addressed sfx seam
yet — see the coverage doc's "addressing mechanism for personal sfx/fades".
