#ifndef FALLOUT_LOOT_HIGHLIGHT_H_
#define FALLOUT_LOOT_HIGHLIGHT_H_

namespace fallout {

// Polled once per main-loop frame (main.cc). Pressing the bind key (LALT)
// toggles the "highlight lootables" overlay, which draws a yellow outline on
// every corpse, container, and ground item on the current elevation. While the
// overlay is on the outline set is kept in sync with the world (new corpses get
// highlighted, looted items drop off) with a repaint only when it actually
// changes.
void lootHighlightUpdate();

// Turns the overlay off and clears every outline it owns. Call before a save or
// a map teardown so no highlight outline leaks into the object stream or the
// next map.
void lootHighlightClear();

} // namespace fallout

#endif /* FALLOUT_LOOT_HIGHLIGHT_H_ */
