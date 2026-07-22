#ifndef RANDOM_H
#define RANDOM_H

#include "db.h"

namespace fallout {

typedef enum Roll {
    ROLL_CRITICAL_FAILURE,
    ROLL_FAILURE,
    ROLL_SUCCESS,
    ROLL_CRITICAL_SUCCESS,
} Roll;

void randomInit();
void randomReset();
void randomExit();
int randomSave(File* stream);
int randomLoad(File* stream);
int randomRoll(int difficulty, int criticalSuccessModifier, int* howMuchPtr);
int randomBetween(int min, int max);
void randomSeedPrerandom(int seed);
unsigned int randomStateFingerprint();

// Full generator state, captured/rewound as one unit. Used by the presentation
// record section (pres_record.cc): the server runs the animate branch it normally
// skips so the leaves can record it, and that branch consumes cosmetic RNG (gib
// pick, fire-dance fling). Snapshot before, restore after, so the AUTHORITATIVE
// stream position is unchanged — randomStateFingerprint() is byte-identical across
// a snapshot+restore, which is the purity oracle the goldens pin.
struct RandomState {
    int idum;
    int iy;
    int iv[32];
};
void randomSnapshot(RandomState* out);
void randomRestore(const RandomState* in);

} // namespace fallout

#endif /* RANDOM_H */
