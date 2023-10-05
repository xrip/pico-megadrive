#include <stdint.h>
#include <stdbool.h>
#include "gwenesis/savestate/gwenesis_savestate.h"

static char *headerString = "Gene0000";
static char saveBuffer[256];

#define WORK_BLOCK_SIZE (256)
static int flashBlockOffset = 0;
static bool isLastFlashWrite = 0;

// This function fills 4kB blocks and writes them in flash when full
static void SaveGwenesisFlashSaveData(unsigned char *dest, unsigned char *src, int size) {

}

struct SaveStateSection {
    int tag;
    int offset;
};

// We have 31*8 Bytes available for sections info
// Do not increase this value without reserving
// another 256 bytes block for header
#define MAX_SECTIONS 31

struct SaveState {
    struct SaveStateSection sections[MAX_SECTIONS];
    uint16_t section;
    int allocSize;
    int size;
    int offset;
    unsigned char *buffer;
    unsigned char  fileName[64];
};


/* Savestate functions */
int saveGwenesisState(unsigned char *destBuffer, int save_size) {

}

void saveGwenesisStateSet(SaveState* state, const char* tagName, int value)
{

}

void saveGwenesisStateSetBuffer(SaveState* state, const char* tagName, void* buffer, int length)
{

}

SaveState* saveGwenesisStateOpenForWrite(const char* fileName)
{

}

/* Loadstate functions */
bool initLoadGwenesisState(unsigned char *srcBuffer) {

}

int loadGwenesisState(unsigned char *srcBuffer) {

}

SaveState* saveGwenesisStateOpenForRead(const char* fileName)
{

}

int saveGwenesisStateGet(SaveState* state, const char* tagName)
{

}

void saveGwenesisStateGetBuffer(SaveState* state, const char* tagName, void* buffer, int length)
{

}