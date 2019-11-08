
#ifndef BUDDY_H
#define BUDDY_H


#include "macros.h"
#include <mutex>

//a structural depiction of a buddy data block
typedef union BuddyDataBlock
{
	union BuddyDataBlock* nextBlock;
	unsigned char data[BLOCK_SIZE];
} Block;

typedef struct BlockListHeader
{
	Block* first;
	Block* last;
} Header;

//structure used to maintain buddy system data management
typedef struct BuddyManagementStructure
{

	Header headers[32]; // array with headers of lists of free blocks of the size 2^i where i is the index in the array
	Block* first;
	int blocks; // number of blocks in the buddy system
	int free_num; // number of free blocks in the system
	int residue; // number of blocks that are only virtually given to buddy, but actually are not allowed to be used
	int largest_deg;//the largest used index in headers array, corresponding to the largest available 
	unsigned char mutex_space[sizeof(std::mutex)];
	std::mutex * guard;

} BMS;


//initializes buddy allocator for the piece of memory of blockNum blocks, starting from addr
//returns the pointer to the structure that the buddy allocator uses for maintaining its data
BMS* buddyInit(void*addr, int blockNum);

/*get a pointer to 2^k free blocks, where 2^k = n, thread safe */
Block* buddyTakeSafe(int n, BMS*buddy);

/*
/*thread safe buddyTake
Block* buddyTakeSafe(int n, BMS*buddy);*/

//returns n blocks strating from addr to buddy allocator
void buddyGive(Block*addr, int n, BMS*buddy);

/*thread safe buddyGive*/
void buddyGiveSafe(Block*addr, int n, BMS*buddy);

void printBuddy(BMS*buddy);

#endif // BUDDY_H
