
#include "slab.h"
#include <stdio.h>
#include "buddy.h"
//#include "slab_int.h"
#define _CRT_SECURE_NO_WARNINGS

#define MAX_NAME_LENGTH (20)

/*********Structures and other types***************/

enum HeaderStrategy { IN, OUT };
enum L1Coloring { NO, YES };
enum ErrorCode { NO_ERROR, GROWTH_FAILURE, DESTROY_NONEMPTY };

typedef struct HeaderOfTheSlabHeaderList
{
	void*first;
}HeaderS;

typedef struct kmem_cache_s
{

	/*name */
	char name[MAX_NAME_LENGTH];

	/*pointers to the next and previous caches in the list of caches*/
	struct kmem_cache_s * next;
	struct kmem_cache_s * prev;

	/*headers of slab lists*/
	HeaderS empty;
	HeaderS partial;
	HeaderS full;

	/*slab header placement strategy, created with default OUT, fixed for some types of caches*/
	HeaderStrategy strategy;

	/*first free byte in the slabs of this cache (before coloring), 0 if OUT strategy, sizeof(SlabHeader)+marker_size if IN*/
	unsigned first_free_byte_offset;

	/*size of the marker for this cache type*/
	unsigned marker_size;

	/*number of slots in a slab*/
	unsigned obj_per_slab;

	/*L1 coloring for the slabs of the cache*/
	L1Coloring coloring_on;

	/*if coloring_on is set to YES, stores the next offset*/
	unsigned next_coloring_offset;

	/*wasted space in the slabs of this cache*/
	unsigned waste;

	/*size of slabs for this cache (in blocks)*/
	unsigned slab_size;

	/*size of objects for this cache (in bytes)*/
	unsigned obj_size;

	/* constructor function */
	void(*ctor)(void *);

	/* destructor function */
	void(*dtor)(void *);

	/*indicator if the cache grew from the last shrinking*/
	unsigned growth_indicator;

	/*last logged error*/
	ErrorCode error;

	/*mutex*/
	unsigned char mutex_space[sizeof(std::mutex)];
	std::mutex * guard;


}kmem_cache_t;

/*structure used for storing the pointers to cahces of sizeN small memory buffers*/
typedef struct cache_sizes {
	size_t           size;
	kmem_cache_t    *sizeNcachep;
} cache_sizes_t;

typedef struct SlabManagementStructure
{
	/*pointer to the buddy management structure*/
	BMS*buddy;

	/*pointer to the slab lookup table*/
	Block*slab_lookup;

	/* cache of cache descriptors */
	kmem_cache_t cacheOfCaches;

	/* cache of slab headers for the sizeN buffers*/
	kmem_cache_t cacheOfSlabHeaders;

	/* cache of sizeN caches, needed because otherwise cache of caches allocation could lead to infinite recursion */
	//kmem_cache_t cacheOfSizeNCaches;

	/*synchronization for standard output*/
	std::mutex * print_guard;
	unsigned char mutex_space[sizeof(std::mutex)];

	/*synchronization for sizeN pointers*/
	std::mutex * sizeN_guard;
	unsigned char mutex_space2[sizeof(std::mutex)];

	/*array of pointers to the size-N caches*/
	cache_sizes_t sizeNpointer[14];


}SMS;

/*************************************************************/


/***************************Function declarations***************/

/*prints info about the chosen cache*/
//void kmem_cache_info(kmem_cache_t * cachep);

/*initializes the slab allocator for memory of block_num blocks starting on address given in space*/
//void kmem_init(void*space, int block_num);

/*creates a cache with the chosen params, ctor and dtor can be NULL*/
//kmem_cache_t *kmem_cache_create(const char *name, size_t size, void(*ctor)(void*), void(*dtor)(void*));

//void* kmem_cache_alloc(kmem_cache_t*cachep);

//void kmem_cache_free(kmem_cache_t *cachep, void *objp);

//int kmem_cache_shrink(kmem_cache_t *cachep);

kmem_cache_t *kmem_cache_create_sizeN(size_t size);

void* kmem_cache_alloc_sizeN(kmem_cache_t*sizeNCache);

void* kmem_cache_alloc_paramd(kmem_cache_t* cachep, unsigned is_sizeN);

//void *kmalloc(size_t size);

//void kfree(const void* objp);

/***************************************************************/

/****************************Slabs**************************/


typedef struct SlabHeader
{
	/*pointer to the next slab in the list*/
	struct SlabHeader*nextSlabHeader;

	/*pointer to the previous slab in the list*/
	struct SlabHeader*prevSlabHeader;

	/*pointer to the owner*/
	kmem_cache_t*cache;

	/*displacement of the first object in the slab because of the L1 cache colouring*/
	unsigned displacement;

	/*number of free objects in the slab that can be allocated*/
	unsigned free_slots;


	/*pointer to the first object in the slab that can be allocated*/
	//void * first_free_slot;

	/*ponter to the first taken slot*/
	//void * first_taken_slot;

	/*used only if the header is not in the slab*/
	void*slab_address;

	/*buffer used for tracking free slots*/
	unsigned* marker;

	/*first free slot in marker*/
	unsigned first_free;

}SlabHeader;


/*************************************************/


/*global variables*/
SMS* sms;
/******************/

/**********************utility functions*****************/

/*returns ideal slab size (in blocks), writes the amount of left over space in the slab to the location pointed to by throw_away
NOTE: use ONLY for the OUT strategy!*/
int getSlabSizeOUT(unsigned obj_size, unsigned*throw_away)
{
	/*first try with a single block, increase until a single object can fit into a slab*/
	unsigned slab_size = BLOCK_SIZE;
	unsigned deg2 = 0;
	while (slab_size < obj_size)
	{
		slab_size *= 2;
		deg2++;
	}
	unsigned best_size = slab_size;
	unsigned best_residuo = slab_size%obj_size;

	/*check if any of the next 2 slab sizes give better results*/
	unsigned MAXSIZE = slab_size << 3;
	while (slab_size<MAXSIZE)
	{
		slab_size *= 2;
		unsigned residuo = slab_size%obj_size;
		if (residuo < best_residuo)
		{
			best_residuo = residuo;
			best_size = slab_size;
		}
		
	}
	*throw_away = best_residuo;
	return best_size/BLOCK_SIZE;
}

void insertSlabToFront(SlabHeader* slab_header, HeaderS*list)
{
	if (list->first)
	{
		((SlabHeader*)(list->first))->prevSlabHeader = slab_header;
	}
	slab_header->nextSlabHeader = (SlabHeader*)list->first;
	slab_header->prevSlabHeader = 0;
	list->first = slab_header;

}

void removeSlabFromList(SlabHeader* slab_header, HeaderS*list)
{
	/*check if first element in list*/
	if (slab_header->prevSlabHeader)
		/*update prev's pointer*/
		slab_header->prevSlabHeader->nextSlabHeader = slab_header->nextSlabHeader;
	else
		/*update first element of list*/
		list->first = slab_header->nextSlabHeader;
	if (slab_header->nextSlabHeader)
		/*update next's pointer*/
		slab_header->nextSlabHeader->prevSlabHeader = slab_header->prevSlabHeader;
}

void insertCacheToFront(kmem_cache_t * cachep, kmem_cache_t * header)
{
	//header->guard->lock();
	if (header->next)
		header->next->prev = cachep;
	cachep->prev = header;
	cachep->next = header->next;
	header->next = cachep;
	//header->guard->unlock();
}

void removeCacheFromList(kmem_cache_t * cachep, kmem_cache_t * header)
{
	//header->guard->lock();
	cachep->prev->next = cachep->next;
	/*if not last in list*/
	if (cachep->next)
		/*update next's pointer*/
		cachep->next->prev = cachep->prev;
	//header->guard->unlock();
}

unsigned block_index(Block*block)
{
	unsigned block_offset = ((unsigned)block - (unsigned)(sms->buddy->first));
	unsigned block_ind = block_offset >> 12;
	return block_ind;
}

SlabHeader* getSlabHeader(void*objp)
{
	unsigned block = (unsigned)objp&BLOCK_MASK;
	unsigned*address = (unsigned*)((unsigned)(sms->slab_lookup) + sizeof(void*)*block_index((Block*)block));
	SlabHeader* sh = (SlabHeader*)(*address);
	return sh;
}

void setSlabHeader(Block*block, SlabHeader * sh)
{
	
	SlabHeader**address = (SlabHeader**)((unsigned)(sms->slab_lookup) + sizeof(void*)*block_index(block));
	*address = sh;
}

/*initialize cache of caches, called from kmem_init only*/
void createCacheOfCaches()
{

	strcpy_s(sms->cacheOfCaches.name, "cacheOfCaches");
	sms->cacheOfCaches.obj_size = sizeof(kmem_cache_t);
	sms->cacheOfCaches.slab_size = 1;
	sms->cacheOfCaches.empty = { 0 };
	sms->cacheOfCaches.partial = { 0 };
	sms->cacheOfCaches.full = { 0 };
	sms->cacheOfCaches.strategy = IN;
	sms->cacheOfCaches.coloring_on = NO;
	sms->cacheOfCaches.ctor = 0;
	sms->cacheOfCaches.dtor = 0;
	sms->cacheOfCaches.next = NULL;
	sms->cacheOfCaches.prev = NULL;
	sms->cacheOfCaches.growth_indicator = 0;
	sms->cacheOfCaches.error = NO_ERROR;
	sms->cacheOfCaches.guard = new(sms->cacheOfCaches.mutex_space) std::mutex();
	/*take a little bit more for the marker, doesn't hurt*/
	sms->cacheOfCaches.marker_size = (BLOCK_SIZE-sizeof(SlabHeader)) / sizeof(kmem_cache_t);
	sms->cacheOfCaches.obj_per_slab = (BLOCK_SIZE - sizeof(SlabHeader)-sizeof(unsigned)*sms->cacheOfCaches.marker_size)/ sizeof(kmem_cache_t);
	sms->cacheOfCaches.first_free_byte_offset = sizeof(SlabHeader)+sizeof(unsigned)*sms->cacheOfCaches.marker_size;
	sms->cacheOfCaches.waste = BLOCK_SIZE - sms->cacheOfCaches.first_free_byte_offset - sms->cacheOfCaches.obj_size*sms->cacheOfCaches.obj_per_slab;
}

/*initialize cache of slab headers for small memory buffers only, called from kmem_init only*/
void createCacheOfSlabHeaders()
{
	/*initialize cache of slab headers*/
	strcpy_s(sms->cacheOfSlabHeaders.name, "cacheOfSlabHeaders");
	sms->cacheOfSlabHeaders.obj_size = sizeof(SlabHeader)+(BLOCK_SIZE/32)*sizeof(unsigned);
	sms->cacheOfSlabHeaders.obj_per_slab = BLOCK_SIZE / sms->cacheOfSlabHeaders.obj_size;
	sms->cacheOfSlabHeaders.marker_size = sms->cacheOfSlabHeaders.obj_per_slab;
	sms->cacheOfSlabHeaders.first_free_byte_offset = sizeof(SlabHeader)+sms->cacheOfSlabHeaders.marker_size*sizeof(unsigned);
	sms->cacheOfSlabHeaders.slab_size = 1;//hardcoded magic
	sms->cacheOfSlabHeaders.empty = { 0 };
	sms->cacheOfSlabHeaders.partial = { 0 };
	sms->cacheOfSlabHeaders.full = { 0 };
	sms->cacheOfSlabHeaders.strategy = IN;
	sms->cacheOfSlabHeaders.coloring_on = NO;
	sms->cacheOfSlabHeaders.next_coloring_offset = 0;
	sms->cacheOfSlabHeaders.next = NULL;
	sms->cacheOfSlabHeaders.prev = NULL;
	sms->cacheOfSlabHeaders.ctor = 0;
	sms->cacheOfSlabHeaders.dtor = 0;
	sms->cacheOfSlabHeaders.growth_indicator = 0;
	sms->cacheOfSlabHeaders.error = NO_ERROR;
	sms->cacheOfSlabHeaders.guard = new(sms->cacheOfCaches.mutex_space) std::mutex();
	sms->cacheOfSlabHeaders.waste = (BLOCK_SIZE - sms->cacheOfSlabHeaders.first_free_byte_offset) % sms->cacheOfSlabHeaders.obj_size;
}

/*initialize cache of sizeN caches, called from kmem_init only*/
//
//void createCacheOfSizeNCaches()
//{
//	/*initialize cache of sizeN caches*/
//	strcpy(sms->cacheOfSizeNCaches.name, "cacheOfSizeNCaches");
//	sms->cacheOfSizeNCaches.obj_size = sizeof(kmem_cache_t);
//	sms->cacheOfSizeNCaches.slab_size = 1;
//	sms->cacheOfSizeNCaches.empty = { 0 };
//	sms->cacheOfSizeNCaches.partial = { 0 };
//	sms->cacheOfSizeNCaches.full = { 0 };
//	sms->cacheOfSizeNCaches.strategy = IN;
//	sms->cacheOfSizeNCaches.coloring_on = NO;
//	sms->cacheOfSizeNCaches.ctor = 0;
//	sms->cacheOfSizeNCaches.dtor = 0;
//	sms->cacheOfSizeNCaches.growth_indicator = 0;
//	sms->cacheOfSizeNCaches.error = NO_ERROR;
//	sms->cacheOfSizeNCaches.guard = new(sms->cacheOfSizeNCaches.mutex_space) std::mutex();
//}


/*****************end of utility functions***************/





void kmem_init(void * space, int block_num)
{
	/*initialize buddy allocator*/
	BMS*buddy = buddyInit(space, block_num);

	/*take a block from buddy for the slab allocator management structure*/
	sms = (SMS*)(buddy+1);
	sms->buddy = buddy;
	/*initialize the array of pointers to the caches of small memory buffers*/
	int shift = 0;
	size_t startval = 32;
	for (shift; shift < 13; shift++)
	{
		sms->sizeNpointer[shift].size = startval;
		sms->sizeNpointer[shift].sizeNcachep = NULL;
		startval <<= 1;
	}
	sms->sizeNpointer[13].size = 0;
	sms->sizeNpointer[13].sizeNcachep = NULL;

	/*initialize the slab lookup table*/
	unsigned lookup_size = buddy->blocks*sizeof(void*) / BLOCK_SIZE;
	if (buddy->blocks*sizeof(void*) % BLOCK_SIZE) lookup_size++;
	sms->slab_lookup = buddyTakeSafe(lookup_size, sms->buddy);

	/*initialize print guard mutex*/
	sms->print_guard = new((void*)(sms->mutex_space)) std::mutex();
	sms->sizeN_guard = new((void*)(sms->mutex_space2)) std::mutex();

	/*create cache of caches, cache of slab headers and cache of sizeN caches*/
	createCacheOfCaches();
	//createCacheOfSizeNCaches();
	createCacheOfSlabHeaders();
	


}

/*allocate one small memory buffer, if the cache for that size is still uninitialized, initialize it first*/
void *kmalloc(size_t size)
{
	/*get deg2 and pow2*/
	unsigned deg2 = 0;
	unsigned pow2 = 1;
	while (pow2 < size)
	{
		deg2++;
		pow2 *= 2;
	}
	if (pow2 < 32)
	{
		pow2 = 32;
		deg2 = 5;
	}

	sms->sizeN_guard->lock();
	kmem_cache_t * cachep = sms->sizeNpointer[deg2-5].sizeNcachep;
	/*if the cache for requested size is not allocated, allocate it!*/
	if (!cachep)
	{
		cachep = kmem_cache_create_sizeN(size);
		sms->sizeNpointer[deg2 - 5].sizeNcachep = cachep;
	}
	sms->sizeN_guard->unlock();

	return kmem_cache_alloc_sizeN(cachep);
}

kmem_cache_t *kmem_cache_create_sizeN(size_t size)
{
	/*convert size to power of 2, just to be sure*/
	unsigned deg2 = 0;
	unsigned pow2 = 1;
	while (pow2 < size)
	{
		deg2++;
		pow2 *= 2;
	}
	if (pow2 < 32)
	{
		pow2 = 32;
		deg2 = 5;
	}
	size = pow2;

	/*request a slot from the cache of caches*/
	kmem_cache_t *cachep = (kmem_cache_t*)kmem_cache_alloc_paramd(&(sms->cacheOfCaches),0);

	/*set cache name*/
	sprintf_s(cachep->name, "size%d", size);

	/*link the cache into the list of caches*/
	insertCacheToFront(cachep, &(sms->cacheOfCaches));

	/*constructor and destructor*/
	cachep->ctor = 0;
	cachep->dtor = 0;

	/*find slab size if the slab header is stored out of slab*/
	unsigned residuo;
	cachep->obj_size = size;
	unsigned slab_size = getSlabSizeOUT(size, &residuo);
	cachep->slab_size = slab_size;
	cachep->growth_indicator = 0;
	cachep->obj_per_slab = (slab_size*BLOCK_SIZE) / size;
	cachep->marker_size = cachep->obj_per_slab;

	/*pick strategy depending on residuo, if there's space for storing slab header + marker, choose IN, otherwise choose OUT*/
	if (residuo > sizeof(SlabHeader)+sizeof(unsigned)*cachep->obj_per_slab)
	{
		cachep->strategy = IN;
		cachep->first_free_byte_offset = sizeof(SlabHeader)+cachep->marker_size*sizeof(unsigned);
	}
	else
	{
		cachep->strategy = OUT;
		cachep->first_free_byte_offset = 0;
	}

	/*calculate wasted space (in bytes)*/
	cachep->waste = (cachep->slab_size*BLOCK_SIZE - cachep->first_free_byte_offset) - cachep->obj_per_slab*size;

	/*colouring not possible for */
	
	if (cachep->waste > CACHE_L1_LINE_SIZE)
	{
		cachep->coloring_on = YES;
		cachep->next_coloring_offset = 0;
	}
	else cachep->coloring_on = NO;

	/*set cache name and empty, partial and full list headers*/
	cachep->empty.first = NULL;
	cachep->partial.first = NULL;
	cachep->full.first = NULL;

	/*set error flag to NO_ERROR*/
	cachep->error = NO_ERROR;

	/*create mutex semaphore lock*/
	cachep->guard = new((void*)(cachep->mutex_space)) std::mutex();

	return cachep;

}



/*used for allocating a new slab for the cache of caches and normal caches if is_sizeN = 0, allocates a slab for sizeN otherwise*/
void getNewSlab(kmem_cache_t*cachep, unsigned is_sizeN)
{
	SlabHeader *slab_header;
	/*slabs for cache of caches are made out of one block each*/
	Block*b = buddyTakeSafe(cachep->slab_size, sms->buddy);
	if (!b)//ERROR, not enough space for a slab!
	{
		cachep->error = GROWTH_FAILURE;
		return;
	}
	if (cachep->strategy == IN)
	{
		/*place the slab header into cache of caches slab*/
		slab_header = (SlabHeader*)b;
	}
	else
	{
		/*find place for the slab header and marker in some small memory buffer*/
		void* space;
		if (is_sizeN)
			space = kmem_cache_alloc_paramd(&(sms->cacheOfSlabHeaders),1);
		else
			space = kmalloc(sizeof(SlabHeader)+cachep->marker_size*sizeof(unsigned));
		slab_header = (SlabHeader*)space;
	}
	/*update slab lookup table*/
	for (unsigned i = 0; i < cachep->slab_size; i++)
		setSlabHeader(b + i, slab_header);
	
	slab_header->cache = cachep;
	slab_header->slab_address = b;
	slab_header->marker = (unsigned*)(slab_header + 1);
	unsigned marker_size = cachep->marker_size;
	unsigned MARKER_BOUNDARY = 0xffffffff;
	/*initialize marker*/
	for (unsigned i = 0; i < marker_size; i++)
	{
		if (i < cachep->obj_per_slab)
			slab_header->marker[i] = i + 1;
		else
			slab_header->marker[i] = MARKER_BOUNDARY;
	}
	/*initialize free slot pointer*/
	slab_header->first_free = 0;
	slab_header->free_slots = cachep->obj_per_slab;
	/*if colouring, displace for proper amount of L1 cache lines*/
	slab_header->displacement = cachep->first_free_byte_offset;
	if (cachep->coloring_on == YES)
	{
		slab_header->displacement += cachep->next_coloring_offset;
		cachep->next_coloring_offset += CACHE_L1_LINE_SIZE;
		if (cachep->waste < cachep->next_coloring_offset) cachep->next_coloring_offset = 0;
	}
	/*call constructor for all slots*/
	if (cachep->ctor)
	{
		for (unsigned i = 0; i < cachep->obj_per_slab; i++)
		{
			/*get address of the i-th slot*/
			void*address = (void*)((unsigned)(slab_header->slab_address)+slab_header->displacement + i*cachep->obj_size);
			/*call ctor for the ith slot*/
			(cachep->ctor)(address);
		}
	}

	/*update the cachep->empty.first pointer*/
	insertSlabToFront(slab_header, &(cachep->empty));
	
}

/*allocate an object from cachep, used for normal caches, thread safe*/
void* kmem_cache_alloc(kmem_cache_t*cachep)
{
	cachep->guard->lock();
	void* ret = kmem_cache_alloc_paramd(cachep, 0);
	cachep->guard->unlock();
	return ret;
}

/*allocate an object from cachep, used for small memory buffer caches, thread safe*/
void* kmem_cache_alloc_sizeN(kmem_cache_t*sizeNCache)
{
	sizeNCache->guard->lock();
	void*ret = kmem_cache_alloc_paramd(sizeNCache, 1);
	sizeNCache->guard->unlock();
	return ret;
}

/*get a slot for allocation in cachep, NOT THREAD SAFE*/
void* kmem_cache_alloc_paramd(kmem_cache_t* cachep, unsigned is_sizeN)
{
	/*the header of a slab where to allocate*/
	SlabHeader*slab_header;
	//kmem_cache_t * cachep = &(sms->cacheOfCaches);
	/*if there are no slabs with free slots, get a new one*/
	if (!cachep->partial.first&&!cachep->empty.first)
	{
		cachep->growth_indicator = 1;
		getNewSlab(cachep, is_sizeN);
	}//now there is at least one slab with free slots!

	int empty=0;

	/*first check if there are any partially full slabs*/
	if (cachep->partial.first)
	{
		slab_header = (SlabHeader*)(cachep->partial.first);
		empty = 0;
	}
	else if (cachep->empty.first)
	{
		slab_header = (SlabHeader*)(cachep->empty.first);
		empty = 1;
	}
	else //ERROR
		exit(2);

	/*get the address of the first free object in the slab and take the slot*/
	void*address = (void*)((unsigned)slab_header->slab_address + slab_header->displacement + slab_header->first_free*cachep->obj_size);
	slab_header->first_free = slab_header->marker[slab_header->first_free];
	slab_header->free_slots--;

	/*check if object was taken from an empty slab, if so, remove the slab from the list of empty slabs, as it is no more empty*/
	if (empty)
	{
		removeSlabFromList(slab_header, &(cachep->empty));
	}

	/*check if the slab became partial/full*/
	if (slab_header->free_slots)//became partial
	{
		/*if the slab was empty, add it to the list of partial slabs*/
		if (empty) insertSlabToFront(slab_header, &(cachep->partial));
	}
	else//became full
	{
		if (!empty) removeSlabFromList(slab_header, &(cachep->partial));
		insertSlabToFront(slab_header, &(cachep->full));
	}

	return address;
}

/*function used for creating normal caches*/
kmem_cache_t *kmem_cache_create(const char *name, size_t size, void(*ctor)(void*), void(*dtor)(void*))
{
	
	if ((!ctor) && (dtor))
	{
		printf("Destruktor bez konstruktora, greska!\n");
		exit(3);
	}

	
	/*request a slot from the cache of caches*/
	kmem_cache_t *cachep = (kmem_cache_t*)kmem_cache_alloc(&(sms->cacheOfCaches));

	(sms->cacheOfCaches.guard)->lock();
	/*link the cache into the list of caches*/
	insertCacheToFront(cachep, &(sms->cacheOfCaches));

	/*find slab size if the slab header is stored out of slab*/
	unsigned residuo;
	unsigned slab_size = getSlabSizeOUT(size, &residuo);
	cachep->obj_size = size;
	cachep->slab_size = slab_size;
	cachep->growth_indicator = 0;
	cachep->obj_per_slab = (slab_size*BLOCK_SIZE) / size;
	cachep->marker_size = cachep->obj_per_slab;

	/*pick strategy depending on residuo, if there's space for storing slab header + marker, choose IN, otherwise choose OUT*/
	if (residuo > sizeof(SlabHeader)+sizeof(unsigned)*cachep->obj_per_slab)
	{
		cachep->strategy = IN;
		cachep->first_free_byte_offset = sizeof(SlabHeader)+cachep->marker_size*sizeof(unsigned);
	}
	else
	{
		cachep->strategy = OUT;
		cachep->first_free_byte_offset = 0;
	}

	/*calculate wasted space (in bytes)*/
	cachep->waste = (cachep->slab_size*BLOCK_SIZE-cachep->first_free_byte_offset)-cachep->obj_per_slab*size;

	/*check if colouring is possible*/
	if (cachep->waste > CACHE_L1_LINE_SIZE)
	{
		cachep->coloring_on = YES;
		cachep->next_coloring_offset = 0;
	}
	else
	{
		cachep->coloring_on = NO;
		cachep->next_coloring_offset = 0;
	}

	/*set cache name and empty, partial and full list headers*/
	strcpy_s(cachep->name, name);
	cachep->empty.first = NULL;
	cachep->partial.first = NULL;
	cachep->full.first = NULL;

	/*assign constructor and destructor, if any*/
	cachep->ctor = ctor;
	cachep->dtor = dtor;

	/*set error flag to NO_ERROR*/
	cachep->error = NO_ERROR;

	/*create mutex semaphore lock*/
	cachep->guard = new((void*)(cachep->mutex_space)) std::mutex();
	(sms->cacheOfCaches.guard)->unlock();
	return cachep;
}

/*frees an object pointed to by the objp pointer from the cache pointed to by cachep*/
void kmem_cache_free_unsafe(kmem_cache_t *cachep, void *objp)
{
	
	/*find the corresponding slab header*/
	SlabHeader*slab_header = getSlabHeader(objp);

	/*call destructor, then constructor for object (if any)*/
	if (cachep->dtor)
		(cachep->dtor)(objp);
	if(cachep->ctor)
		(cachep->ctor)(objp);


	/*get the index of the slot*/
	unsigned slot = ((unsigned)objp - (unsigned)(slab_header->slab_address) - slab_header->displacement)/cachep->obj_size;

	/*update first free slot index */
	slab_header->marker[slot] = slab_header->first_free;
	slab_header->first_free = slot;

	unsigned full = (slab_header->free_slots == 0);
	/*update the number of objects in the slab*/
	slab_header->free_slots++;

	/*check if the removal of the object implies a need for list update*/
	if (full)//the slab was full, move to partial
	{
		removeSlabFromList(slab_header, &(cachep->full));
	}
	if (slab_header->free_slots == cachep->obj_per_slab)//became empty
	{
		if (!full) removeSlabFromList(slab_header, &(cachep->partial));
		insertSlabToFront(slab_header, &(cachep->empty));
	}
	else//became partial
	{
		if (full)
			insertSlabToFront(slab_header, &(cachep->partial));
	}
	
	
}

void kmem_cache_free(kmem_cache_t*cachep, void*objp)
{
	cachep->guard->lock();
	kmem_cache_free_unsafe(cachep, objp);
	cachep->guard->unlock();
}

int kmem_cache_shrink_unsafe(kmem_cache_t *cachep)
{
	/*check if the cache grew from the last shrink attempt*/
	if (cachep->growth_indicator)
	{
		cachep->growth_indicator = 0;
		return 0;
	}
	else//the cache did not grow, so shrink it!
	{
		unsigned blocks_freed = 0;
		while (cachep->empty.first)
		{
			SlabHeader*slab_header = (SlabHeader*)(cachep->empty.first);
			cachep->empty.first = slab_header->nextSlabHeader;
			Block*taken = (Block*)slab_header->slab_address;
			/*if the cache keeps its slab descriptors in the cacheOfSlabHeaders, remove it from there*/
			if (cachep->strategy = OUT)
			{
				/*free the slab header from the cacheOfSlabHeaders*/
				kmem_cache_free_unsafe(cachep, slab_header);
			}
			buddyGiveSafe(taken, cachep->slab_size, sms->buddy);
			/*update slab lookup table*/
			for (unsigned i = 0; i < cachep->slab_size; i++)
				setSlabHeader(taken + i, 0);

			blocks_freed += cachep->slab_size;
		}
		return blocks_freed;
	}
}


int kmem_cache_shrink(kmem_cache_t * cachep)
{
	cachep->guard->lock();
	int ret = kmem_cache_shrink_unsafe(cachep);
	cachep->guard->unlock();
	return ret;
}

void kfree(const void* objp)
{
	/*find the corresponding slab header*/
	SlabHeader*slab_header = getSlabHeader((void*)objp);

	/*find the owner cache*/
	kmem_cache_t * cachep = slab_header->cache;
	

	cachep->guard->lock();
	/*get the index of the slot*/
	unsigned slot = ((unsigned)objp - slab_header->displacement) / cachep->obj_size;

	/*update first free slot index */
	slab_header->marker[slot] = slab_header->first_free;
	slab_header->first_free = slot;

	unsigned full = (slab_header->free_slots == 0);
	/*update the number of objects in the slab*/
	slab_header->free_slots++;

	/*check if the removal of the object implies a need for list update*/
	if (full)//the slab was full, move to partial
	{
		removeSlabFromList(slab_header, &(cachep->full));
	}
	if (slab_header->free_slots == cachep->obj_per_slab)//became empty
	{
		insertSlabToFront(slab_header, &(cachep->empty));
	}
	else//became partial
	{
		if (full)
			insertSlabToFront(slab_header, &(cachep->partial));
	}
	cachep->guard->unlock();

}

/*destroy function for normal caches*/
void kmem_cache_destroy(kmem_cache_t * cachep)
{
	cachep->guard->lock();
	if (cachep->partial.first || cachep->full.first)
	{
		cachep->error = DESTROY_NONEMPTY;
		return;
	}
	else
	{
		/*shrink cache*/
		kmem_cache_shrink_unsafe(cachep);
		
		/*remove cache from the list of caches*/
		removeCacheFromList(cachep, &(sms->cacheOfCaches));
	}
	cachep->guard->unlock();
}

/*utility function that prints information about the cache pointed to by cachep*/
void kmem_cache_info(kmem_cache_t * cachep)
{

	cachep->guard->lock();
	
	unsigned total_slabs = 0;
	unsigned free_slots = 0;
	SlabHeader* p = (SlabHeader*)(cachep->full.first);
	while (p)
	{
		total_slabs += 1;
		p = p->nextSlabHeader;
	}
	p = (SlabHeader*)(cachep->partial.first);
	while (p)
	{
		total_slabs += 1;
		free_slots += p->free_slots;
		p = p->nextSlabHeader;
	}
	p = (SlabHeader*)(cachep->empty.first);
	while (p)
	{
		total_slabs += 1;
		free_slots += p->free_slots;
		p = p->nextSlabHeader;
	}
	unsigned total_slots = total_slabs*cachep->obj_per_slab;
	unsigned taken_slots = total_slots - free_slots;

	sms->print_guard->lock();
	printf("\n");
	printf("Cache info:\n");
	printf("name: %s\n", cachep->name);
	printf("object size: %dB\n", cachep->obj_size);
	printf("slab size: %d blocks\n", cachep->slab_size);
	printf("number of objects per slab: %d\n", cachep->obj_per_slab);
	printf("total number of blocks in cache: %d\n", total_slabs*cachep->slab_size);
	if (total_slots)
		printf("slot usage: %d/%d = %.2f \n", taken_slots, total_slots, (double)(taken_slots) / total_slots);
	else
		printf("slot usage: 0/0 = UNDEF\n");

	printf("\n\n");
	sms->print_guard->unlock();
	
	cachep->guard->unlock();

	/*printf("slab header placement strategy: %s\n", (cachep->strategy ? "OUT" : "IN"));
	printf("slab coloring: %s\n", (cachep->coloring_on ? "YES" : "NO"));
	printf("growth indicator: %d\n", cachep->growth_indicator);
	printf("waste: %dB\n", cachep->waste);
	printf("\n");*/
}

/*utility function that returns the last error code*/
int kmem_cache_error(kmem_cache_t * cachep)
{
	cachep->guard->lock();
	int temp = (int)(cachep->error);
	cachep->guard->unlock();
	return temp;
}