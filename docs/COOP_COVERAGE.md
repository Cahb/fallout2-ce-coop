# Co-op coverage — vanilla Fallout 2 vs our tapped engine

Synthesis of a three-axis sweep (2026-07-25). The detail lives in three sibling documents;
this one holds the numbers, what the axes AGREE on, and one merged action list.

| axis | document | headline |
|---|---|---|
| The vanilla feature inventory, ruled one by one | `COOP_COVERAGE_FEATURES.md` | ~75 % of features, **~60-65 % of a playthrough** |
| Simulated but NOT FORWARDED (right sim, wrong screen) | `COOP_COVERAGE_WIRE.md` | ~60 % — state streams well, **addressing is half-done** |
| What the server cannot execute at all (stub/abort surface) | `COOP_COVERAGE_STUBS.md` | **93 %** of the severance surface safe; residue = 9 items |

## ⚠ THE THREE HEADLINES ABOVE ARE NOT COMPARABLE — do not read them as one number

They answer three different questions over three different denominators, and stacking them in one
table (as this doc originally did) invites exactly the wrong reading: *"93 %, so we're nearly
done."* We are not.

| headline | denominator | what a % of it actually means |
|---|---|---|
| **~75 %** | **102 vanilla features/systems**, hand-enumerated from engine tables (opcode tables, request handlers, verb tables, skill/perk/kill-type tables) | THE ONLY ONE that answers "how much of Fallout 2 works in co-op" |
| **~60 %** | a QUALITY axis, not an inventory: 63 personal-feedback call sites (54 % addressed), 26 feedback systems (58 % clean), 8 request hooks (25 %) | "is the right thing shown to the right person" — a sub-property OF the features above, double-counting them |
| **93 %** | **232 client-severance STUB functions** — engine plumbing | "does the server survive calling this", nothing about features. 7 % of 232 is the abort landmines. Quoting it beside 75 % is misleading and it is only here for completeness |

**Precision, stated honestly.** The feature axis is 60/102 strictly NATIVE (58.8 %); 74/102 (72.5 %)
if you count the 14 UNSEEN entries that are wired but have never been run with two real players;
73.5 % with partial credit; ~75 % once weighted by how much of a playthrough each family touches.
The spread between 58.8 % and 75 % IS the uncertainty, and the 0.75 credit given to UNSEEN is a
judgement, not a measurement. The denominator is itself a judgement: a differently-cut inventory of
80 or 150 entries would move every number by several points. **Treat these as ±5 and use the ranked
gap list, not the percentage.**

## The number, honestly

**~75 % of the feature inventory is built (60 of 102 NATIVE, +14 wired-but-unverified).
~60-65 % of an actual Temple→Enclave co-op playthrough is reachable and correct.**

Say both, because the gap between them IS the finding: a feature count cannot see that one dead
level-picker outweighs twenty working perks. Four absent entries are not features, they are
GATES — elevators (Sierra, Navarro, the Military Base, Vault City's vault, Vault 15, the Shi
Temple, the Wanamingo Mine), the town map (no district navigation anywhere), the endgame (cannot
be watched, and reaching it kills the server), and rejoin corruption (ends sessions).

Per family (partial credit: NATIVE 1.0, UNSEEN 0.75, SHARED 0.5, ABSENT 0):

| family | score |
|---|---|
| Items & inventory | 0.88 |
| Combat | 0.80 |
| World interaction | 0.73 |
| Progression & meta | 0.72 |
| Social | 0.72 |
| Character | 0.71 |
| Party / infrastructure | 0.53 |

## ►► What the three axes AGREE on: the shape of what is missing

Almost nothing is missing because the SIMULATION is missing. Three causes account for nearly
every gap, and each has a different cure:

1. **A HOOK RETURNS A SENTINEL.** `ScriptRequestHandler` has 8 virtuals; the server overrides 2
   (`script_request_handler_server.cc:26-80`). The other six fall through to a base class that
   answers "nothing happened" — so elevators read as "the player cancelled the picker", the town
   map clears its transition, the endgame clears its flag, and STEAL silently does nothing. **No
   error, no log, no crash.** Two independent axes found the elevator case without knowing about
   each other; it appears in no memory or doc before today.
2. **ADDRESSING, not computation.** Roughly half the feedback ABOUT one player is shouted at
   everyone (93 `consoleMessage` broadcasts, of which 29 are provably personal — the code already
   decided whose message it was and then broadcast it), and a handful of things only the host ever
   sees. The sim is right; the audience is wrong. Note the project's own rule: `subject == gDude`
   is NOT a host test (a `ServerActorScope` rebinds it), so these are "right author, wrong
   audience" — the true host-only sites are the ones OUTSIDE any scope (queued events, AI turns,
   script opcodes).
3. **VERIFICATION DEBT AT N>1.** 14 features are wired and believed correct but have never been
   exercised with two real players. That is not build debt, and no golden can retire it: only one
   gate in the tree runs N>1 (`scripts/check_sheet.sh`).

Plus a small fourth, now **CLOSED**: **live abort landmines** — 6 when this was written, **0 now**
(2026-07-25). `pipboyRestHeadless` went with the rest work; `gameShowDeathDialog` with the
radiation fix; and the remaining four in one pass — `automapShow` (the Motion Sensor now opens the
USER's own automap over an addressed event), `endgamePlayMovie` (victory is SURVIVABLE, not yet
watchable — it mirrors the client's own guarded branch), `textObjectsRemoveByOwner` (`float_msg`'s
clear form, a benign no-op) and `showMesageBox` (prints the real "master.dat is missing" line where
a headless operator can read it). Nothing in the enumerated severance surface can now abort the
process on a path ordinary play walks down.

## Merged action list

Ranked by damage to a real session, with the axis that found each and an effort guess.

| # | fix | why it is high | effort |
|---|---|---|---|
| ~~1~~ | ~~**`gameShowDeathDialog` non-fatal**~~ | **DONE** — `_process_rads` addresses the death line to the dying actor on a dedicated server (and the rad/poison warnings with it); the stub is headless-safe, so no future path turns one player's rad counter into everyone's process death | done |
| ~~2~~ | ~~**`dudeEnableState`/`dudeDisableState` take a subject**~~ | **DONE (2 of 3)** — the trio + `dudeIsSneaking` take a subject, mark the sheet row dirty, and refuse a non-player subject (a shared proto must never be marked). Sneak is per-actor (incl. `_sneak_working`) and now has a `sneak` VERB; the level-up badge is per-actor AND re-derived (`pcLevelUpBadgeRefresh`) since the server has no character screen to clear it. **Addiction is NOT fixed** — its flag is downstream of 9 SHARED gvars, so a per-actor flag could never be cleared; still blocked on the gvar funnel | done |
| ~~3~~ | ~~**The three other abort→benign conversions**~~ | **DONE 2026-07-25**, plus `showMesageBox`. The Motion Sensor got a real wiring rather than a muted no-op (addressed `EVENT_AUTOMAP_OPEN`, latched like the elevator panel); `endgamePlayMovie` makes victory survivable but still unwatchable — streaming the slideshow is #7 | done |
| 4 | **Route the 29 personal broadcasts** — the two XP lines are **DONE** (`_combat_give_exps` + `_show_skill_use_messages`, addressed on `kMsgChannelReward`; an extra no longer earns in silence), as are the radiation/poison warnings. REMAINING: examine (`proto_instance.cc:43-46`, highest frequency) and the rest of the list | every player still reads "You see: a locker" when one of them looks at it | leaf per site |
| ~~5~~ | ~~**Elevators**~~ | **DONE** — server-side `elevatorSelect` streams vanilla's panel to the rider (`EVENT_ELEVATOR_PROMPT`), `elev <level>` rides. The tables moved to `elevator_data.cc` (f2_core) so the SERVER resolves destinations: the client only ever names a BUTTON. Gated by `scripts/check_rest_elevator.sh` | done |
| ~~6~~ | ~~**Rest**~~ | **DONE** — the loop moved to `rest.cc` (f2_core) with the presentation as a frame callback, so single-player and the server share ONE simulation; `rest`/`restopt` verbs + an admin `rest`. Retired one of the six abort landmines (`pipboyRestHeadless`) | done |
| 7 | **Server-side `endgame()`** — evaluate the 52 gvar-matched slides and stream them | no campaign payoff exists. ⚠ MUST land with #3's `endgamePlayMovie` or wiring it re-introduces the abort. `FULL_RUN_RECON.md` proved the ending is a pure function of 16 gvars | medium |
| ~~8~~ | ~~**extras are never in `gPartyMembers`**~~ | **DONE 2026-07-26** — and NOT by joining the registry, which would have been the trap: that list is saved, levelled, position-synced and garbage-collected as companions, and `partyFixMultipleMembers` `objectDestroy()`s what it reads as a duplicate (player bodies escape it only because their pids are not in party.txt). Instead the four READERS widened, in two scopes — GROUP for travel/party-count, SOLO for a trade or a skill use. Closed: barter price, Outdoorsman + the Motion Sensor bonus, encounter SIZING + the pipboy rest option, and an extra's skill use being substituted by a worse companion. ►► `METARULE_PARTY_COUNT` deliberately left counting companions only — see the FEATURES row | done |
| 9 | **Town map** — one wire event for the entrance list | you always arrive at a city's FIRST map; district navigation does not exist. The viewer sends `wmEnter` unconditionally (`worldmap_ui.cc:270-291`) | medium |
| 10 | **Addressing mechanism for personal sfx/fades** — add `actorNetId` to `sfxPlay`/`screenFadeOut`/`screenFadeIn`, and DECODE the fades | closes a whole category: the level-up chime plays on everyone's speakers, and `EVENT_FADE_OUT/IN` are emitted by the server and **never decoded** by the client, so the time-skip fade is thrown away | medium |

## The 5 subtlest gaps — a player notices these before we do

1. **Empathy shows no colours.** Every dialog option renders neutral: `client_dialog.cc:158`
   hardcodes reaction `50` (NEUTRAL), so the per-option reaction never reaches the wire. A player
   who took Empathy will report "my perk does nothing".
2. ~~**An extra's Barter and Outdoorsman are worth zero**~~ — **FIXED 2026-07-26** (#8). Kept here
   because the SHAPE recurs: both looked like bad luck rather than a bug, which is what makes this
   class expensive to find. Anything phrased "the party's best at X" is a candidate-set question.
3. **The kills list on the character screen is frozen.** `gKillsByType` is a bare static
   (`critter.cc:253`), not a gvar — so it never streams, while karma *next to it on the same
   screen* now updates live. The inconsistency is what gives it away.
4. **Scripts think the party is smaller than it is** — and it STAYS that way, by ruling
   (2026-07-26). Scripts refuse entry on that number, so counting players turns every
   "leave your friends outside" gate into a wall. Sizing and offering see the real group
   (`partyGroupSize()`); gating sees companions only.
5. **Addiction and gender belong to the host.** Every extra is male for gender-gated dialog, and
   addiction lives in 9 shared gvars — a player who never touched Jet can inherit the host's
   withdrawal. ⚠ The ADDICTED indicator flag is deliberately still host-only even though
   `dudeEnableState` could now place it per-actor: `dudeIsAddicted` reads the shared gvars, so a
   per-actor flag would light and never clear. Fix the gvars first, then the flag falls out.

## Corrections this sweep forced on our own notes

Recorded because each was load-bearing somewhere:

- **The 13 script-reachable sfall leaves no longer abort** — they answer headless and log once
  (`serverStubHeadlessOnce`, `server_stubs.cc:151`). The code now states the rule: any stub a
  script opcode can reach belongs in that set, never in the aborting one.
- **`interfaceGetCurrentHitMode` is SAFE** (benign `return -1`); the "latent server crash"
  framing is retired. P5's H6 cut-list item is DONE, and all six "still open tail" items are
  benign bodies. What remains of the cut list is the ~157 renderer-tail aborts, which are never
  de-stubbed by standing rule — i.e. **not on the co-op critical path**.
- **Karma / reputation / quest gvars DO stream now** (`gvarDeltaScan`, `object_delta.cc:321-361`),
  and the viewer's pipboy is reachable. Notes saying otherwise are stale — which is what makes
  the shared, party-based karma model the owner ruled for actually *work* today.
- **The retired bespoke cues are DEAD CODE at both ends.** `EVENT_DOOR_STATE`/`ACTION_ANIM`/
  `EXPLOSION_FX`/`WEAPON_TAKE_OUT` have no core emitter left and the client never had decoders, so
  `F2_SERVER_PRES_RECORD=0` is **not** a working fallback: doors, gestures, explosions and weapon
  draws go silent. The note calling them "the DEFAULT (flag off)" is wrong.
- **`F2_MOVIES` is default ON**, not off as `server_stubs.cc:406-414` claims — so the "a viewer
  fails to render a cutscene → the server parks on a barrier no operator can release" failure mode
  is live by default.

## What these numbers do NOT say

Every figure above is **static reachability plus code reading**. None of it is behaviour observed
with two real players, and the sweep deliberately did not run the engine. Two of the six aborts
rest on unverified assumptions about which opcodes shipped scripts actually emit — settle those by
tracing a real run, not by reading more. And per the project's own lesson, `scripts/srv_sweep.sh`
over all 155 maps is the oracle that would name any *reached* stub this static pass mis-triaged.
