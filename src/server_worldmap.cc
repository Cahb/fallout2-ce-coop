#include "server_worldmap.h"

#include <cstdio>
#include <vector>

#include "game.h"
#include "map.h"
#include "presenter.h"
#include "scripts.h" // scriptsDisable — the reversible half of the leaving-the-map teardown
#include "sim_clock.h"
#include "worldmap.h"
#include "worldmap_intent.h"
#include "worldmap_ui.h"
#include "object.h"

namespace fallout {

static std::function<bool()> gWorldmapServerPump;
static bool gWorldmapServerDriverActive = false;

void worldmapSetServerPump(std::function<bool()> pump)
{
    gWorldmapServerPump = std::move(pump);
}

bool worldmapServerActive()
{
    return gWorldmapServerDriverActive;
}

// Fog of war: the per-subtile visited/known grid the viewer paints the worldmap
// from. It is written by a scatter of sim paths (wmMarkSubTileRadiusVisited and
// friends, plus city visit/known transitions), so rather than hook every writer
// we flatten the whole grid and diff it against a shadow copy. FO2 has 20 tiles
// => 840 subtiles => an 840-byte event, so shipping it whole is cheaper than
// per-subtile delta bookkeeping would be.
static std::vector<unsigned char> gSubtileShadow;

static void emitSubtilesIfChanged()
{
    const int count = wmMaxTileNum * SUBTILE_GRID_HEIGHT * SUBTILE_GRID_WIDTH;
    if (count <= 0) {
        return;
    }

    std::vector<unsigned char> flat;
    flat.reserve(count);
    for (int tileIndex = 0; tileIndex < wmMaxTileNum; tileIndex++) {
        TileInfo* tile = &(wmTileInfoList[tileIndex]);
        for (int column = 0; column < SUBTILE_GRID_HEIGHT; column++) {
            for (int row = 0; row < SUBTILE_GRID_WIDTH; row++) {
                flat.push_back((unsigned char)tile->subtiles[column][row].state);
            }
        }
    }

    if (flat == gSubtileShadow) {
        return;
    }
    gSubtileShadow = flat;
    presenter()->worldmapSubtiles(flat.data(), (int)flat.size());
}

static void emitState()
{
    // WHICH DISTRICTS ARE KNOWN. Only for the area underfoot — it is the only one whose
    // town map can open — and it has to be shipped because it is sim state the viewer's
    // own worldmap data cannot know: without it a viewer never sees that a city has a
    // layout to choose from, and every arrival lands on the front door.
    int visitedState = 0;
    unsigned int entranceMask = 0;
    if (wmGenData.currentAreaId != -1) {
        CityInfo* city = &(wmAreaInfoList[wmGenData.currentAreaId]);
        visitedState = city->visitedState;
        int count = city->entrancesLength;
        if (count > 32) {
            count = 32; // the mask is 32 bits; no FO2 city comes close
        }
        for (int index = 0; index < count; index++) {
            if (city->entrances[index].state != 0) {
                entranceMask |= 1u << index;
            }
        }
    }

    presenter()->worldmapState(
        wmGenData.worldPosX, wmGenData.worldPosY,
        wmGenData.walkDestinationX, wmGenData.walkDestinationY,
        wmGenData.isWalking, wmGenData.walkDistance,
        wmGenData.carFuel, wmGenData.currentAreaId,
        wmGenData.isInCar, visitedState, entranceMask);

    // ►► THE CLOCK TRAVELS TOO. Travel burns game time (worldmapTravelClockTick →
    // wmGameTimeIncrement), but the only emitter of WORLD_DELTA_GAMETIME is the beat
    // scan (object_delta.cc), and serverTick is PARKED for the whole worldmap session
    // because this driver owns the loop. So the viewer's clock stayed frozen at the
    // moment it entered and the worldmap's date/time readout never moved for the whole
    // trip — owner-reported. Same shape as the simClockAdvance hole in the step below:
    // a block-and-pump driver must ship what the parked spine would have shipped.
    // Coupled to the state emit on purpose — position and time-of-day are read off the
    // same screen, so they must not drift apart.
    presenter()->worldDelta(WORLD_DELTA_GAMETIME);
}

int worldmapServerDriver()
{
    ScopedGameMode gm(GameMode::kWorldmap);

    // ►►►► THE LEAVING-THE-MAP TEARDOWN BELONGS TO THE TRANSITION, NOT TO THE SCREEN.
    // It used to run here, as `wmTransitionSaveMap(); wmTransitionSuspendScripts();`,
    // mirroring vanilla's wmInterfaceInit. Vanilla can afford that because its worldmap
    // has exactly one exit — enter a location — so a mapLoadById ALWAYS follows and
    // rebuilds what the teardown destroyed. OURS HAS MORE EXITS: the player's ESC
    // (WM_INTENT_ESCAPE) and every pump bail (in combat, no clients, no claimant,
    // terminal quit) leave with map == -1 and load nothing. The teardown had already
    // happened, so the server kept simulating a world it had just dismantled:
    //
    //   * on a SAVEABLE map, _map_save_in_game(true) reaches its `_obj_remove_all() +
    //     _proto_remove_all() + _square_reset()` tail (map.cc) — every object gone, and
    //     no presenter()->worldClear() with it, so viewers kept drawing a world the
    //     server no longer had. This is the "black map" report.
    //   * on a RANDOM ENCOUNTER map that tail is SKIPPED (`if (a1 && !wmMapIsSaveable())`
    //     erases the .SAV and returns), so the objects survived — but
    //     wmTransitionSuspendScripts' _scr_remove_all() had already deleted every script
    //     on the map. Measured on a spore-plant encounter: 26 scripted critters before,
    //     1 after. The plants still stand there, solid and visible, with no
    //     critter_p_proc, no AI, no timers, and combat can never start again. You can
    //     walk, and nothing else works.
    //
    // Both faces are one bug: an irreversible teardown performed for a screen that may
    // not lead anywhere. So only the REVERSIBLE half runs at entry — scriptsDisable(),
    // which wmTransitionResumeScripts() undoes exactly — and the destructive half moved
    // to the mapLoadById below, where it is byte-identical to what it did here (and
    // where mapLoad would do it anyway: map.cc calls _map_save_in_game(true) and
    // _obj_remove_all() -> _scr_remove_all() itself).
    //
    // Disabling is enough to keep the map we are leaving inert for the whole trip:
    // scriptExecProc() is the single funnel for every proc, timed events included
    // (queue.cc's EVENT_TYPE_SCRIPT handler is scriptEventProcess -> scriptExecProc),
    // and it early-returns on !gScriptsEnabled. Travel advances game time through
    // wmGameTimeIncrement, which DOES run queueProcessEvents — those events still fire,
    // they just cannot run script code, which is the same outcome the removal bought.
    scriptsDisable();

    // The car-placement scripts must not think the car is still sitting on the map we
    // just left, or they decline to place it (and its trunk) on the next one — see
    // wmCarClearPlacedTile. sfall's hook point is wmInterfaceInit; on a dedicated
    // server that UI is on another machine, so the equivalent is here, where the
    // authoritative worldmap session begins.
    wmCarClearPlacedTile();

    wmMatchWorldPosToArea(wmGenData.worldPosX, wmGenData.worldPosY, &(wmGenData.currentAreaId));

    presenter()->worldmapBegin();

    // ►► STATE BEFORE THE SCREEN OPENS. Same reasoning as the fog sync below, and a
    // worse failure: the viewer's wmGenData still holds whatever its last LOCAL
    // worldmap session left (position, destination, area, fuel, isInCar), and until
    // this event lands it renders the party at a stale position with NO CAR. The car
    // is the sharp edge — wmInterfaceInit locks the car art ONCE, gated on isInCar
    // (worldmap_ui.cc), so a viewer that opens the screen believing it is on foot has
    // no car sprite for the WHOLE trip, and its first click is aimed from the wrong
    // marker. Begin + state leave here back-to-back in one wire batch and the modal is
    // opened later from the client's main loop, so isInCar is already authoritative by
    // the time the screen initialises. This matters for EVERY entry (a script's
    // `give_car_to_party` metarule from the car's use_p_proc is what drives), not just
    // the car — an on-foot trip had the same stale-marker first click.
    emitState();

    // Full fog sync on entry: clear the shadow so the first diff always ships
    // the whole grid. A viewer that just opened the worldmap has whatever fog
    // its own last session left behind, which is not authoritative.
    gSubtileShadow.clear();
    emitSubtilesIfChanged();

    // One line per ENTRY, matching the exit line below. `inCar=1` says the trip came
    // from the car metarule; a bogus pos/area here means the driver started from state
    // the last session left behind rather than the map we just left.
    fprintf(stderr, "[wmsrv] driver enter: pos=%d,%d area=%d inCar=%d fuel=%d dude=%d\n",
        wmGenData.worldPosX, wmGenData.worldPosY, wmGenData.currentAreaId,
        wmGenData.isInCar ? 1 : 0, wmGenData.carFuel,
        gDude != nullptr ? gDude->netId : -1);

    unsigned int partyHealTime = 0;
    int map = -1;

    // ►► THE POSITION THE TRANSITION LEFT US AT — the anchor the ESC undo restores.
    // The map behind this screen is still fully resident (see the teardown note at the
    // top), and it belongs to THIS worldmap position: the party stepped off it here.
    // Anything that ends the session without a mapLoadById is therefore only coherent
    // if the party is put back here too. Captured after the entry wmMatchWorldPosToArea
    // so it pairs with the currentAreaId the viewer was just told about.
    const int entryPosX = wmGenData.worldPosX;
    const int entryPosY = wmGenData.worldPosY;

    gWorldmapServerDriverActive = true;

    while (true) {
        if (_game_user_wants_to_quit != 0) {
            break;
        }

        // Paced travel: each outer-loop iteration runs at most one step, then
        // yields to the viewer via the pump so the client can animate.
        if (wmGenData.isWalking) {
            // Each travel step consumes one server tick's worth of sim time.
            // MUST advance the clock ourselves: serverTick — the only other
            // simClockAdvance caller — is parked for the whole worldmap session
            // because this driver owns the loop. Without this the sim clock is
            // FROZEN across the entire journey, and both cadences keyed to it
            // silently never fire again after their first check:
            //   * wmRndEncounterOccurred's 1500ms rate limit  -> NO random
            //     encounters, ever;
            //   * worldmapTravelRestHeal's 1000ms cadence     -> the party
            //     never heals while travelling.
            // Same fix, same reason, as the wmtravel probe (command.cc).
            simClockAdvance(kServerTickDelta);
            unsigned int now = simClockNow();

            int worldX = wmGenData.worldPosX;
            int worldY = wmGenData.worldPosY;

            worldmapTravelStep(worldX, worldY);

            if (worldmapTravelRestHeal(now, partyHealTime)) {
                partyHealTime = now;
            }

            worldmapTravelMarkVisited();

            if (worldmapTravelClockTick()) {
                if (_game_user_wants_to_quit != 0) {
                    break;
                }
            }

            if (wmGenData.isWalking) {
                if (worldmapTravelEncounterCheck()) {
                    if (wmGenData.encounterMapId != -1) {
                        map = wmGenData.encounterMapId;
                    }
                    wmGenData.isWalking = false;
                }
            }

            emitState();
            emitSubtilesIfChanged();

            // A random encounter fired and staged its map. LEAVE NOW — the tail
            // below performs the mapLoadById. Falling through to `continue` would
            // re-enter the loop not-walking, and the not-walking branch BLOCKS in
            // the pump waiting for a viewer intent: the encounter would be staged
            // but never entered, and the player would sit on a frozen worldmap
            // until they happened to click somewhere. (The bottom-of-loop
            // `if (map != -1) break` is only reachable after an intent is popped,
            // so it does not cover this path.)
            if (map != -1) {
                break;
            }

            // Pump before next step so the viewer has time to render this frame.
            if (gWorldmapServerPump != nullptr) {
                if (!gWorldmapServerPump()) {
                    break;
                }
                if (worldmapIntentPending()) {
                    wmGenData.isWalking = false;
                }
            }

            continue;
        }

        // Not walking — block-and-pump for viewer intents.
        WorldmapIntent intent;
        bool haveIntent = worldmapIntentPeek(&intent);

        if (gWorldmapServerPump != nullptr) {
            while (!haveIntent) {
                if (!gWorldmapServerPump()) {
                    break;
                }
                haveIntent = worldmapIntentPeek(&intent);
            }
        }

        if (!haveIntent) {
            break;
        }

        worldmapIntentPop();

        // One line per viewer intent actually CONSUMED. The failure this catches is
        // the one that reads as "my click did nothing": a click that never became an
        // intent prints nothing here, which separates a lost click (viewer/wire) from
        // a click the driver acted on (sim).
        fprintf(stderr, "[wmsrv] intent kind=%d x=%d y=%d area=%d\n",
            (int)intent.kind, intent.x, intent.y, wmGenData.currentAreaId);

        if (intent.kind == WM_INTENT_MOVE) {
            wmPartyInitWalking(intent.x, intent.y);
            emitState();
            emitSubtilesIfChanged();
        } else if (intent.kind == WM_INTENT_ENTER) {
            if (wmGenData.currentAreaId != -1) {
                CityInfo* city = &(wmAreaInfoList[wmGenData.currentAreaId]);
                // intent.x is the town-map ENTRANCE the traveller picked, or -1 for
                // "wherever this city's front door is" — an unvisited city has no
                // layout to show, so the viewer cannot pick and does not try.
                bool resolved = intent.x >= 0
                    ? wmAreaResolveEntrance(intent.x, &map) != -1
                    : wmAreaFindFirstValidMap(&map) != -1;
                if (!resolved && intent.x >= 0) {
                    // A refused entrance must not silently become the front door: that
                    // would let a bad index quietly relocate the party. Fall back only
                    // to the same answer the player would have got without a town map.
                    fprintf(stderr, "f2_server: wmenter entrance %d refused (out of range or undiscovered)\n", intent.x);
                    resolved = wmAreaFindFirstValidMap(&map) != -1;
                }
                if (resolved) {
                    city->visitedState = 2;

                    if (wmGenData.isInCar) {
                        wmGenData.isInCar = false;
                        if (wmGenData.currentAreaId == -1) {
                            wmCarParkAtMapArea(map);
                        } else {
                            wmGenData.currentCarAreaId = wmGenData.currentAreaId;
                        }
                    }
                }
            } else {
                // Standing on open wasteland, not over any known area: vanilla
                // still lets you drop into the world here (wmWorldMapFunc's
                // `else { map = 0; }`). Map 0 is the generic wilderness map —
                // the same "nowhere in particular" terrain a random encounter
                // stages on. Without this the driver exits with map == -1 and
                // the player just gets a dead worldmap.
                map = 0;
            }
            break;
        } else if (intent.kind == WM_INTENT_ESCAPE) {
            // The cancel itself is performed by the map == -1 tail below, which every
            // OTHER way of leaving without a destination reaches too. Nothing to do here.
            break;
        }

        if (map != -1) {
            break;
        }
    }

    gWorldmapServerDriverActive = false;

    // ►►►► A SESSION THAT LANDS NOWHERE MUST BE UNDONE — EVERY WAY OUT, NOT JUST ESC.
    // Vanilla's wmWorldMapFunc has no KEY_ESCAPE branch at all: its worldmap has exactly
    // ONE exit, enter a location, so a mapLoadById ALWAYS follows and there is no such
    // thing as a trip that ends nowhere. Ours has several, and they all arrive here with
    // map == -1: the player's `wmesc`, and EVERY pump bail — no clients left, no
    // claimant, combat started, terminal quit. mapHandleTransition (map.cc) clears
    // gMapTransition regardless of what this function returns, so the session simply ends
    // on top of the map the party walked out of. That map is intact and script-alive
    // (the destructive teardown lives in the map != -1 branch below), which is why the
    // server looks perfectly happy — but NOTHING WAS UNDONE, and two pieces of state
    // were left describing a trip that never finished:
    //
    //   * WORLD POSITION. Travel moved worldPosX/Y. End the session there and the party
    //     stands in the map they left while the worldmap says they are somewhere else —
    //     leave Vault City, travel toward NCR, stop, and you are inside Vault City with
    //     your marker on NCR, from where the next exit grid drops you. Owner-reported:
    //     "I'm still in the previous random encounter and the server is kinda ok with it".
    //   * isInCar. The ONLY thing that sets it is METARULE_GIVE_CAR_TO_PARTY
    //     (wmCarGiveToParty), which transitions straight into this driver, and the ONLY
    //     thing that clears it is arriving somewhere (WM_INTENT_ENTER). So a true value
    //     here always belongs to the session being cancelled, and leaving it set puts the
    //     party "in the car" while standing on foot on a map.
    //
    // ►► WHY THIS IS IN THE TAIL AND NOT IN THE ESCAPE BRANCH, which is where it was
    // first written: the bail paths are the SAME bug through a different door, and the
    // one that matters most is the one nobody presses. The last client crashing during a
    // trip bails the pump ("no clients") and lands here — so the state a player comes
    // back to after a crash was exactly the state ESC used to leave behind. Gating on
    // the outcome (map == -1) instead of on the intent covers all of them by construction
    // and cannot be missed by a future fifth exit.
    //
    // What deliberately does NOT rewind: game time, fog of war, and travel healing — the
    // party really did walk out and back, and those are the cost of it. Script timed
    // events swallowed during the trip (scriptsDisable makes scriptExecProc early-return,
    // and scriptEventProcess has already dequeued them) are NOT a new divergence: vanilla
    // drops them identically, via the _scr_remove_all its own worldmap entry performs.
    if (map == -1) {
        const int fromPosX = wmGenData.worldPosX;
        const int fromPosY = wmGenData.worldPosY;

        wmGenData.isWalking = false;
        wmGenData.worldPosX = entryPosX;
        wmGenData.worldPosY = entryPosY;
        wmGenData.walkDestinationX = entryPosX;
        wmGenData.walkDestinationY = entryPosY;
        wmGenData.walkDistance = 0;
        wmGenData.isInCar = false;
        wmMatchWorldPosToArea(wmGenData.worldPosX, wmGenData.worldPosY, &(wmGenData.currentAreaId));

        // ►► SHIP THE REWIND BEFORE THE SCREEN CLOSES. The viewer renders the worldmap
        // out of its OWN wmGenData mirror and opens on it — the entry emitState exists
        // for exactly that reason. Skip this and the mirror keeps the travelled-to
        // position and the travelling cross, so the NEXT worldmap session opens on a
        // stale marker and its first click is aimed from the wrong place (the
        // no-re-derivation-path failure). Harmless on the bail paths that have no viewer
        // left to hear it.
        emitState();

        fprintf(stderr, "[wmsrv] trip cancelled: pos %d,%d -> %d,%d area=%d inCar->0\n",
            fromPosX, fromPosY, entryPosX, entryPosY, wmGenData.currentAreaId);
    }

    // ►► A SESSION CONSUMES ONLY ITS OWN INTENTS. The queue is process-global and was
    // cleared exactly once, at serverInstall (server_loop.cc) — so anything queued but
    // not popped before this loop broke was inherited by the NEXT worldmap session and
    // acted on there. Two ways to leave one behind, and the cancel above is one of
    // them: leave with a click still queued and the cancelled trip's move is the first
    // thing the next session does, walking the party somewhere nobody asked for and
    // undoing the rewind above. The other predates this and is plain double-clicking —
    // the driver pops one intent per iteration and an ENTER breaks out immediately, so
    // a second click made during the same session outlived it.
    worldmapIntentClear();

    presenter()->worldmapEnd();

    // One line per worldmap session — this path has no headless oracle and its
    // failure mode (map == -1) is invisible on the wire: the viewer just gets a
    // worldmapEnd and no rebaseline. `areaId=-1 map=-1` on an ENTER is the tell
    // that area resolution failed.
    fprintf(stderr, "[wmsrv] driver exit: areaId=%d map=%d\n",
        wmGenData.currentAreaId, map);
    if (map != -1) {
        // ►► The teardown deferred from entry, in the SAME order it ran there, so a
        // trip that really does land somewhere behaves exactly as before. Scripts go
        // back ON first because _map_save_in_game(true) runs scriptsExecMapExitProc()
        // and map_exit_p_proc must still get its turn — at entry nothing had disabled
        // them yet, and scriptExecProc() would silently skip it otherwise.
        wmTransitionResumeScripts();
        wmTransitionSaveMap();
        wmTransitionSuspendScripts();

        mapLoadById(map);
        // ►► NOW that emissions are live again, say what this place is. The line is
        // built by wmSetupRandomEncounter INSIDE the load, where the network presenter
        // drops every console message, so co-op players used to arrive in a random
        // encounter with no word about what they had walked into — single player says
        // "You have encountered: some radscorpions fighting some cannibals" and we said
        // nothing. No-op for an ordinary city arrival.
        wmEncounterDescriptionFlush();
        fprintf(stderr, "[wmsrv] entered map=%d elev=%d dudeTile=%d gen=%d\n",
            mapGetCurrentMap(), gElevation,
            gDude != nullptr ? gDude->tile : -1, mapGetLoadGeneration());
    }

    // ►► CLEAR THE STAGING **AFTER** THE LOAD, NEVER BEFORE — the entire encounter
    // (every critter in it, and the "You have encountered…" line that says what it
    // is) is built by wmSetupRandomEncounter, which mapLoad calls from INSIDE
    // mapLoadById above (map.cc) and which opens with
    //
    //     if (wmGenData.encounterMapId == -1) { return 0; }
    //
    // Clearing first therefore did not cancel the encounter — the map still loaded,
    // because `map` had already been copied out — it silently emptied it. You dropped
    // onto the right terrain with nobody on it and no word about why, every single
    // time. Only script-driven SPECIAL encounters still populated, because their own
    // map script does the placing, which is what made this look intermittent.
    //
    // Vanilla's order is the same as this one and that is not a coincidence: it clears
    // in wmInterfaceExit, which wmWorldMapFunc runs AFTER its mapLoadById (worldmap_ui.cc).
    // The clear moved above the load when the driver was extracted from that loop.
    wmEncounterStagingClear();

    wmTransitionResumeScripts();

    return (map != -1) ? 0 : -1;
}

} // namespace fallout