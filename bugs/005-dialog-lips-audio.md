# 005 — Dialog lipsync audio + animation for the viewer

**Status**: SPEC (2026-07-19)
**Blocks**: NPCs with voiced dialogue (talking-head audio + lip sync) are silent on viewer

## Current state

- `lipsLoad` FATAL crash fixed with `serverLoopActive()` guard in `gameDialogStartLips`
  — but the guard just SKIPS lipsync entirely. Feature is not implemented.
- The dialog engine runs server-side and has access to audio filenames from
  `gsay_say` opcodes (scripts.cc:2763-2765), but they're never sent to the viewer.
- `EVENT_DIALOG_NODE` sends: speakerNetId, driverNetId, reaction, reply text,
  options. No audio filename.
- The viewer's `_gdProcessUpdate` / `_gdProcess` path (which normally calls
  `gameDialogStartLips` with the audio filename) is bypassed entirely.

## What needs to happen

### Step 1: Add optional audio filename to EVENT_DIALOG_NODE

In `presenter.h`, the `dialogNode` virtual method:

```cpp
virtual void dialogNode(Object* speaker, Object* driver, int reaction,
    const char* reply, const char* const* options, int optionCount) {}
```

Add an optional audio parameter:

```cpp
virtual void dialogNode(Object* speaker, Object* driver, int reaction,
    const char* reply, const char* const* options, int optionCount,
    const char* audioFileName = nullptr) {}
```

In `presenter_network.cc`, emit it:
```cpp
putString(audioFileName != nullptr ? audioFileName : ""); // u16 length 0 = no audio
```

In `client_net.cc` `onDialogNode`, decode it after options:
```cpp
std::string audioFileName = r.str(); // may be empty
```

Pass to `clientDialogOnNode`.

### Step 2: Server-side: catpure audio filename from `gsay_say`

In `game_dialog.cc`, `dialogEmitNode()` currently emits the node at the top of
the server barrier. The audio filename is set by `gsay_say` opcode which calls
`gameDialogSetMessageReply(...)` with an audio-embedded `MessageListItem`.

The audio filename lives in `MessageListItem.audio` (a char array). When
`gsay_say` runs, the script calls `_scr_get_msg_str_speech` which sets up
the reply AND optional audio. On the server, `gameDialogStartLips` is guarded
out, so the audio is lost.

Need: store the audio filename in a server-accessible static (or pass it
through from the message-list resolution). In `_gdProcessUpdate`, after the
reply message resolution, capture the audio filename from the message list
item's `.audio` field. Store in a static `gDialogPendingAudio[16]`.

Then in `dialogEmitNode`, pass it:
```cpp
presenter()->dialogNode(gGameDialogSpeaker, gDude, GAME_DIALOG_REACTION_NEUTRAL,
    gDialogReplyText, optionPtrs, count,
    gDialogPendingAudio[0] ? gDialogPendingAudio : nullptr);
```

### Step 3: Viewer-side: trigger lipsync on receipt

In `client_dialog.h`, add `audioFileName` parameter:
```cpp
void clientDialogOnNode(int speakerNetId, int driverNetId, int reaction,
    const char* reply, const char* const* options, int optionCount,
    const char* audioFileName = nullptr);
```

In `client_dialog.cc` `clientDialogOnNode`, after seeding options and calling
`gameDialogRenderNode()`, if `audioFileName` is non-empty, call
`gameDialogStartLips(audioFileName)`. This triggers the vanilla lipsync
pipeline on the viewer:

```
gameDialogStartLips → lipsLoad(audio, headFrm) → lipsStart()
  → gameDialogTicker → lipsTicker() → updates talking-head mouth frames
  → gameDialogEndLips → lipsFree() on next node/end
```

`gameDialogEndLips` is already called from `_gdialogExitFromScript` →
`clientDialogOnEnd` path. And from `gameDialogBarterButtonUpMouseUp` which
is called when barter is entered. So lipsync ends when the dialog window
closes or barter is entered.

### Step 4: Wire event for non-audio nodes

When there's NO audio (reply text only, no voice acting), send empty string
(`""`, wire-encoded as u16 length 0). The viewer skips lipsync.

Backward-compatible: old client decodes the audio string but `clientDialogOnNode`
ignores it (default nullptr). Old server sends empty string. New server+client
pair gets the feature.

## Non-trivial considerations

1. **Lipsync timing vs dialog node timing**: Vanilla plays audio then pauses
   at the end of `gameDialogStartLips` (it doesn't block — the ticker drives
   lipsync frames asynchronously). The viewer's dialog loop continues running:
   `inputGetInput` returns keys while audio plays. The user can advance to the
   next node before audio finishes — `gameDialogEndLips` stops it cleanly.
   This matches vanilla behavior: you can skip voiced lines.

2. **HeadFid needed for lipsync**: `gameDialogStartLips` needs the head FID
   to load the mouth-frame art (`lipsLoad(audio, headFrm)` — where
   `headFrm = artCopyFileName(OBJ_TYPE_HEAD, gGameDialogHeadFid & 0xFFF, ...)`).
   The viewer already derives `headFid` from the speaker's proto in
   `clientDialogOnNode`, and `gGameDialogHeadFid` is set by `_gdialogInitFromScript`.
   So the head FID is available.

3. **Audio file path**: `lipsLoad` expects an audio filename like `"dcXXXXXX"`.
   The server's message-list `.audio` field already stores this. The viewer's
   `lipsLoad` loads it from the FO2 data files. Works the same as vanilla.

4. **`soundPlayFile("censor")` beep path**: When audio is `nullptr`,
   `gameDialogStartLips` plays the censor beep. This happens for NPCs whose
   lines have no voice acting. Currently this path is also server-guarded
   (new guard). Should only run on viewer.

## Files touched

- `src/presenter.h` — add `audioFileName` param to `dialogNode` virtual
- `src/presenter_network.cc` — wire encoding of audio filename
- `src/game_dialog.cc` — `dialogEmitNode()` capture audio from message-list
- `src/client_net.cc` — `onDialogNode` decode audio, pass to clientDialogOnNode
- `src/client_dialog.{h,cc}` — accept audio in `clientDialogOnNode`, call
  `gameDialogStartLips`
- `src/game_dialog.cc` — remove `serverLoopActive()` guard from `gameDialogStartLips`
  (after verifying it's only called from viewer path OR making it safe on server)

## Verification

1. Boot f2_server on a map with a voiced NPC (e.g., Den story-teller Leanne
   on denbus1 — check her message list for `.audio` fields)
2. Connect viewer, talk to the NPC
3. Confirm: talking head mouth animates in sync with audio (if any)
4. NPCs without voiced lines: no crash, no audio, text-only (current behavior)
