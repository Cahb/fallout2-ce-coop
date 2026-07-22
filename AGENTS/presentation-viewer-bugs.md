---
name: presentation-viewer-bugs
description: "BANKED open viewer/sync bugs from live play — gib corpse resyncs to a generic death frame, can't loot gibbed critters, NPC in-combat draw not seen, throw-retrieve desync, walk/run anim desync. Viewer-side = no headless oracle; live-verify each. Triage/home per item."
metadata: 
  node_type: memory
  type: project
  originSessionId: 253d29b4-5e7c-4ce9-82bf-0ceb771292c4
  modified: 2026-07-19T05:09:37.948Z
---

Owner live-play observations 2026-07-18 (post ranged-slice c85fb0e). All VIEWER-side →
NO headless oracle (goldens/purity only oracle the server sim); live-verify each per
[[visual-verification-protocol]] [[anim-decouple-verification]]. Hypotheses UNVERIFIED —
per [[dont-declare-not-a-bug-confidently]], confirm mechanism before claiming a fix.

## BUG 2 — gib corpse settles to a GENERIC frame (POLISH — owner dropped, not trivial)
Symptom: rocket/gib combat kill plays the correct BITS-AND-PIECES death anim, but the resting
("idle WERE_DEAD") corpse is a plain death, not gib remains. Also on resync/join. Owner note:
"better for readability but a bug per se." OWNER DIFFERENTIAL CLUE: a placed-BOMB explosion
gibs the corpse CORRECTLY, but an in-COMBAT rocket does not.
PARTIAL ROOT CAUSE (code-confirmed): _show_death does NOT set the corpse fid (actions.cc:588
`anim<48 && anim>63` is IMPOSSIBLE — vanilla quirk — only flat/outline/itemDrop). The viewer's
gib corpse comes from the death ANIMATION's final frame (record channel). The headless server
finalizes via critterKill(defender,-1,false) → critter.cc:852 anim=-1 → LAST_SF_DEATH_ANIM =
GENERIC SF corpse, rides OBJECT_DELTA, OVERWRITES the viewer's gib. BUT NOT the full story:
BOTH the explosion path (actions.cc:2073/2078/2084) AND the combat path (combat.cc:5591) use
critterKill(...,-1,...) — so the bomb-vs-rocket differential is NOT critterKill alone; it must
be a presentation-CHANNEL/timing difference (bomb = EVENT_EXPLOSION_FX cue + client-local
actionExplode keeps the gib corpse; rocket = record channel + delta overwrite). THAT unexplained
differential is why it is NOT trivial → PARKED as polish. Note critterKill is SF-single-frame
(objectSetFrame(0)); gib deaths have NO SF variant (corpse IS the anim's last frame), so a real
fix = set fid=death-anim + frame=last (reuse _pick_death/_check_death actions.cc:293/370) AND
resolve the channel/delta-overwrite. Cousin of [[frame-index-render-gotcha]] + the OBJ_CREATE-
rotation gap. Would REBLESS any golden with a gib kill. HOME: death/corpse family, low priority.

## BUG 3 — cannot LOOT blown-up (gibbed) critters
Symptom: explosion-killed critters DROP items but looting them does nothing (no container
opens, no response). MECHANISM (hypothesis): gib death may leave the corpse in a state the
loot interaction rejects (e.g. gib corpses flagged differently, or itemDropAll scattered the
items to the ground rather than into a lootable corpse container, or the corpse object's
can-loot gate fails). CHECK: does _action_loot_container / the loot verb accept a gib corpse?
where do gib-dropped items go (ground vs corpse inventory)? Compare vanilla SP loot of a
gibbed body. HOME: loot interaction + death/corpse family. Likely SEPARATE from 1/2.

## BUG 5 — NPC in-combat weapon EQUIP/draw animation not seen on viewer (owner 2026-07-18)
Symptom: an NPC already wielding a pistol fires fine (armed fid replicated), but the EQUIP/draw
(take-out) animation when a critter equips a weapon mid-combat is missing — critter snaps to
armed without the draw motion. Melee/unarmed unaffected. NOT a convergence/throw regression:
inventory.cc is untouched by A/B/convergence, and the wield take-out ships on a SEPARATE presseq
channel (independent of attackResult). CODE SAYS IT SHOULD WORK: _inven_wield (combat_ai.cc:2635)
→ _invenWieldFunc(animate=true); in-combat + weaponAnimationCode!=0 → animationRegisterTakeOut-
Weapon + recordedDraw=true → presSeq(actor=critter) (inventory.cc:485-506). So if not seen,
either (a) the NPC started ARMED (map-placed) → no mid-combat equip → nothing to draw = NOT a bug;
(b) tribal-art: the wield artExists(fire fid) check (inventory.cc:378) FAILS for tribal skin +
gun → wield returns -1 (no equip at all); or (c) the take-out presseq IS shipped but not played on
the viewer (real pump/playback gap). DISCRIMINATOR: trace the ship site (log non-dude in-combat
"TAKE_OUT shipped"). Likely PRE-EXISTING (weapon-draw slice db327ae was live-verified for a DUDE/
out-of-combat spear draw, NOT an NPC in-combat draw), surfaced by the cleaner post-convergence
presentation. HOME: weapon-draw/wield family. Chase after the throw arc.

## BUG 6 — thrown-weapon RETRIEVE CYCLE not presenter-driven (owner 2026-07-18, after throw arc B)
Throw ARC works (spear flies + lands at target). But the AI retrieve→pickup→re-equip→re-throw
cycle around it desyncs: (A) pickup WALK not animated (combat MOVE not recorded yet); (B) re-EQUIP
after pickup not shown ([[= BUG 5]]); (C) ground spear removed on SERVER at pickup but the DISCONNECT
does NOT sync to the viewer → phantom spears ACCUMULATE (owner saw 3 on ground while guard held 1;
server conserves 1 — verified). A introduced ground spears which SURFACED this item-removal-sync gap.
Also owner: occasional "0-frame HIT" (attack lands, no anim) = same missing-state-transition class as
B5. ROOT (owner insight, correct): the record channel does not yet own the WHOLE combat turn (move +
pickup + equip + item churn) — only the discrete launch/flight/land. FIX = the post-throw roadmap:
combat MOVE (locomotion in-combat) + NPC wield presentation (B5) + item ground↔inventory sync. Not a
bug in throw B. HOME: combat-MOVE slice + wield family + object/inventory-delta sync.

## BUG 4 — walk/run animation desync out of combat AND in combat
Symptom: (out of combat) addicts RUN/flee when they should just WALK; (in combat) guards
flip between run and walk unexpectedly. MECHANISM (hypothesis): the walk-vs-run ANIM choice
is locally-derived on the viewer (the "walk-frame guess", spec's KEEP lane for free-roam
glide) and mis-derives run vs walk — the authoritative run/walk flag isn't faithfully
replicated. Connects DIRECTLY to the Locomotion family: free-roam glide stays STATE-lane with
locally-derived walk frames (accepted #2/#7 cosmetics), and in-combat MOVE is the next-but-one
roadmap item (fold into recorded seq → authoritative run/walk). CHECK: is artRun/run-vs-walk
in the MOVE event payload? does the viewer honor it or re-guess from distance/AP? HOME:
Locomotion family (combat MOVE slice will address the in-combat half; free-roam half is the
KEEP-lane cosmetic — may need the run/walk flag added to the MOVE event).

## TRIAGE / ORDER
2+3 cluster around EXPLOSION/DEATH/CORPSE presentation+sync (corpse-frame polish + gib-loot);
5 is the wield/weapon-draw family; 6 is the throw-retrieve tail (see [[combat-full-record-channel]]);
4 is Locomotion (fold the in-combat half into the combat-MOVE work). See [[pres-record-build-track]].
