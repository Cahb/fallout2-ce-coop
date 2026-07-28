#ifndef SOUND_EFFECTS_CACHE_H
#define SOUND_EFFECTS_CACHE_H

namespace fallout {

// The maximum number of sound effects that can be loaded and played
// simultaneously.
//
// ►► RAISED FROM VANILLA'S 4 (audio_engine.cc has the long version). Effect number
// five does not queue, degrade or steal a slot — soundEffectLoad refuses it and
// returns nullptr, and the caller plays nothing. Vanilla's combat could not stack
// four cues; a co-op turn boundary with a dozen hostiles stacks a dozen, and the
// shot that lands on the far side of that burst was going unheard. Bounded by the
// mixer pool (AUDIO_ENGINE_SOUND_BUFFERS), which must stay comfortably larger:
// music, the ambient bed and speech draw from the same slots.
#define SOUND_EFFECTS_MAX_COUNT (16)

int soundEffectsCacheInit(int cache_size, const char* effectsPath);
void soundEffectsCacheExit();
int soundEffectsCacheInitialized();
void soundEffectsCacheFlush();
int soundEffectsCacheFileOpen(const char* fname, int* sampleRate);
int soundEffectsCacheFileClose(int handle);
int soundEffectsCacheFileRead(int handle, void* buf, unsigned int size);
int soundEffectsCacheFileWrite(int handle, const void* buf, unsigned int size);
long soundEffectsCacheFileSeek(int handle, long offset, int origin);
long soundEffectsCacheFileTell(int handle);
long soundEffectsCacheFileLength(int handle);

} // namespace fallout

#endif /* SOUND_EFFECTS_CACHE_H */
