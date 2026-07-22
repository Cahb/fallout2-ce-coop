#ifndef FALLOUT_INPUT_H_
#define FALLOUT_INPUT_H_

namespace fallout {

typedef void(TickerProc)();

typedef int(ScreenshotHandler)(int width, int height, unsigned char* buffer, unsigned char* palette);

int inputInit(int a1);
void inputExit();
int inputGetInput();
void get_input_position(int* x, int* y);
void _process_bk();
void enqueueInputEvent(int a1);
void inputEventQueueReset();
void tickersExecute();
void tickersAdd(TickerProc* fn);
void tickersRemove(TickerProc* fn);
void tickersEnable();
void tickersDisable();
// Ticker-list lifecycle (definitions in f2_core timing.cc); the client drives
// these from inputInit()/inputExit(), the server relies on static zero-init.
void tickersReset();
void tickersFree();
// Register the real wall-clock source (client binds this to SDL_GetTicks). When
// unset, getTicks() uses a core-only monotonic fallback. See timing.cc.
void clockProviderSet(unsigned int (*provider)());
void takeScreenshot();
int screenshotHandlerDefaultImpl(int width, int height, unsigned char* data, unsigned char* palette);
void screenshotHandlerConfigure(int keyCode, ScreenshotHandler* handler);
bool getTicksIsSynthetic();
unsigned int getTicks();
void inputPauseForTocks(unsigned int ms);
void inputBlockForTocks(unsigned int ms);
unsigned int getTicksSince(unsigned int a1);
unsigned int getTicksBetween(unsigned int a1, unsigned int a2);
unsigned int _get_bk_time();
int _GNW95_input_init();
void _GNW95_process_message();
void _GNW95_clear_time_stamps();
void _GNW95_lost_focus();

void beginTextInput();
void endTextInput();

} // namespace fallout

#endif /* FALLOUT_INPUT_H_ */
