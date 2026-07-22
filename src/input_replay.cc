#include "input_replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "debug.h"
#include "kb.h"

namespace fallout {

enum class ReplayMode {
    kOff,
    kReplay,
    kRecord,
};

struct MouseTraceEvent {
    unsigned int iteration;
    int dx;
    int dy;
    int buttonMask;
};

struct KeyTraceEvent {
    unsigned int iteration;
    int scancode;
    int down;
};

static ReplayMode gMode = ReplayMode::kOff;
static bool gInitialized = false;
static unsigned int gIteration = 0;

static std::vector<MouseTraceEvent> gMouseEvents;
static std::vector<KeyTraceEvent> gKeyEvents;
static size_t gNextMouseEvent = 0;
static size_t gNextKeyEvent = 0;
static int gStickyButtonMask = 0;

static FILE* gRecordStream = nullptr;
static int gLastRecordedButtonMask = 0;

static void inputReplayInit()
{
    gInitialized = true;

    const char* replayPath = getenv("F2_INPUT_REPLAY");
    if (replayPath != nullptr) {
        FILE* stream = fopen(replayPath, "r");
        if (stream == nullptr) {
            debugPrint("input-replay: cannot open trace '%s'\n", replayPath);
            return;
        }

        char line[256];
        while (fgets(line, sizeof(line), stream) != nullptr) {
            if (line[0] == 'M') {
                MouseTraceEvent event;
                if (sscanf(line, "M %u %d %d %d", &event.iteration, &event.dx, &event.dy, &event.buttonMask) == 4) {
                    gMouseEvents.push_back(event);
                }
            } else if (line[0] == 'K') {
                KeyTraceEvent event;
                if (sscanf(line, "K %u %d %d", &event.iteration, &event.scancode, &event.down) == 3) {
                    gKeyEvents.push_back(event);
                }
            }
        }
        fclose(stream);

        gMode = ReplayMode::kReplay;
        debugPrint("input-replay: replaying '%s' (%zu mouse, %zu key events)\n",
            replayPath,
            gMouseEvents.size(),
            gKeyEvents.size());
        return;
    }

    const char* recordPath = getenv("F2_INPUT_RECORD");
    if (recordPath != nullptr) {
        gRecordStream = fopen(recordPath, "w");
        if (gRecordStream == nullptr) {
            debugPrint("input-replay: cannot open record file '%s'\n", recordPath);
            return;
        }

        gMode = ReplayMode::kRecord;
        debugPrint("input-replay: recording to '%s'\n", recordPath);
    }
}

void inputReplayPumpTick()
{
    if (!gInitialized) {
        inputReplayInit();
    }

    gIteration++;

    if (gMode != ReplayMode::kReplay) {
        return;
    }

    // Inject due keyboard events through the same path SDL key events take.
    while (gNextKeyEvent < gKeyEvents.size() && gKeyEvents[gNextKeyEvent].iteration <= gIteration) {
        const KeyTraceEvent& event = gKeyEvents[gNextKeyEvent];

        KeyboardData keyboardData;
        keyboardData.key = event.scancode;
        keyboardData.down = event.down != 0 ? 1 : 0;
        _kb_simulate_key(&keyboardData);

        gNextKeyEvent++;
    }
}

unsigned int inputReplayGetIteration()
{
    return gIteration;
}

bool inputReplayOverrideMouse(MouseData* data)
{
    if (!gInitialized) {
        inputReplayInit();
    }

    if (gMode != ReplayMode::kReplay) {
        return false;
    }

    data->x = 0;
    data->y = 0;
    data->wheelX = 0;
    data->wheelY = 0;

    while (gNextMouseEvent < gMouseEvents.size() && gMouseEvents[gNextMouseEvent].iteration <= gIteration) {
        const MouseTraceEvent& event = gMouseEvents[gNextMouseEvent];
        data->x += event.dx;
        data->y += event.dy;
        gStickyButtonMask = event.buttonMask;
        gNextMouseEvent++;
    }

    data->buttons[0] = (gStickyButtonMask & 0x1) != 0;
    data->buttons[1] = (gStickyButtonMask & 0x2) != 0;

    return true;
}

void inputReplayRecordMouse(const MouseData* data)
{
    if (gMode != ReplayMode::kRecord || gRecordStream == nullptr) {
        return;
    }

    int buttonMask = (data->buttons[0] != 0 ? 0x1 : 0) | (data->buttons[1] != 0 ? 0x2 : 0);
    if (data->x != 0 || data->y != 0 || buttonMask != gLastRecordedButtonMask) {
        fprintf(gRecordStream, "M %u %d %d %d\n", gIteration, data->x, data->y, buttonMask);
        fflush(gRecordStream);
        gLastRecordedButtonMask = buttonMask;
    }
}

void inputReplayRecordKey(int scancode, bool down)
{
    if (gMode != ReplayMode::kRecord || gRecordStream == nullptr) {
        return;
    }

    fprintf(gRecordStream, "K %u %d %d\n", gIteration, scancode, down ? 1 : 0);
    fflush(gRecordStream);
}

} // namespace fallout
