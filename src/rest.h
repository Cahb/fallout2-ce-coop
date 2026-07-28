#ifndef REST_H
#define REST_H

namespace fallout {

// Why a rest stopped. The caller needs the distinction: the pipboy screen bails out
// of ITSELF only when a queued event fired (vanilla's _proc_bail_flag), not when the
// player pressed escape.
enum RestOutcome {
    kRestCompleted = 0, // ran the full duration (or nobody needed healing)
    kRestAborted, // the frame proc said stop — escape, or a quit request
    kRestInterrupted, // a queued game event fired and wants the screen gone
};

// The PRESENTATION half of one rest frame, called once per simulated frame. Draw the
// clock, poll input, pace the frame; return true to stop the rest. The server passes
// nullptr and the loop runs at simulation speed.
typedef bool RestFrameProc();

// Rest for hours:minutes (duration 0), or until healed / until the party is healed
// (PIPBOY_REST_DURATION_UNTIL_*, party_member.h). Advances the game clock, accrues
// the party's healing — every player actor, not just the host (owner ruling) — and
// lets due queued events fire.
//
// ►► THIS IS A SIMULATION, so only the authority may call it: the game clock and
// everyone's hit points move. A co-op client asks with the `rest` verb instead
// (server_control.cc) and the pipboy's own leaf refuses a viewer outright.
RestOutcome restPerform(int hours, int minutes, int duration, RestFrameProc* onFrame);

} // namespace fallout

#endif /* REST_H */
