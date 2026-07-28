#ifndef XFILE_H
#define XFILE_H

#include <stdio.h>

#include <zlib.h>

#include "dfile.h"
#include "platform_compat.h"

namespace fallout {

typedef enum XFileType {
    XFILE_TYPE_FILE,
    XFILE_TYPE_DFILE,
    XFILE_TYPE_GZFILE,
    // Read-only, RAM-backed stream (no disk). Used for the co-op join/rebaseline
    // blob so several viewers on one machine never share a scratch file. Portable
    // (plain buffer + cursor), unlike fmemopen which is POSIX-only.
    XFILE_TYPE_MEMORY,
} XFileType;

// A universal database of files.
typedef struct XBase {
    // The path to directory or .DAT file that this xbase represents.
    char* path;

    // The [DBase] instance that this xbase represents.
    DBase* dbase;

    // A flag used to denote that this xbase represents .DAT file (true), or
    // a directory (false).
    //
    // NOTE: Original type is 1 byte, likely unsigned char.
    bool isDbase;

    // Next [XBase] in linked list.
    struct XBase* next;
} XBase;

typedef struct XFile {
    XFileType type;
    union {
        FILE* file;
        DFile* dfile;
        gzFile gzfile;
    };
    // XFILE_TYPE_MEMORY only: an owned buffer plus a cursor. Readable always;
    // WRITABLE when opened via xfileOpenMemoryWrite, in which case the buffer grows on
    // demand and memoryCapacity tracks the allocation (memorySize is the logical length,
    // i.e. the high-water mark of what has been written).
    unsigned char* memoryBuffer;
    long memorySize;
    long memoryPosition;
    long memoryCapacity;
    bool memoryWritable;
} XFile;

typedef struct XList {
    int fileNamesLength;
    char** fileNames;
} XList;

int xfileClose(XFile* stream);
XFile* xfileOpen(const char* filename, const char* mode);
// Open a READ-ONLY stream over an in-memory copy of [data]. No disk, no path.
XFile* xfileOpenMemory(const void* data, size_t size);

// ►► Open a WRITABLE, growable RAM stream. The other half of XFILE_TYPE_MEMORY, and the
// reason it exists: several places serialized a structure by writing it to a scratch file
// under /tmp and immediately reading it back, purely because there was no in-memory File to
// write into (the co-op join blob, the per-actor sheet delta — each said so in a comment).
// That left litter behind on every run and made two processes on one box fight over a path.
//
// Write with the ordinary fileWrite/filePrintFormatted calls, then read the bytes straight
// out with xfileMemoryData/xfileMemorySize — no read-back pass, no path, nothing to clean up.
XFile* xfileOpenMemoryWrite();

// The bytes written so far (nullptr if this is not a memory stream). Valid until the next
// write or xfileClose.
const unsigned char* xfileMemoryData(XFile* stream);

// The logical length of a memory stream — what has actually been written, not the
// allocation. 0 for any other stream type.
long xfileMemorySize(XFile* stream);
int xfilePrintFormatted(XFile* xfile, const char* format, ...);
int xfilePrintFormattedArgs(XFile* stream, const char* format, va_list args);
int xfileReadChar(XFile* stream);
char* xfileReadString(char* string, int size, XFile* stream);
int xfileWriteChar(int ch, XFile* stream);
int xfileWriteString(const char* s, XFile* stream);
size_t xfileRead(void* ptr, size_t size, size_t count, XFile* stream);
size_t xfileWrite(const void* buf, size_t size, size_t count, XFile* stream);
int xfileSeek(XFile* stream, long offset, int origin);
long xfileTell(XFile* stream);
void xfileRewind(XFile* stream);
int xfileEof(XFile* stream);
long xfileGetSize(XFile* stream);
bool xbaseReopenAll(char* paths);
bool xbaseOpen(const char* path);
bool xlistInit(const char* pattern, XList* xlist);
void xlistFree(XList* xlist);

} // namespace fallout

#endif /* XFILE_H */
