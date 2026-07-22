#ifndef FALLOUT_MSG_CHANNEL_H_
#define FALLOUT_MSG_CHANNEL_H_

namespace fallout {

// What KIND of message-log line this is — never what colour it is.
//
// The server names the channel; the CLIENT owns the palette (display_monitor.cc
// gMessageChannelStyles). That split is the whole point: presentation stays out
// of the simulation, a re-skinned viewer restyles everything without touching the
// server, and a mod can add a channel without the wire learning about fonts.
//
// Values are WIRE VALUES — append only, never renumber. kDefault must stay 0: it
// is the value an absent trailing channel field decodes to, which is what keeps
// every pre-existing message byte-identical on the wire (see
// presenter_network.cc consoleMessageStyled).
enum MessageChannel {
    // Vanilla behaviour. Everything the engine emitted before channels existed.
    kMsgChannelDefault = 0,

    // World narration during a fight — damage, deaths, who hit what. Shared: this
    // is most of what makes a co-op fight readable.
    kMsgChannelCombat = 1,

    // "You don't have enough action points." Nothing happened, and it is aimed at
    // one player (presenter.h consoleMessageFor).
    kMsgChannelRefusal = 2,

    // The server talking as itself: greetings, a player joining or leaving, admin
    // announcements. Not part of the game world.
    kMsgChannelSystem = 3,

    // Player-to-player chat.
    kMsgChannelChat = 4,

    // Something good happened to you — XP, a level, a quest closing.
    kMsgChannelReward = 5,

    kMsgChannelCount,
};

} // namespace fallout

#endif /* FALLOUT_MSG_CHANNEL_H_ */
