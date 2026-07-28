#ifndef DBOX_H
#define DBOX_H

namespace fallout {

typedef enum DialogBoxOptions {
    DIALOG_BOX_LARGE = 0x01,
    DIALOG_BOX_MEDIUM = 0x02,
    DIALOG_BOX_NO_HORIZONTAL_CENTERING = 0x04,
    DIALOG_BOX_NO_VERTICAL_CENTERING = 0x08,
    DIALOG_BOX_YES_NO = 0x10,
    DIALOG_BOX_0x20 = 0x20,
} DialogBoxOptions;

// TEMP DIAGNOSTIC [wmenc]: arm a one-shot trace on the NEXT showDialogBox, which
// prints every input code that box receives and which one closed it. The hunt it
// exists for is "the encounter prompt never appears, yet the server gets a decline
// within two pump spins" — a box that returns 0 that fast was answered by an input
// nobody typed, and the code alone names the source: 501 is the NO button (a mouse
// release inside its rect), KEY_ESCAPE (27) is a keystroke or an injected one from
// the viewer's own service ticker. Cleared by the box that consumes it, so an armed
// trace can never leak into an unrelated dialog. Remove with the rest of [wmenc].
void dialogBoxTraceNext(const char* tag);

// TEMP DIAGNOSTIC [wmenc]: non-null while a traced box is OPEN. The tracer above
// proved the closing input is a real KEY_ESCAPE arriving ~1 frame after the box opens
// — so the queue flush at open is not the answer and the ESC has a live producer.
// This lets every producer (the two in _process_bk, and each injection site in
// client_net.cc) name itself, but only inside the window where it matters.
const char* dialogBoxTraceActiveTag();

int showDialogBox(const char* title, const char** body, int bodyLength, int x, int y, int titleColor, const char* a8, int bodyColor, int flags);
int showLoadFileDialog(char* title, char** fileList, char* dest, int fileListLength, int x, int y, int flags);
int showSaveFileDialog(char* title, char** fileList, char* dest, int fileListLength, int x, int y, int flags);

} // namespace fallout

#endif /* DBOX_H */
