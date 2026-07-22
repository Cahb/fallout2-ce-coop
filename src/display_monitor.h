#ifndef DISPLAY_MONITOR_H
#define DISPLAY_MONITOR_H

namespace fallout {

int displayMonitorInit();
int displayMonitorReset();
void displayMonitorExit();
void displayMonitorAddMessage(char* string);

// As above, but the line is painted in the style this channel maps to
// (msg_channel.h). An out-of-range channel falls back to kMsgChannelDefault, so a
// mod inventing a channel this viewer does not know still gets a readable line
// rather than garbage. displayMonitorAddMessage is exactly this with
// kMsgChannelDefault.
void displayMonitorAddMessageStyled(char* string, int channel);
void displayMonitorDisable();
void displayMonitorEnable();

} // namespace fallout

#endif /* DISPLAY_MONITOR_H */
