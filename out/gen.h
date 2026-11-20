#pragma once
#include <stdint.h>
struct BlockEntry { unsigned char bank; uint16_t addr; void (*fn)(void); };
extern const BlockEntry kEntries[];
extern const unsigned kEntryCount;
extern const uint16_t kResetVector, kNmiVector, kIrqVector;
