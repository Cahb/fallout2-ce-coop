# Co-op feature coverage — the vanilla inventory, ruled one by one

**The question this answers:** *"here's Fallout 2 vanilla, here's our tapped engine; what do we
lack, what percentage of features/systems did we implement/tap for the co-op experience, what is
still blind"* — including the subtle end: karma, faction joins, reputation, and companions
reacting to what the player does.

**Method.** The inventory is built from what the ENGINE implements (opcode tables, request
handlers, verb tables, gvar declarations, skill/perk/kill-type tables), not from memory of the
game. Each entry is then ruled with the project's own tests, which are NOT re-litigated here:
- **Group-effects ruling** (memory `coop-group-effects-like-party`): "in SP, did this reach beyond
  the dude?" yes ⇒ group-wide, no ⇒ per-actor.
- **Karma & reputation stay PARTY-BASED** (owner ruling). For those the question is only whether
  the shared read is anchored on the right subject and whether the effect reaches every screen.
- **`subject == gDude` is not a test for "the host"** — `ServerActorScope` rebinds it; on a client
  it is that client's own actor. Buckets per `docs/COMBAT_GDUDE_AUDIT.md`: A local-player,
  B host-by-ruling, C the ACTING player, D per-actor property. The dangerous misclass is C→D.

**Verdict classes.**
| class | meaning |
|---|---|
| **NATIVE** | correct in co-op today — right for every player, or right-as-shared by ruling |
| **SHARED/HOST** | works, but anchored on slot 0 or one global ⇒ wrong or partial at N>1 |
| **UNSEEN** | wired, never exercised or verified with 2+ real players |
| **ABSENT** | not wired |

Adjacent axes, deliberately not duplicated here: `docs/COOP_COVERAGE_STUBS.md` (server
stub/abort surface) and `docs/COOP_COVERAGE_WIRE.md` (simulated-but-not-forwarded). Where a gap
below is a stub or a wire gap it says so in one line and points there.

---

## 1. Character — 14 entries (NATIVE 8 / SHARED 1 / UNSEEN 2 / ABSENT 3)

| feature | verdict | evidence |
|---|---|---|
| Character creation (SPECIAL/traits/tag at roll) | NATIVE | `src/player_create.cc`; `create` verb `server_control.cc:1771`; owner-confirmed done (`coop-v1-elephants`) |
| SPECIAL per actor | NATIVE | per-slot proto row, `playerSheetRowWrite`; `docs/PLAYER_SHEET_DESIGN.md` |
| Traits incl. negatives | NATIVE | both trait slots in the PAC2 appendix; `trait.cc:374/409` subject fix @643b88a (live-unverified) |
| Perks + ranks | NATIVE | per-actor perk ranks; `perkAdd` marks dirty → `EVENT_PLAYER_SHEET` |
| Skills + tagged skills | NATIVE | skills live in the per-actor proto; `gTaggedSkills` per-actor |
| Level-up: award + spend | NATIVE | `sheet_intent.{h,cc}`, verbs `skillup/skilldown/perkpick` @552942d; award moved into `pcAddExperienceWithOptions` |
| Tag! / Mutate! follow-ups | UNSEEN | `tagpick`/`mutpick` verbs exist; the SCREEN half is live-unverified (`per-player-character-sheet`) |
| Crippled limbs / blindness | NATIVE | `obj->data.critter.combat.results` is per-actor; Doctor heal tables `skill.cc:62-76` take the subject |
| Death gating | NATIVE | `critterIsDead` (DAM_DEAD or hp<=0), derived not tracked |
| Revive | NATIVE | `critterRevive()` in `critter.cc`; healing item on a downed player, `revive <slot>` admin verb. **owner-confirmed working 2026-07-25** |
| **Addictions** | **SHARED** | `item.cc:3142` stores "am I addicted" in a **shared gvar** → the host's addiction blocks an extra's roll, antidotes misfire across players. Audit #7, deferred behind the gvar wrapper funnel |
| **Gender / appearance per player** | **ABSENT** | gender is never sent to the server (12-int spec only); `_art_vault_guy_num` is a global. Design = `docs/COOP_SEAM_DESIGNS.md` §2 |
| **Sneak** | **ABSENT** | `dudeHasState(DUDE_STATE_SNEAKING)` reads `gDude`'s proto flags (`critter.cc:1368`); the viewer's key 1 / skilldex Sneak is deliberately unwired (`main.cc:902`) |
| **All-players-dead recovery** | **ABSENT** | revive needs a LIVING player ⇒ a full wipe is a soft-lock with no in-band escape. Product call, `coop-v1-elephants` |

## 2. Combat — 16 entries (NATIVE 11 / SHARED 2 / UNSEEN 1 / ABSENT 2)

| feature | verdict | evidence |
|---|---|---|
| Turn-based, per-actor turns | NATIVE | M3 @fe49133, per-slot barrier + intent routing |
| Player-initiated combat | NATIVE | `cstart` verb |
| Aimed shots / hit locations | NATIVE | `cattack <net> [hitMode] [hitLoc]`, `server_control.cc:3416-3436` (clamped at the trust boundary) |
| Weapon modes / active-hand switch | NATIVE | `hand <0\|1>` verb, 0 AP, @0c2c0c8 |
| Criticals / knockdown / knockout | NATIVE | crit-table + enhanced-knockout + Stonewall subject fixes @643b88a — check.sh-gated, **live-unverified** |
| Burst fire | UNSEEN | no co-op verification on record; extras' `extrasFlags` path also feeds the splash-narration bug. ►► NOT a bug, and worth writing down because it LOOKS like one: a thrown spear that landed on a nearby friendly in a tight melee was the engine rolling a **MISS** and vanilla resolving that miss onto another critter in the scrum — i.e. friendly fire from a missed shot, working as designed. Owner ruling 2026-07-25: expected, good that it survived the co-op path, **do not chase it**. Burst SMG is believed to behave the same way |
| Throwing / grenades | NATIVE | §12 throw fix shipped; `scripts/throw_smoke.sh`. Cosmetic tail: BUG6 retrieve desync |
| Explosives arm + drop | NATIVE | `useitem_armexplosive` verb; `explosiveIsExplosive` reject on the plain use-on path (`server_control.cc:2466`) |
| AoE / splash damage | NATIVE | damage applies; narration degraded → `rocket-splash-no-damage-narration` |
| Reload / unload | NATIVE | `reload`/`unload` @dd6f4e7 + positional reload sound & magic-hands gesture |
| Armour / AC | NATIVE | `_adjust_ac` takes the armor object; script-wield AC give-back per-player @5157335 |
| Combat narration per reader | NATIVE | @8aff1a1 — "you" means you, plus the attacker/weapon header |
| **NPC AI noticing multiple players** | **SHARED** | `scriptContextDude` resolves to the **nearest** living player, so a zone that should catch everyone catches one per proc tick; an lvar latched against P1 can be advanced by P2 (OPEN-Q#1, `coop-mp-track`) |
| **Party/companion AI** | **SHARED** | `combat_ai.cc:2376-2379`, `:3007-3010`, `:3168` follow/guard/nearest-teammate all anchor on `gDude`; during an AI turn NO scope is held ⇒ that is slot 0. Sulik hovers around the host, never around an extra |
| **Sneak attacks / Silent Death** | **ABSENT** | deliberately host-only until sneak is per-actor (`combat.cc:4849` comment, `:4731/:4778`) |
| **Two-floor / concurrent combat** | **ABSENT** | one file-static combat context; a 2nd fight is dropped, and every roster filters on `gElevation` = the CAMERA (MP_PROPOSAL Ch 14.3a). One fight freezes free-roam engine-wide |

## 3. Items & inventory — 14 entries (NATIVE 10 / UNSEEN 3 / ABSENT 1)

| feature | verdict | evidence |
|---|---|---|
| Equip / wield / hands | NATIVE | `invwield`/`invunwield`; open bug: wielded weapon intermittently lost on rejoin (`coop-open-issues`) |
| Drop, incl. the count dial | NATIVE | `invdrop`; count modal wired 2026-07-25, live-verify owed (`drop-count-divergence`) |
| Pick up | NATIVE | `get` verb; item-vanishes-at-gesture-end sync NOT built (`PRESENTATION_PACING_DESIGN.md` §11 feature B) |
| Containers — item AND scenery | NATIVE | `loot`/`take`/`put`/`takeall`; use `_obj_action_can_use`, not a scenery-type list (`two-kinds-of-containers`) |
| Corpse looting | NATIVE | **owner-confirmed working 2026-07-25.** BUG3 "cannot loot gibbed critters" + the take-all tail remain as BUGS, not as missing wiring |
| Stacking / caps / money | NATIVE | pid-summed quantities; barter commits move caps correctly (verified in the barter smoke) |
| Drugs & chems | NATIVE | `_perform_drug_effect` threaded on the critter; withdrawal timers owned by extras survive save/load (`queue.cc` `queueEventBelongsToExtra`) |
| Repair / Science on objects | UNSEEN | `skill` verb allow-lists both (`server_control.cc:427-428`); no co-op verification on record |
| First Aid / Doctor | NATIVE | Healer perk + game-time cost fixed for extras @643b88a |
| Ammo types / swap | NATIVE | `ammoQuantity`/`ammoTypePid` ride `OBJECT_DELTA_INVENTORY` |
| Script weapon upgrades (e.g. the New Reno gunsmith) | UNSEEN | pure dialog + `op_wield_obj_critter`; the AC give-back bug is fixed, the path is unexercised |
| Inventory screen during combat | NATIVE | @cc2fae3 — priced at the SCREEN (4 AP / 2 with Quick Pockets), turn-end blocking not loop blocking |
| Carry weight / encumbrance | NATIVE | per-actor `STAT_CARRY_WEIGHT`; `lootTakeAll` gates on it |
| **Barter item inspect** | **ABSENT** | no way to see what you are buying (`barter-track`) |

## 4. World interaction — 13 entries (NATIVE 8 / UNSEEN 2 / ABSENT 3)

| feature | verdict | evidence |
|---|---|---|
| Doors open/close | NATIVE | `use`/`usedoor` + `doorState` slide; `obj->frame` streams @cee0fa3 with a double-apply suppressor |
| Locks / Lockpick | NATIVE | `skill <net> SKILL_LOCKPICK` runs the real `actionUseSkill` |
| Lock / jam word visibility | UNSEEN | `obj->data.flags` / `door.openFlags` are server-authoritative and not streamed → **wire axis**, `COOP_COVERAGE_WIRE.md` |
| Traps (detect / disarm) | NATIVE | **owner-confirmed working for P2 on the Temple map, 2026-07-25** |
| Stairs / ladders | NATIVE | `use` → `_obj_use` → `mapSetTransition` |
| **Elevators** | **ABSENT** | `ServerScriptRequestHandler` overrides ONLY `dialogEnter` and `worldMap` (`script_request_handler_server.cc:26-80`), so `elevatorSelect` falls through to the base `return -1` (`script_request_handler.h:47`) and the whole placement/transition block is skipped — exactly as if the player cancelled the picker. `METARULE_ELEVATOR` (`interpreter_extra.cc:3288`) is the only entry point. **24 elevators are declared** (`elevator.h:7-30`): Sierra ×3, Vault City, Vault 15 ×2, Navarro ×4, the Shi Temple, the Wanamingo Mine, Toxic Caves, Military Base ×2, Vault 13, and the Brotherhood/Master/Glow set |
| Exit grids / map transitions | NATIVE | open to ALL as of 2026-07-23 (`playerActorMayTransit` = `playerActorIs`); live-verify owed |
| Scenery use (levers, computers) | NATIVE | `use` verb, any scenery, `_obj_use` routes |
| Real-time script map mutation | NATIVE | frame @cee0fa3, per-object + ambient light @8113084; the remainder is the **wire axis** |
| **Stealing / pickpocketing** | **ABSENT** | `stealing()` is the base no-op on the server; the viewer's skilldex STEAL is parked with a comment saying why (`main.cc:927`). Engine already refuses steal in combat (msg 902) |
| **Planting items on a live critter** | **ABSENT** | same steal path |
| Planting into containers / corpses | NATIVE | `put` verb, out-of-combat gated (`server_control.cc:3251`) |
| Highlight lootables | NATIVE | `src/loot_highlight.cc` @14266d2, client-only, never on the wire or in a save; live-verify owed |

## 5. Social — 16 entries (NATIVE 8 / SHARED 4 / UNSEEN 2 / ABSENT 2)

This is the family the owner's subtle question lands in. **The headline is good news:** karma and
every reputation are plain gvars that the engine only *displays*, and gvars now stream, so the
shared party-based model the owner wants is actually in place and visible.

| feature | verdict | evidence |
|---|---|---|
| Dialog (text, options, head, voice, lips) | NATIVE | initiator-driven, all viewers render, only the driver picks (`dialog-streaming-track`) |
| Barter | NATIVE | full loop; `EVENT_BARTER_BEGIN/STATE/END` @984f0e6 + viewer window @7021697 |
| NPC reaction value | NATIVE (as shared) | reaction is stored in the NPC's **own lvar 0** (`reaction.cc:8-14`, `:42`) — per-NPC, never per-player, so party-shared is structurally correct |
| Karma | NATIVE (as shared) | `GVAR_PLAYER_REPUTATION`; sfall karma-change message in `game.cc:132`; **now streamed** — `gvarDeltaScan` diffs the whole gvar array each beat (`object_delta.cc:321-361`) and the viewer writes it straight into `gGameGlobalVars` (`client_net.cc:2510-2526`) @634f4cf |
| Reputation titles — Childkiller, Berserker, Champion, Grave Digger, Slaver | NATIVE (as shared) | `game_vars.h:8-18`; **the engine never reads them** — they are pure script gvars, so the whole title system is script logic over a shared, streamed store. `GVAR_PARTY_CHILDKILLER` (`:296`) likewise |
| Town reputations (19 towns) | NATIVE (as shared) | `character_editor.cc:526` `gTownReputationEntries`, read from gvars → streamed |
| Companion recruit / dismiss | NATIVE | `op_party_add` (`interpreter_extra.cc:4028`) via dialog, which works |
| Player chat | NATIVE | `say` verb + `client_say.cc`, styled channels |
| ~~Barter price inputs~~ | NATIVE | **FIXED 2026-07-26** — `partyGetBestSkillValueFor(SKILL_BARTER, dude)`: SOLO scope (companions + the player at the table, owner ruling), and Master Trader now reads that actor instead of relying on gDude being rebound. Players are still NOT in `gPartyMembers` — see the header comment for why that would be dangerous |
| **Script party count** | **NATIVE BY RULING** | ►►►► **DO NOT "FIX" THIS.** I widened `_getPartyMemberCount()` to include players on 2026-07-26 and the owner killed it the same hour, correctly: scripts REFUSE ENTRY on this number ("leave some of your friends outside" — Vault City, the Sierra doors, escorts), so counting players walls a co-op party out of the map. A truthful count that makes the game unplayable at three seats is worse than vanilla's undercount. ►► RULE: if a script can refuse something based on a party size, players do not count; if it only SIZES or OFFERS something, they do (`partyGroupSize()` — now used by worldmap encounter sizing and the pipboy rest options) |
| **NPC gating on the player's identity** | **SHARED** | `op_get_pc_stat` → `pcGetStat(data)` with **no subject** (`interpreter_extra.cc:4947`, `stat.cc:688`) ⇒ scripts reading level/XP/karma always read slot 0. Ruling owed (`per-player-character-sheet`). Gender-gated dialog reads a proto that is male for every extra (see §1) |
| **Kill counts (the char-sheet kills list)** | **SHARED** | `gKillsByType` is one static array (`critter.cc:253`), bumped once per kill in the XP funnel (`combat.cc:5873`) and **not streamed** — it is not a gvar, so it freezes at each viewer's baseline while karma next to it updates live |
| **Faction joins** — Slavers, Hubologists, the New Reno families, NCR / Vault City citizenship, BoS, Enclave | **UNSEEN** | entirely gvar + script; no engine surface at all, so the mechanism is present and shared-by-construction. Never exercised with 2 players — and the C→D / lvar-double-fire class is exactly what would corrupt a membership state machine |
| **Companion reaction to player behaviour** (Sulik and child-killing, women, grave-digging) | **UNSEEN** | script-only: the companion's dialog/`critter_p_proc` reads the shared rep gvars and `dude_obj`. Both halves exist (gvars stream, `dude_obj` = nearest living player). **Untested**, and it inherits the two known distortions: whichever player is nearest is treated as "the dude", and the reaction fires once per proc tick, not once per player |
| ~~Dialog option scrolling~~ | NATIVE | **SHIPPED 2026-07-26** — PageUp/PageDown + arrows/wheel (the arrows only when the reply is not paging, so it keeps first claim). Scrolling down is refused unless the last render actually hid something, since only the renderer knows how many lines each option wraps to. Also fixed: a viewer could not CLICK the tenth option at all (buttons post `49 + index`, the handler only took '1'-'9'). LIVE-VERIFY OWED |
| **Empathy option colour-coding** | **ABSENT** | the per-option `reaction` is NOT on the wire — `client_dialog.cc:158` calls `gameDialogAddTextOptionWithProc(-1, text, 0, 50)`, hardcoding `GAME_DIALOG_REACTION_NEUTRAL`. The renderer that would colour them is `game_dialog.cc:2809-2833`. A player with Empathy sees no hints at all |

## 6. Progression & meta — 19 entries (NATIVE 11 / SHARED 1 / UNSEEN 3 / ABSENT 4)

| feature | verdict | evidence |
|---|---|---|
| Quest state | NATIVE (as shared) | gvars, streamed |
| Pipboy quest list | NATIVE | `pipboyInit` runs before the viewer branch; PIP key/button wired @1795420 and the lists render from streamed gvars |
| Holodisks | NATIVE | `EVENT_HOLODISK_CLEAR/ADD` (52/53) + a server-authored disk @d6267f9 |
| Pipboy status | NATIVE | read-only, per-client |
| Pipboy automap | UNSEEN | viewer-local; `automapSaveCurrent()` is a deliberate server no-op (`server_stubs.cc:320`), so the server contributes nothing and each viewer records its own |
| **Player-initiated REST** | **ABSENT** | `pipboyRest` returns false for a viewer and the alarm tab routes into vanilla's own refusal (`pipboy.cc:2048-2058`); `pipboyRestHeadless` is a **stub abort** on the server (`server_stubs.cc:492` → **stub axis**). There is no `rest` verb in either verb table |
| Heal-over-time while travelling | NATIVE | `worldmapTravelRestHeal` per travel step (`server_worldmap.cc:162`); rest-heal iterates every player actor (owner ruling, audit #18) |
| Worldmap travel | NATIVE | open to all; `wmmove`/`wmenter`/`wmesc` |
| Random encounters + accept/decline prompt | NATIVE | `EVENT_ENCOUNTER_PROMPT/CLOSE` (49/50), first-answer-wins @701160a; live-verify owed |
| ~~Encounter avoidance (Outdoorsman)~~ | NATIVE | **FIXED 2026-07-26** — GROUP scope (travel is a party act): every online living player's Outdoorsman counts, the Motion Sensor bonus checks whoever is CARRYING it, and the catch-XP is credited + addressed to the player whose skill actually carried the roll. Kept BEST-in-party, not an average: an average makes the party worse when someone joins |
| Special encounters | NATIVE | marker placement fixed 2026-07-25 (`wmCitySizeDimensions`/`wmHotspotDimensions` lock-on-demand) |
| ~~Town map / multi-entrance city choice~~ | NATIVE | **SHIPPED 2026-07-26** — the viewer runs vanilla's own town map and sends an ENTRANCE INDEX; `wmAreaResolveEntrance` resolves it server-side and refuses an out-of-range or undiscovered one (the elevator rule: a map index off the wire would be teleport-anywhere). ►► The hole that would have made it inert: "which districts have we found" is server state that never reached a viewer, so `worldmapState` now carries the visited state + a discovered-entrance bitmask for the area UNDERFOOT. LIVE-VERIFY OWED |
| Cars / the Highwayman | NATIVE | **owner-confirmed driving + fuel consumption 2026-07-25**; 4 bugs fixed (2 vanilla) @770c593. Open: whether fuel DEPLETION (running dry) behaves, and trunk-loss → permanent data loss (`worldmap-streaming-track`) |
| Time of day / day-night light | NATIVE | `WORLD_DELTA_GAMETIME` + `WORLD_DELTA_LIGHT` @8113084 |
| Save / load, N-actor | NATIVE | admin `save`/`load` + the `'PAC2'` appendix carrying every extra's body/inventory/sheet |
| Autosave | NATIVE | **owner-confirmed working 2026-07-25.** Remaining nuance, not a break: `serverAutosaveTick` gates on `wmMapIsSaveable`, so random-encounter and wilderness maps are skipped AND the latch advances — worth a design pass, never blind gate removal |
| **Endgame slideshow + narrator + credits** | **ABSENT** | `endgamePlaySlideshow` returns immediately under `serverLoopActive` (`endgame.cc:221`) and `endgamePlayMovie` sets `_game_user_wants_to_quit = 2` and returns (`:256`). Reaching the ending kills the server and nobody sees anything. `SCRIPT_REQUEST_ENDGAME` also has no server handler |
| **Freeplay after the endgame** | **ABSENT** | banked in IDEAS.md; today the endgame is terminal |
| Movies / cutscenes | NATIVE | first-ack-wins barrier + `EVENT_MOVIE_SEEN_STATE`; alt-tab wedge fixed |

## 7. The party / infrastructure — 10 entries (NATIVE 4 / SHARED 1 / UNSEEN 1 / ABSENT 4)

| feature | verdict | evidence |
|---|---|---|
| N bodies, per-player sheets | NATIVE | the "N bodies ONE sheet" era is over — per-player sheets are an owner requirement, shipped |
| Join / login / account identity | NATIVE | name-keyed accounts, `login` verb, `docs/ACCOUNT_IDENTITY_DESIGN.md` |
| Presence (leave → park, return → reattach) | NATIVE | `serverControlDrainPresence`, @00b1d99 |
| Per-actor action pacing gate | NATIVE | `server_players.{h,cc}` @0c2c0c8, kill switch `F2_SERVER_ACTION_GATE=0` |
| Companion levelling | NATIVE (by ruling) | `_partyMemberIncLevels` fires only for the host earner (`stat.cc:1071-1073`) so companions do not compound by player count |
| **Residual host-ness** | **SHARED** | no screen is host-only any more; what remains is the disengaged dialog-scope fallback and scattered scripts caching `dude_obj` (`coop-mp-track`) |
| Party members across a rebaseline | UNSEEN | party bodies are `OBJECT_NO_SAVE`; rebaseline fix @c314a91, live-verify owed |
| **Rejoin during combat** | **ABSENT** | the EXISTING combatant hangs on the map-loading screen; a reconnected player is not in the fixed turn roster. Mid-combat roster insert is a feature (`coop-mp-track`) |
| **Rejoin world fidelity** | **ABSENT/broken** | `applyBlob` does not tear down cleanly: containers vanish, inventories dangle, later SIGSEGV. Root + plan = `docs/APPLYBLOB_TEARDOWN_PLAN.md` |
| **Players on different elevations / different maps** | **ABSENT** | every combat/AI roster filters on `gElevation` (the camera); one gMap, one object list, one combat context |

---

## The number

**Denominator: 102 vanilla features/systems**, enumerated above from the engine. Counted by
NUMBER of features:

| | count | share |
|---|---|---|
| NATIVE | 60 | **58.8 %** |
| UNSEEN (wired, unverified at N>1) | 14 | 13.7 % |
| SHARED/HOST (partial at N>1) | 9 | 8.8 % |
| ABSENT | 19 | 18.6 % |
| **NATIVE + UNSEEN** ("built, believed to work") | **74** | **72.5 %** |
| **partial credit** (NATIVE 1.0, UNSEEN 0.75, SHARED 0.5, ABSENT 0) | 75.0 | **73.5 %** |

Per family (partial-credit score / entries):

| family | score | n/N (NATIVE only) |
|---|---|---|
| Items & inventory | 0.88 | 10/14 |
| Combat | 0.80 | 11/16 |
| World interaction | 0.73 | 8/13 |
| Progression & meta | 0.72 | 11/19 |
| Social | 0.72 | 8/16 |
| Character | 0.71 | 8/14 |
| Party / infrastructure | 0.53 | 4/10 |

**Weighted by how much of a playthrough each family touches** (combat 25 %, social 20 %, items
15 %, world 15 %, progression 15 %, character 5 %, infra 5 %) the two views agree: **~75 %**.

**They diverge on one honest reading, and it matters.** Counting *how much of a Temple→Enclave
co-op playthrough is reachable and correct*, the number is materially lower — call it **60-65 %** —
because four ABSENT entries are not features, they are gates: dead **elevators** lock out the
interiors of Sierra, Navarro, the Military Base, Vault City's vault, Vault 15, the Shi Temple and
the Wanamingo Mine; the missing **town map** removes district-level navigation everywhere; the
**endgame** cannot be watched and kills the server; and **rejoin corruption** ends sessions. A
feature-count percentage cannot see that one dead level-picker outweighs twenty working perks.

Say both numbers out loud: **~75 % of the feature inventory, ~60-65 % of a playthrough.**

## Top 10 gaps, ranked by how much of a co-op playthrough they damage

1. **Elevators dead** — `elevatorSelect` is the un-overridden base `-1`. Roughly 15 live FO2
   elevators; several whole locations become unenterable.
2. **Rejoin / hot-join corrupts the world** — containers vanish, later crash. Ends sessions;
   `docs/APPLYBLOB_TEARDOWN_PLAN.md`.
3. **No town map** — always the city's first map; kills district navigation in New Reno, Vault
   City, San Francisco, NCR, Broken Hills, Redding.
4. **Endgame absent** — the ending cannot be seen and the server exits. No campaign payoff.
5. **All-players-dead soft-lock** — no in-band recovery from a wipe.
6. **Steal + sneak absent** — a whole build (thief/stealth) is unplayable, and Silent Death /
   Silent Running are dead with it.
7. **Party/companion AI anchored on the host** — companions ignore extras in and out of combat.
8. **NPC notice is nearest-player-only, and lvar state machines double-fire** — reads as content
   bugs; corrupts quest and faction state machines.
9. **Player-initiated rest absent** — no way to pass time or heal on purpose outside travel.
10. **Two-floor and concurrent combat absent** — one fight freezes everyone, including a player
    on another floor who can neither join nor leave.

## The 5 subtlest gaps a player will notice before we do

1. **Empathy shows no colours.** Every dialog option renders neutral because the per-option
   reaction never reaches the wire (`client_dialog.cc:158` hardcodes `50`). A player who picked
   Empathy will say "my perk does nothing" long before we look.
2. **An extra player's Barter and Outdoorsman are worth zero.** `partyGetBestSkillValue` walks
   `gPartyMembers`, which extras are never added to. The party's designated talker gets vanilla
   prices; the party's designated scout never avoids an encounter. Both look like bad luck.
3. **The kills list on the character screen is frozen.** `gKillsByType` is a static, not a gvar,
   so it does not stream — and it sits on the same screen as karma, which now updates live. The
   inconsistency is what gives it away.
4. **Scripts think the party is smaller than it is.** `METARULE_PARTY_COUNT` under-counts by the
   number of extra players, so "leave some of your friends outside" gates let too many in — or
   the reverse, when a script counts to decide a reaction.
5. **Addiction and gender belong to the host.** Every extra is male for dialog purposes and shares
   the host's addiction flags: a player who never touched Jet can get the host's withdrawal
   behaviour, and gender-gated lines read off slot 0. Both are silent and both look like content.
