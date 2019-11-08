

#include "buddy.h"
#include "macros.h"
#include <stdio.h>

/***********************utility functions****************************/

/*returns k where 2^k is the first power of 2 greater or equal to n*/
int getDeg2(int n)
{
	int deg2 = 0;
	int i = 1;
	while (i < n)
	{
		i *= 2;
		deg2++;
	}
	return deg2;
}

void printBlockList(Block*first)
{
	if (!first)
	{
		printf("empty\n");
		return;
	}
	while (first)
	{
		printf("%x -> ", first);
		first = first->nextBlock;
	}
	printf("\n");
}

void printBuddy(BMS*buddy)
{
	int i = 0;
	for (i = 0; i <= buddy->largest_deg; i++)
	{
		printf("headers[%d]: ", i);
		printBlockList(buddy->headers[i].first);
	}
}


/***************end of utility functions*****************************/


/*request n consecutive blocks from buddy, thread safe*/
Block* buddyTakeSafe(int n, BMS*buddy)
{
	buddy->guard->lock();
	//printf("\n");
	/*buddy subsystem can only return a memory segment that is a power of 2 times size of a block, so transform n to
	the first power of 2 that is greater or equal to it*/
	int pow2 = 1;
	int deg2 = 0;
	while (pow2 < n)
	{
		pow2 *= 2;
		deg2++;
	}
	n = pow2;

	/*if the requested number of blocks is greater than the current number of free blocks in the buddy subsystem, ERROR*/
	if (n>buddy->free_num)
	{
		//printf("Not enough memory in the buddy subsystem, %d blocks requested, only %d blocks free\n", n, buddy->free_num);
		buddy->guard->unlock();
		return 0;
	}

	/*go through headers and check if there is a block big enough*/
	int i;
	for (i = deg2; i <= buddy->largest_deg; i++)
	{
		if (buddy->headers[i].first)
		{
			break;
		}
	}//after this loop, i stores the index in headers where to take an element from the list or largest_deg+1 if not found

	/*if not found a suitable piece of memory, ERROR*/
	if (i == buddy->largest_deg + 1)
	{
		//printf("Not enough consecutive blocks in the buddy subsystem");
		buddy->guard->unlock();
		return 0;
	}
	else//found a suitable piece of memory
	{
		//printf("Found a suitable piece of memory!\n");

		/*take the suitable blok b from buddy*/
		Block*b = buddy->headers[i].first;
		buddy->headers[i].first = b->nextBlock;
		if (buddy->headers[i].last == b)
			buddy->headers[i].last = 0;

		/*if the block is larger than needed, split it into smaller blocks*/
		int split_count = i - deg2;
		//printf("Splits needed: %d\n", split_count);

		int displacement = 1 << deg2;
		int j;
		for (j = 0; j < split_count; j++)
		{


			/*get address of the next piece of b to be returned to buddy*/
			Block*b2 = b + displacement;
			//printf("Split: %d Displacement: %d b2: %x\n", j, displacement, b2);

			/*return b2 to the appropriate list*/
			b2->nextBlock = buddy->headers[deg2 + j].first;
			buddy->headers[deg2 + j].first = b2;

			/*increase the displacement*/
			displacement *= 2;
		}

		buddy->free_num -= n;
		buddy->guard->unlock();
		return b;
	}

}

/*give n blocks starting from addr to buddy*/
void buddyGive(Block*addr, int n, BMS* buddy)
{
	//printf("\nCurrent buddy status: \n");
	//printBuddy(buddy);
	//printf("Total free blocks: %d\n", buddy->free_num);

	/*calculate offset from the first block of the buddy system*/
	unsigned offset = (unsigned)addr - (unsigned)(buddy->first);
	//printf("offset: %x ", offset);

	/*calculate the bitmask for the difference bit between the returned block and his buddy*/
	unsigned BITHOLDER = BIT12;
	int index = getDeg2(n);
	//printf("index: %d ", index);
	BITHOLDER <<= index;
	//after this piece of code, the BITHOLDER holds the difference bit
	//printf("BITHOLDER: %x\n", BITHOLDER);

	/*now calculate the parity of the returned block*/
	int parity = offset&BITHOLDER;

	/*calculate the buddy for the returned block*/
	unsigned target;
	if (parity)
	{
		target = offset - BITHOLDER;
	}
	else
	{
		target = offset + BITHOLDER;
	}
	target = target + (unsigned)(buddy->first);
	//printf("Blok: %x, Size(in blocks): %d, Difference bit: %x, Target: %x\n", addr, n, BITHOLDER, target);


	/*search for buddy of the returned block, if found merge them, if not insert the returned block into headers[index]*/
	int found = 0;
	Block*prev = 0;
	Block*first = buddy->headers[index].first;
	Block*cur = first;

	/*if headers[index] is empty, the buddy of the returned block is not found for sure*/
	if (!cur)
	{
		//printf("Buddy not found, inserting into headers[%d]\n", index);
		buddy->headers[index].first = addr;
		buddy->headers[index].last = addr;

		/*insert addr into headers[index]*/
		buddy->free_num += n;
		addr->nextBlock = 0;//the next block in the list doesn't exist

	}

	/*if headers[index] is not empty, go through the list and compare elements to target*/
	else
	{
		while (cur)
		{
			if ((unsigned)cur == target)//found
			{
				found = 1;
				//printf("Found the target ( %x )!\n", target);
				if (prev)// found, not on first element
				{
					//printf("Found not on first element\n");

					/*remove target from headers[index]*/
					buddy->free_num -= n;//we are removing the target
					prev->nextBlock = cur->nextBlock;

					/*update headers[index] if needed*/
					if (cur == buddy->headers[index].last)
					{
						buddy->headers[index].last = prev;
					}

				}
				else // found on the first element
				{
					//printf("Found on first element\n");

					/*remove target from headers[index]*/
					buddy->free_num -= n;//we are removing the target

					/*update headers[index].first*/
					buddy->headers[index].first = cur->nextBlock;

					/*update headers[index].last if needed*/
					if (cur == buddy->headers[index].last)
					{
						buddy->headers[index].last = prev;
					}
				}
				/*give buddy a two times bigger block using buddyGive()*/
				Block*inserted;
				if (parity)
					inserted = (Block*)target;
				else
					inserted = addr;
				buddyGive(inserted, 2 * n, buddy);
				break;//target is found, so exit the loop
			}
			prev = cur;
			cur = cur->nextBlock;
		}
		if (!found)
		{
			//printf("The target was not found, inserting %x into buddy->headers[%d]\n", addr, index);
			addr->nextBlock = buddy->headers[index].first;
			buddy->headers[index].first = addr;
		}

	}

}

/*thread safe buddyGive*/
void buddyGiveSafe(Block*addr, int n, BMS* buddy)
{
	buddy->guard->lock();
	buddyGive(addr, n, buddy);
	buddy->guard->unlock();
}

//initializes buddy allocator for the piece of memory of blockNum blocks, starting from addr
//returns the real starting address that the buddy allocator uses
BMS* buddyInit(void*addr, int blockNum)
{
	int blocks = blockNum;

	/*check if the address addr is a starting address of a block*/
	unsigned newaddress = GET_BLOCK(addr);
	if (newaddress != (unsigned)addr)
	{

		newaddress = newaddress + BIT12;//next block
		blocks = blockNum - 1;//first and last block are not used if the allocated memory was not aligned, so we throw away 1 block in total
		//printf("Alligning to the first block: %x\n", newaddress);
	}
	void*p = (void*)newaddress;
	/*the first block of the memory is not given to buddy, it is used for the BMS*/
	BMS* myBMS = (BMS*)p;
	blocks--;
	/*calculate the address of the first block given to buddy*/
	Block*first = (Block*)p + 1;


	/*evaluating the number of blocks for buddy and the number of taken blocks*/
	int deg2 = 0;
	int pow2 = 1;
	while (pow2 < blocks)
	{
		pow2 *= 2;
		deg2++;
	}//after this while, up2 is the first power of 2 greater or equal to blocks
	int residue = pow2 - blocks;
	//printf("Buddy will work with %d blocks \nResidue blocks: %d\n", pow2, residue);

	/*initialize buddy management structure*/
	myBMS->blocks = pow2;
	myBMS->first = first;
	myBMS->residue = residue;
	//myBMS->free_num = blocks;
	myBMS->free_num = 0; // this is initialized on zero because buddyGive() is used later to add blocks to the system
	myBMS->largest_deg = deg2;
	myBMS->guard = new((void*)myBMS->mutex_space) std::mutex();
	int i = 0;
	for (i = 0; i <= deg2; i++)
	{
		myBMS->headers[i].first = myBMS->headers[i].last = 0;
	}
	//printf("Size of the BMS: %dB\n", sizeof(BMS));

	/*make the headers' lists*/
	for (i = 0; i < blocks; i++)
	{
		buddyGive(first + i, 1, myBMS);
	}

	return myBMS;
}








