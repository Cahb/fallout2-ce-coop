#include <string.h>

#include "db.h"
#include "game_movie.h"

namespace fallout {

// The movie SEEN ledger, split out of the client's game_movie.cc playback TU
// ([[p5-cut-list]]; the H4 art lesson applied in miniature: when sim state is
// welded into a presentation file, move the state, not the projector).
//
// Which movies the player has watched is SIM/SAVE state, not presentation:
// scripts branch on it (the gmovie_played opcode via scripts.cc), proto.cc and
// worldmap.cc gate the vault-suit content on it, and it round-trips through the
// savegame. So f2_core owns the array and its accessors; game_movie.cc keeps
// the SDL playback pipeline (windows/palette/subtitles/sound) that marks it.
//
// This is what lets the headless server answer gameMovieIsSeen() truthfully
// instead of the old blanket `return false` stub — that lie would have silently
// mis-branched any script asking whether a movie had played.

// 0x596C78
static unsigned char gGameMoviesSeen[MOVIE_COUNT];

// The server-side movie barrier (see game_movie.h for the release-policy ruling).
// Null pump = no barrier at all, which is the client, the golden probe, and any
// server with no viewers attached.
static std::function<bool()> gMovieServerPump;
static bool gMovieAcked = false;

void gameMovieSetServerPump(std::function<bool()> pump)
{
    gMovieServerPump = std::move(pump);
}

void gameMovieAck()
{
    gMovieAcked = true;
}

void gameMovieServerBarrier()
{
    if (gMovieServerPump == nullptr) {
        return;
    }

    // Clear HERE, not on release: an ack that arrives late (a second viewer
    // finishing after the first already released the room) must not pre-release the
    // NEXT movie. The window between two movies is the only safe place to reset.
    gMovieAcked = false;

    while (!gMovieAcked) {
        if (!gMovieServerPump()) {
            break; // bail: no viewers left, or quit — never wedge the tick
        }
    }
}

// The seen half of gameMoviesInit/gameMoviesReset. The client's playback-state
// reset (gGameMovieIsPlaying/gGameMovieFaded) stays with the pipeline and calls
// through to this.
void gameMoviesResetSeen()
{
    memset(gGameMoviesSeen, 0, sizeof(gGameMoviesSeen));
}

// Ledger H-30 (extracted from gameMoviePlay): the seen flag is savegame state
// queried by scripts (gameMovieIsSeen opcode) — marking it is sim authority,
// not a playback side effect.
void gameMovieMarkSeen(int movie)
{
    // Wire-driven on the viewer (onMoviePlay / the movie verb), so the index crosses
    // a trust boundary — a bad value is an OOB WRITE into gGameMoviesSeen. Vanilla
    // callers always pass a MOVIE_* constant, so SP/golden never trip this.
    if (movie < 0 || movie >= MOVIE_COUNT) {
        return;
    }
    gGameMoviesSeen[movie] = 1;
}

// 0x44EB04
bool gameMovieIsSeen(int movie)
{
    if (movie < 0 || movie >= MOVIE_COUNT) {
        return false;
    }
    return gGameMoviesSeen[movie] == 1;
}

const unsigned char* gameMoviesSeenData()
{
    return gGameMoviesSeen;
}

// 0x44E638
int gameMoviesLoad(File* stream)
{
    if (fileRead(gGameMoviesSeen, sizeof(*gGameMoviesSeen), MOVIE_COUNT, stream) != MOVIE_COUNT) {
        return -1;
    }

    return 0;
}

// 0x44E664
int gameMoviesSave(File* stream)
{
    if (fileWrite(gGameMoviesSeen, sizeof(*gGameMoviesSeen), MOVIE_COUNT, stream) != MOVIE_COUNT) {
        return -1;
    }

    return 0;
}

} // namespace fallout
