#include <levos/kernel.h>
#include <levos/palloc.h>
#include <levos/bitmap.h>
#include <levos/spinlock.h>

/**  Durand's Amazing Super Duper Memory functions.  */

#define VERSION 	"1.1"
#define DEFAULT_ALIGNMENT	16ul//4ul				///< This is the byte alignment that memory must be allocated on. IMPORTANT for GTK and other stuff.

#define ALIGN_TYPE		uint32_t ///unsigned char[16] /// unsigned short
#define ALIGN_INFO		sizeof(ALIGN_TYPE)*16	///< Alignment information is stored right before the pointer. This is the number of bytes of information stored there.


#define USE_CASE1
#define USE_CASE2
#define USE_CASE3
#define USE_CASE4
#define USE_CASE5

//#define LIBALLOC_DEBUG

#ifdef LIBALLOC_DEBUG
# define FLUSH(x) ;
# define printf(...) printk(__VA_ARGS__)
#endif

//#define CONFIG_HEAP_DEBUG 1

#define PREFIX(x) x

static elem_type heap_bitmap_bits[512];
static struct bitmap heap_bitmap;
static spinlock_t malloc_lock __align(4);

static struct liballoc_major *l_memRoot = NULL;	///< The root memory block acquired from the system.
static struct liballoc_major *l_bestBet = NULL; ///< The major with the most free memory.

static unsigned int l_pageSize  = 4096;			///< The size of an individual page. Set up in liballoc_init.
static unsigned int l_pageCount = 16;			///< The number of pages to request per chunk. Set up in liballoc_init.
static unsigned long long l_allocated = 0;		///< Running total of allocated memory.
static unsigned long long l_inuse	 = 0;		///< Running total of used memory.

static long long l_warningCount = 0;		///< Number of warnings encountered
static long long l_errorCount = 0;			///< Number of actual errors
static long long l_possibleOverruns = 0;	///< Number of possible overruns

static int __iterator = 0;

void
heap_init()
{
    heap_bitmap.bit_cnt = 2048;
    heap_bitmap.bits = heap_bitmap_bits;
    spin_lock_init(&malloc_lock);
    l_pageSize = 4096;
    l_pageCount = 16;
    __iterator = 0;
    printk("heap: initialized with %d bytes of bitmap\n", sizeof(heap_bitmap_bits));
}

size_t
heap_proc_heapstats(int pos, void *buf, size_t len, char *__arg)
{
    printk("Used heap memory: %d bytes\n", l_inuse);
    return 0;
}

/** This function is supposed to lock the memory data structures. It
 * could be as simple as disabling interrupts or acquiring a spinlock.
 * It's up to you to decide. 
 *
 * \return 0 if the lock was acquired successfully. Anything else is
 * failure.
 */
int liballoc_lock() {
	spin_lock(&malloc_lock);
    return 0;
}

/** This function unlocks what was previously locked by the liballoc_lock
 * function.  If it disabled interrupts, it enables interrupts. If it
 * had acquiried a spinlock, it releases the spinlock. etc.
 *
 * \return 0 if the lock was successfully released.
 */
int liballoc_unlock() {
	spin_unlock(&malloc_lock);
    return 0;
}


/** This is the hook into the local system which allocates pages. It
 * accepts an integer parameter which is the number of pages
 * required.  The page size was set up in the liballoc_init function.
 *
 * \return NULL if the pages were not allocated.
 * \return A pointer to the allocated memory.
 */
extern void* liballoc_alloc(size_t pages) {
    int pg = bitmap_scan_and_flip(&heap_bitmap, 0, pages, 0);
    if (pg == BITMAP_ERROR)
        return NULL;
    void *addr = (void *) ((int) VIRT_BASE + 4 * 1024 * 1024 + pg * 4096);
    return addr;
}

/** This frees previously allocated memory. The void* parameter passed
 * to the function is the exact same value returned from a previous
 * liballoc_alloc call.
 *
 * The integer value is the number of pages to free.
 *
 * \return 0 if the memory was successfully freed.
 */
int liballoc_free(void *ptr, size_t pages) {
    int pg = ((int)ptr - VIRT_BASE - 4 * 1024 * 1024) / 4096;
    bitmap_set_multiple(&heap_bitmap, pg, pages, 0);
    return 0;
}

/** This macro will conveniently align our pointer upwards */
#define ALIGN(align, ptr )													\
		if ( align > 1 )											\
		{																\
			uintptr_t diff;												\
			ptr = (void*)((uintptr_t)ptr + ALIGN_INFO);					\
			diff = (uintptr_t)ptr & (align-1);						\
			if ( diff != 0 )											\
			{															\
				diff = align - diff;								\
				ptr = (void*)((uintptr_t)ptr + diff);					\
			}															\
			*((ALIGN_TYPE*)((uintptr_t)ptr - ALIGN_INFO)) = 			\
				diff + ALIGN_INFO;										\
		}															


#define UNALIGN(align, ptr )													\
		if ( align > 1 )											\
		{																\
			uintptr_t diff = *((ALIGN_TYPE*)((uintptr_t)ptr - ALIGN_INFO));	\
			if ( diff < (align + ALIGN_INFO) )						\
			{															\
				ptr = (void*)((uintptr_t)ptr - diff);					\
			}															\
		}
				


#define LIBALLOC_MAGIC	0xc001c0de
#define LIBALLOC_DEAD	0xdeaddead

/** A structure found at the top of all system allocated 
 * memory blocks. It details the usage of the memory block.
 */
struct liballoc_major
{
	struct liballoc_major *prev;		///< Linked list information.
	struct liballoc_major *next;		///< Linked list information.
	unsigned int pages;					///< The number of pages in the block.
	unsigned int size;					///< The number of pages in the block.
	unsigned int usage;					///< The number of bytes used in the block.
	struct liballoc_minor *first;		///< A pointer to the first allocated memory in the block.	
};


/** This is a structure found at the beginning of all
 * sections in a major block which were allocated by a
 * malloc, calloc, realloc call.
 */
struct	liballoc_minor
{
	struct liballoc_minor *prev;		///< Linked list information.
	struct liballoc_minor *next;		///< Linked list information.
	struct liballoc_major *block;		///< The owning block. A pointer to the major structure.
	unsigned int magic;					///< A magic number to idenfity correctness.
	unsigned int size; 					///< The size of the memory allocated. Could be 1 byte or more.
	unsigned int req_size;				///< The size of memory requested.
    unsigned int iterator;
};





// ***********   HELPER FUNCTIONS  *******************************

static void *liballoc_memset(void* s, int c, size_t n)
{
	unsigned int i;
	for ( i = 0; i < n ; i++)
		((char*)s)[i] = c;
	
	return s;
}
static void* liballoc_memcpy(void* s1, const void* s2, size_t n)
{
  char *cdest;
  char *csrc;
  unsigned int *ldest = (unsigned int*)s1;
  unsigned int *lsrc  = (unsigned int*)s2;

  while ( n >= sizeof(unsigned int) )
  {
      *ldest++ = *lsrc++;
	  n -= sizeof(unsigned int);
  }

  cdest = (char*)ldest;
  csrc  = (char*)lsrc;
  
  while ( n > 0 )
  {
      *cdest++ = *csrc++;
	  n -= 1;
  }
  
  return s1;
}
 

#if defined LIBALLOC_DEBUG || defined INFO
static void liballoc_dump()
{
#ifdef LIBALLOC_DEBUG
	struct liballoc_major *maj = l_memRoot;
	struct liballoc_minor *min = NULL;
#endif

	printf( "liballoc: ------ Memory data ---------------\n");
	printf( "liballoc: System memory allocated: %d bytes\n", l_allocated );
	printf( "liballoc: Memory in used (malloc'ed): %d bytes\n", l_inuse );
	printf( "liballoc: Warning count: %d\n", l_warningCount );
	printf( "liballoc: Error count: %d\n", l_errorCount );
	printf( "liballoc: Possible overruns: %d\n", l_possibleOverruns );

#ifdef LIBALLOC_DEBUG
		while ( maj != NULL )
		{
			printf( "liballoc: %x: total = %d, used = %d\n",
						maj, 
						maj->size,
						maj->usage );

			min = maj->first;
			while ( min != NULL )
			{
				printf( "liballoc:    %x: %d bytes\n",
							min, 
							min->size );
				min = min->next;
			}

			maj = maj->next;
		}
#endif

	FLUSH();
}
#endif



// ***************************************************************

static struct liballoc_major *allocate_new_page( unsigned int size )
{
	unsigned int st;
	struct liballoc_major *maj;

		// This is how much space is required.
		st  = size + sizeof(struct liballoc_major);
		st += sizeof(struct liballoc_minor);

				// Perfect amount of space?
		if ( (st % l_pageSize) == 0 )
			st  = st / (l_pageSize);
		else
			st  = st / (l_pageSize) + 1;
							// No, add the buffer. 

		
		// Make sure it's >= the minimum size.
		if ( st < l_pageCount ) st = l_pageCount;
		
		maj = (struct liballoc_major*)liballoc_alloc( st );

		if ( maj == NULL ) 
		{
			l_warningCount += 1;
			#if defined LIBALLOC_DEBUG || defined INFO
			printf( "liballoc: WARNING: liballoc_alloc( %d ) return NULL\n", st );
			FLUSH();
			#endif
			return NULL;	// uh oh, we ran out of memory.
		}
		
		maj->prev 	= NULL;
		maj->next 	= NULL;
		maj->pages 	= st;
		maj->size 	= st * l_pageSize;
		maj->usage 	= sizeof(struct liballoc_major);
		maj->first 	= NULL;

		l_allocated += maj->size;

		#ifdef LIBALLOC_DEBUG
		printf( "liballoc: Resource allocated %x of %d pages (%d bytes) for %d size.\n", maj, st, maj->size, size );

		printf( "liballoc: Total memory usage = %d KB\n",  (int)((l_allocated / (1024))) );
		FLUSH();
		#endif
	
		
      return maj;
}


int
__new_iterator(size_t req_size, size_t align)
{
    int it = __iterator ++;
    //printk("##\tmalloc:\tsize:\t0x%x\talign:%d\tid:%d\n", req_size, align, it);
#ifdef CONFIG_HEAP_DEBUG
    printk("malloc,%d\n", it);
#endif

    //if (it == 998)
        //dump_stack(8);

    //if (it == 553 ||
            //it == 656 || it ==  657)
        //dump_stack(8);
    return it;
}

void *PREFIX(__na_malloc)(size_t req_size, size_t align);
void *PREFIX(na_malloc)(size_t req_size, size_t align)
{
    void *ret = __na_malloc(req_size, align);
    if (ret == NULL)
        panic("OUT OF MEMORY due to request of %d bytes!\n", req_size);
    //printk("%s: ret 0x%x -> 0x%x\n", __func__, ret, ret + req_size);
    return ret;
}
void *PREFIX(__na_malloc)(size_t req_size, size_t align)
{
	int startedBet = 0;
	unsigned long long bestSize = 0;
	void *p = NULL;
	uintptr_t diff;
	struct liballoc_major *maj;
	struct liballoc_minor *min;
	struct liballoc_minor *new_min;
	unsigned long size = req_size;

    //printk("%s(size:%d, align: %d)", __func__, req_size, 16);
    //if (req_size == 96)
        //dump_stack(8);

	// For alignment, we adjust size so there's enough space to align.
	if ( align > 1 )
	{
		size += align + ALIGN_INFO;
	}
				// So, ideally, we really want an alignment of 0 or 1 in order
				// to save space.
	
	liballoc_lock();

	if ( size == 0 )
	{
		l_warningCount += 1;
		printk("liballoc: WARNING: alloc( 0 ) called\n");
		liballoc_unlock();
		return PREFIX(malloc)(1);
	}
	

	if ( l_memRoot == NULL )
	{
		#if defined LIBALLOC_DEBUG || defined INFO
		#ifdef LIBALLOC_DEBUG
		printf( "liballoc: initialization of liballoc " VERSION "\n" );
		#endif
		//atexit( liballoc_dump );
		FLUSH();
		#endif
			
		// This is the first time we are being used.
		l_memRoot = allocate_new_page( size );
		if ( l_memRoot == NULL )
		{
		  liballoc_unlock();
		  #ifdef LIBALLOC_DEBUG
		  printf( "liballoc: initial l_memRoot initialization failed\n", p); 
		  FLUSH();
		  #endif
		  return NULL;
		}

		#ifdef LIBALLOC_DEBUG
		printf( "liballoc: set up first memory major %x\n", l_memRoot );
		FLUSH();
		#endif
	}


	#ifdef LIBALLOC_DEBUG
	printf( "liballoc: %x PREFIX(malloc)( %d ): ", 
					__builtin_return_address(0),
					size );
	FLUSH();
	#endif

	// Now we need to bounce through every major and find enough space....

	maj = l_memRoot;
	startedBet = 0;
	
	// Start at the best bet....
	if ( l_bestBet != NULL )
	{
		bestSize = l_bestBet->size - l_bestBet->usage;

		if ( bestSize > (size + sizeof(struct liballoc_minor)))
		{
			maj = l_bestBet;
			startedBet = 1;
		}
	}
	
	while ( maj != NULL )
	{
		diff  = maj->size - maj->usage;	
										// free memory in the block

		if ( bestSize < diff )
		{
			// Hmm.. this one has more memory then our bestBet. Remember!
			l_bestBet = maj;
			bestSize = diff;
		}
		
		
#ifdef USE_CASE1
			
		// CASE 1:  There is not enough space in this major block.
		if ( diff < (size + sizeof( struct liballoc_minor )) )
		{
			#ifdef LIBALLOC_DEBUG
			printf( "CASE 1: Insufficient space in block %x\n", maj);
			FLUSH();
			#endif
				
				// Another major block next to this one?
			if ( maj->next != NULL ) 
			{
				maj = maj->next;		// Hop to that one.
				continue;
			}

			if ( startedBet == 1 )		// If we started at the best bet,
			{							// let's start all over again.
				maj = l_memRoot;
				startedBet = 0;
				continue;
			}

			// Create a new major block next to this one and...
			maj->next = allocate_new_page( size );	// next one will be okay.
			if ( maj->next == NULL ) break;			// no more memory.
			maj->next->prev = maj;
			maj = maj->next;

			// .. fall through to CASE 2 ..
		}

#endif

#ifdef USE_CASE2
		
		// CASE 2: It's a brand new block.
		if ( maj->first == NULL )
		{
			maj->first = (struct liballoc_minor*)((uintptr_t)maj + sizeof(struct liballoc_major) );

			
			maj->first->magic 		= LIBALLOC_MAGIC;
            maj->first->iterator    = __new_iterator(req_size, align);
			maj->first->prev 		= NULL;
			maj->first->next 		= NULL;
			maj->first->block 		= maj;
			maj->first->size 		= size;
			maj->first->req_size 	= req_size;
			maj->usage 	+= size + sizeof( struct liballoc_minor );


			l_inuse += size;
			
			
			p = (void*)((uintptr_t)(maj->first) + sizeof( struct liballoc_minor ));

			ALIGN( align, p );
			
			#ifdef LIBALLOC_DEBUG
			printf( "CASE 2: returning %x\n", p); 
			FLUSH();
			#endif
			memset(p, 0, req_size);
			liballoc_unlock();		// release the lock
			return p;
		}

#endif
				
#ifdef USE_CASE3

		// CASE 3: Block in use and enough space at the start of the block.
		diff =  (uintptr_t)(maj->first);
		diff -= (uintptr_t)maj;
		diff -= sizeof(struct liballoc_major);

		if ( diff >= (size + sizeof(struct liballoc_minor)) )
		{
			// Yes, space in front. Squeeze in.
			maj->first->prev = (struct liballoc_minor*)((uintptr_t)maj + sizeof(struct liballoc_major) );
			maj->first->prev->next = maj->first;
			maj->first = maj->first->prev;
				
			maj->first->magic 	= LIBALLOC_MAGIC;
            maj->first->iterator = __new_iterator(req_size, align);
            //printk("##\tmalloc:\tsize:\t0x%x\talign:%d\tid:%d\n", req_size, align, maj->first->iterator);
			maj->first->prev 	= NULL;
			maj->first->block 	= maj;
			maj->first->size 	= size;
			maj->first->req_size 	= req_size;
			maj->usage 			+= size + sizeof( struct liballoc_minor );

			l_inuse += size;

			p = (void*)((uintptr_t)(maj->first) + sizeof( struct liballoc_minor ));
			ALIGN(align,  p );

			#ifdef LIBALLOC_DEBUG
			printf( "CASE 3: returning %x\n", p); 
			FLUSH();
			#endif
			memset(p, 0, req_size);
			liballoc_unlock();		// release the lock
			return p;
		}
		
#endif


#ifdef USE_CASE4

		// CASE 4: There is enough space in this block. But is it contiguous?
		min = maj->first;
		
			// Looping within the block now...
		while ( min != NULL )
		{
				// CASE 4.1: End of minors in a block. Space from last and end?
				if ( min->next == NULL )
				{
					// the rest of this block is free...  is it big enough?
					diff = (uintptr_t)(maj) + maj->size;
					diff -= (uintptr_t)min;
					diff -= sizeof( struct liballoc_minor );
					diff -= min->size; 
						// minus already existing usage..

					if ( diff >= (size + sizeof( struct liballoc_minor )) )
					{
						// yay....
						min->next = (struct liballoc_minor*)((uintptr_t)min + sizeof( struct liballoc_minor ) + min->size);
						min->next->prev = min;
						min = min->next;
						min->next = NULL;
						min->magic = LIBALLOC_MAGIC;
                        min->iterator = __new_iterator(req_size, align);
                        //printk("##\tmalloc:\tsize:\t0x%x\talign:%d\tid:%d\n", req_size, align, min->iterator);
						min->block = maj;
						min->size = size;
						min->req_size = req_size;
						maj->usage += size + sizeof( struct liballoc_minor );

						l_inuse += size;
						
						p = (void*)((uintptr_t)min + sizeof( struct liballoc_minor ));
						ALIGN(align, p );

						#ifdef LIBALLOC_DEBUG
						printf( "CASE 4.1: returning %x of size %d bytes and align %d\n",
                                    p, size, align); 
						FLUSH();
						#endif
						liballoc_unlock();		// release the lock
						return p;
					}
				}



				// CASE 4.2: Is there space between two minors?
				if ( min->next != NULL )
				{
					// is the difference between here and next big enough?
					diff  = (uintptr_t)(min->next);
					diff -= (uintptr_t)min;
					diff -= sizeof( struct liballoc_minor );
					diff -= min->size;
										// minus our existing usage.

					if ( diff >= (size + sizeof( struct liballoc_minor )) )
					{
						// yay......
						new_min = (struct liballoc_minor*)((uintptr_t)min + sizeof( struct liballoc_minor ) + min->size);

						new_min->magic = LIBALLOC_MAGIC;
						new_min->next = min->next;
                        new_min->iterator = __new_iterator(req_size, align);
                        //printk("##\tmalloc:\tsize:\t0x%x\talign:%d\tid:%d\n", req_size, align, new_min->iterator);
						new_min->prev = min;
						new_min->size = size;
						new_min->req_size = req_size;
						new_min->block = maj;
						min->next->prev = new_min;
						min->next = new_min;
						maj->usage += size + sizeof( struct liballoc_minor );
						
						l_inuse += size;
						
						p = (void*)((uintptr_t)new_min + sizeof( struct liballoc_minor ));
						ALIGN(align,  p );


						#ifdef LIBALLOC_DEBUG
						printf( "CASE 4.2: returning %x\n", p); 
						FLUSH();
						#endif
						
						liballoc_unlock();		// release the lock
						memset(p, 0, req_size);
						return p;
					}
				}	// min->next != NULL

				min = min->next;
		} // while min != NULL ...


#endif

#ifdef USE_CASE5

		// CASE 5: Block full! Ensure next block and loop.
		if ( maj->next == NULL ) 
		{
			#ifdef LIBALLOC_DEBUG
			printf( "CASE 5: block full\n");
			FLUSH();
			#endif

			if ( startedBet == 1 )
			{
				maj = l_memRoot;
				startedBet = 0;
				continue;
			}
				
			// we've run out. we need more...
			maj->next = allocate_new_page( size );		// next one guaranteed to be okay
			if ( maj->next == NULL ) break;			//  uh oh,  no more memory.....
			maj->next->prev = maj;

		}

#endif

		maj = maj->next;
	} // while (maj != NULL)


	
	liballoc_unlock();		// release the lock

	#ifdef LIBALLOC_DEBUG
	printf( "All cases exhausted. No memory available.\n");
	FLUSH();
	#endif
	#if defined LIBALLOC_DEBUG || defined INFO
	printf( "liballoc: WARNING: PREFIX(malloc)( %d ) returning NULL.\n", size);
	liballoc_dump();
	FLUSH();
	#endif
	return NULL;
}

void *PREFIX(malloc)(size_t req)
{
    void *ret = na_malloc(req, DEFAULT_ALIGNMENT);
    //printk("%s(%d): caller 0x%x\n", __func__, req, __builtin_return_address(0));
    return ret;
}

void *PREFIX(pa_malloc)(size_t req)
{
    return na_malloc(req, 4096);
}






void PREFIX(na_free)(size_t align, void *ptr)
{
	struct liballoc_minor *min;
	struct liballoc_major *maj;
    void *orig_ptr = ptr;


	if ( ptr == NULL ) 
	{
		l_warningCount += 1;
		#if defined LIBALLOC_DEBUG || defined INFO
		printf( "liballoc: WARNING: PREFIX(free)( NULL ) called from %x\n",
							__builtin_return_address(0) );
		FLUSH();
		#endif
		return;
	}

	UNALIGN(align,  ptr );

	liballoc_lock();		// lockit


	min = (struct liballoc_minor*)((uintptr_t)ptr - sizeof( struct liballoc_minor ));

    //printk("##\tfree:\tptr:\t0x%x\talign:%d\tid:%d\n", orig_ptr, align, min->iterator);
#ifdef CONFIG_HEAP_DEBUG
    printk("free,%d\n", min->iterator);
    if (min->iterator == 0 || min->iterator > 3000) {
        printk("invalid free of id %d, align 0x%x, ?ptr 0x%x\n",
                    min->iterator, align, orig_ptr);
        dump_stack(8);
    }
#endif

    min->iterator = 13371337;

	
	if ( min->magic != LIBALLOC_MAGIC ) 
	{
		l_errorCount += 1;

		// Check for overrun errors. For all bytes of LIBALLOC_MAGIC 
		if ( 
			((min->magic & 0xFFFFFF) == (LIBALLOC_MAGIC & 0xFFFFFF)) || 
			((min->magic & 0xFFFF) == (LIBALLOC_MAGIC & 0xFFFF)) || 
			((min->magic & 0xFF) == (LIBALLOC_MAGIC & 0xFF)) 
		   )
		{
			l_possibleOverruns += 1;
			//printk( "liballoc: ERROR: Possible 1-3 byte overrun for magic %x != %x\n",
								//min->magic,
								//LIBALLOC_MAGIC );
		}
						
						
		if ( min->magic == LIBALLOC_DEAD )
		{
			//printk( "liballoc: ERROR: multiple PREFIX(free)() attempt on %x from %x.\n", 
									//ptr,
									//__builtin_return_address(0) );
		}
		else
		{
			//#if defined LIBALLOC_DEBUG || defined INFO
			//printk( "liballoc: ERROR: Bad PREFIX(free)( %x ) called from %x\n",
								//ptr, "somewhere");
								//__builtin_return_address(0) );
			//FLUSH();
			//#endif
		}
			
		// being lied to...
		liballoc_unlock();		// release the lock
		return;
	}

	#ifdef LIBALLOC_DEBUG
	printf( "liballoc: %x PREFIX(free)( %x ): ", 
				__builtin_return_address( 0 ),
				ptr );
	FLUSH();
	#endif
	

		maj = min->block;

		l_inuse -= min->size;

		maj->usage -= (min->size + sizeof( struct liballoc_minor ));
		min->magic  = LIBALLOC_DEAD;		// No mojo.

		if ( min->next != NULL ) min->next->prev = min->prev;
		if ( min->prev != NULL ) min->prev->next = min->next;

		if ( min->prev == NULL ) maj->first = min->next;	
							// Might empty the block. This was the first
							// minor.


	// We need to clean up after the majors now....

	if ( maj->first == NULL )	// Block completely unused.
	{
		if ( l_memRoot == maj ) l_memRoot = maj->next;
		if ( l_bestBet == maj ) l_bestBet = NULL;
		if ( maj->prev != NULL ) maj->prev->next = maj->next;
		if ( maj->next != NULL ) maj->next->prev = maj->prev;
		l_allocated -= maj->size;

		liballoc_free( maj, maj->pages );
	}
	else
	{
		if ( l_bestBet != NULL )
		{
			int bestSize = l_bestBet->size  - l_bestBet->usage;
			int majSize = maj->size - maj->usage;

			if ( majSize > bestSize ) l_bestBet = maj;
		}

	}
	

	#ifdef LIBALLOC_DEBUG
	printf( "OK\n");
	FLUSH();
	#endif
	
	liballoc_unlock();		// release the lock
}

void PREFIX(pa_free)(void *ptr)
{
    na_free(4096, ptr);
}


void PREFIX(free)(void *ptr)
{
    na_free(DEFAULT_ALIGNMENT, ptr);
}





void* PREFIX(calloc)(size_t nobj, size_t size)
{
       int real_size;
       void *p;

       real_size = nobj * size;
       
       p = PREFIX(malloc)( real_size );

       liballoc_memset( p, 0, real_size );

       return p;
}


void*   PREFIX(na_realloc)(size_t align, void *p, size_t size)
{
	void *ptr;
	struct liballoc_minor *min;
	unsigned int real_size;

	// Honour the case of size == 0 => free old and return NULL
	if ( size == 0 )
	{
		PREFIX(free)( p );
		return NULL;
	}

	// In the case of a NULL pointer, return a simple malloc.
	if ( p == NULL ) return PREFIX(malloc)( size );

	// Unalign the pointer if required.
	ptr = p;
	UNALIGN(align, ptr);

	liballoc_lock();		// lockit

		min = (struct liballoc_minor*)((uintptr_t)ptr - sizeof( struct liballoc_minor ));

		// Ensure it is a valid structure.
		if ( min->magic != LIBALLOC_MAGIC ) 
		{
			l_errorCount += 1;
	
			// Check for overrun errors. For all bytes of LIBALLOC_MAGIC 
			if ( 
				((min->magic & 0xFFFFFF) == (LIBALLOC_MAGIC & 0xFFFFFF)) || 
				((min->magic & 0xFFFF) == (LIBALLOC_MAGIC & 0xFFFF)) || 
				((min->magic & 0xFF) == (LIBALLOC_MAGIC & 0xFF)) 
			   )
			{
				l_possibleOverruns += 1;
				#if defined LIBALLOC_DEBUG || defined INFO
				printf( "liballoc: ERROR: Possible 1-3 byte overrun for magic %x != %x\n",
									min->magic,
									LIBALLOC_MAGIC );
				FLUSH();
				#endif
			}
							
							
			if ( min->magic == LIBALLOC_DEAD )
			{
				#if defined LIBALLOC_DEBUG || defined INFO
				printf( "liballoc: ERROR: multiple PREFIX(free)() attempt on %x from %x.\n", 
										ptr,
										__builtin_return_address(0) );
				FLUSH();
				#endif
			}
			else
			{
				#if defined LIBALLOC_DEBUG || defined INFO
				printf( "liballoc: ERROR: Bad PREFIX(free)( %x ) called from %x\n",
									ptr,
									__builtin_return_address(0) );
				FLUSH();
				#endif
			}
			
			// being lied to...
			liballoc_unlock();		// release the lock
			return NULL;
		}	
		
		// Definitely a memory block.
		
		real_size = min->req_size;

		if ( real_size >= size ) 
		{
			min->req_size = size;
			liballoc_unlock();
			return p;
		}

	liballoc_unlock();

	// If we got here then we're reallocating to a block bigger than us.
	ptr = PREFIX(malloc)( size );					// We need to allocate new memory
	liballoc_memcpy( ptr, p, real_size );
	PREFIX(free)( p );

	return ptr;
}

void* PREFIX(realloc)(void *p, size_t sz)
{
    return na_realloc(DEFAULT_ALIGNMENT, p, sz);
}

