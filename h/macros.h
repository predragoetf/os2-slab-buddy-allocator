
#ifndef MACROS_H
#define MACROS_H

#define CACHE_L1_LINE_SIZE (64)
#define BLOCK_SIZE (4096)
#define POINTER_SIZE sizeof(void*)
#define BLOCK_MASK 0xfffff000
#define GET_BLOCK(p) ((unsigned)p)&BLOCK_MASK
#define BIT12 0x00001000

#endif // MACROS



