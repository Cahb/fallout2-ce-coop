#include "server_worldmap.h"

#include <cstdio>
#include <vector>

#include "game.h"
#include "map.h"
#include "presenter.h"
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
    presenter()->worldmapState(
        wmGenData.worldPosX, wmGenData.worldPosY,
        wmGenData.walkDestinationX, wmGenData.walkDestinationY,
        wmGenData.isWalking, wmGenData.walkDistance,
        wmGenData.carFuel, wmGenData.currentAreaId,
        wmGenData.isInCar);

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

    wmTransitionSaveMap();
    wmTransitionSuspendScripts();

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
                if (wmAreaFindFirstValidMap(&map) != -1) {
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
            break;
        }

        if (map != -1) {
            break;
        }
    }

    gWorldmapServerDriverActive = false;

    wmEncounterStagingClear();

    presenter()->worldmapEnd();

    // One line per worldmap session — this path has no headless oracle and its
    // failure mode (map == -1) is invisible on the wire: the viewer just gets a
    // worldmapEnd and no rebaseline. `areaId=-1 map=-1` on an ENTER is the tell
    // that area resolution failed.
    fprintf(stderr, "[wmsrv] driver exit: areaId=%d map=%d\n",
        wmGenData.currentAreaId, map);
    if (map != -1) {
        mapLoadById(map);
        fprintf(stderr, "[wmsrv] entered map=%d elev=%d dudeTile=%d gen=%d\n",
            mapGetCurrentMap(), gElevation,
            gDude != nullptr ? gDude->tile : -1, mapGetLoadGeneration());
    }

    wmTransitionResumeScripts();

    return (map != -1) ? 0 : -1;
}

} // namespace fallout