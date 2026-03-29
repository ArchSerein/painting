#include <lib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* private variables */
static char *mem_heap_start;
static char *mem_heap_end;

int mm_init(void);

static int ensure_mm_init(void) {
  if (mem_heap_start != 0)
    return 0;
  return mm_init();
}

/* 
 * mem_sbrk - simple model of the sbrk function. Extends the heap 
 *    by incr bytes and returns the start address of the new area. In
 *    this model, the heap cannot be shrunk.
 */
void *mem_sbrk(int incr)  {
  char *old_brk = mem_heap_end;

  if (incr < 0) {
	  printf("ERROR: mem_sbrk failed. Ran out of memory...\n");
	  return (void *)-1;
  }
  void *ret = sbrk(incr);
  if (ret == (void *)-1)
    return (void *)-1;
  mem_heap_end += incr;
  return (void *)old_brk;
}


static void *find_fit(uint32_t size) {
  void *bp;

  for(bp = mem_heap_start; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp))
  {
      if((!GET_ALLOC(HDRP(bp))) && (GET_SIZE(HDRP(bp)) >= size))
          return bp;
  }
  return NULL;
}

static void place(void *bp, uint32_t asize) {
  uint32_t csize = GET_SIZE(HDRP(bp));

  if((csize - asize) >= 2*DSIZE) {
      PUT(HDRP(bp), PACK(asize,1));
      PUT(FTRP(bp), PACK(asize,1));

      bp = NEXT_BLKP(bp);

      PUT(HDRP(bp), PACK((csize-asize),0));
      PUT(FTRP(bp), PACK((csize-asize),0));
  }
  else    {
      PUT(HDRP(bp), PACK(csize, 1));
      PUT(FTRP(bp), PACK(csize, 1));
  }
}


static void *coalesce(void *bp) {
  uint32_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
  uint32_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
  uint32_t size = GET_SIZE(HDRP(bp));

  if(prev_alloc && next_alloc)
      return bp;
  else if(prev_alloc && !next_alloc)
  {
      size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
      PUT(FTRP(bp), PACK(size,0));
      PUT(FTRP(bp),PACK(size,0));
  }
  else if(!prev_alloc && next_alloc)
  {
      size += GET_SIZE(HDRP(PREV_BLKP(bp)));
      PUT(FTRP(bp), PACK(size,0));
      PUT(HDRP(PREV_BLKP(bp)),PACK(size,0));
      bp = PREV_BLKP(bp);
  }
  else {
      size += (GET_SIZE(HDRP(PREV_BLKP(bp))) + GET_SIZE(FTRP(NEXT_BLKP(bp))));
      PUT(HDRP(PREV_BLKP(bp)),PACK(size,0));
      PUT(FTRP(NEXT_BLKP(bp)), PACK(size,0));
      bp = PREV_BLKP(bp);
  }
  return bp;
}


static void *extend_heap(uint32_t words) {
  char *bp;
  uint32_t size;

  size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;
  if((long)(bp = mem_sbrk(size)) == -1)
      return NULL;
  
  PUT(HDRP(bp),PACK(size,0));
  PUT(FTRP(bp),PACK(size,0));
  PUT(HDRP(NEXT_BLKP(bp)), PACK(0,1));

  return coalesce(bp);
}

/* 
 * mm_init - initialize the malloc package.
 */

int mm_init(void) {
  mem_heap_start = sbrk(0);
  mem_heap_end = mem_heap_start;

  if(mem_sbrk(4*WSIZE) == (void *)-1)
      return -1;
  PUT(mem_heap_start,0);
  PUT(mem_heap_start + (1*WSIZE), PACK(DSIZE,1));
  PUT(mem_heap_start + (2*WSIZE), PACK(DSIZE,1));
  PUT(mem_heap_start + (3*WSIZE), PACK(0,1));

  mem_heap_start += (2*WSIZE);
  if(extend_heap(CHUNKSIZE/WSIZE) == NULL)
      return -1;

  return 0;
}

/* 
 * malloc - Allocate a block by incrementing the brk pointer.
 *     Always allocate a block whose size is a multiple of the alignment.
 */
void *malloc(uint32_t size) {
  uint32_t asize;
  uint32_t extendsize;
  char *bp;

  if (ensure_mm_init() < 0)
      return NULL;

  if(size == 0) {
      return NULL;
  }
  if(size <= DSIZE)
      asize = 2*DSIZE;
  else 
      asize = DSIZE * ((size + (DSIZE) + (DSIZE-1)) / DSIZE);

  if((bp = find_fit(asize)) != NULL) {
      place(bp,asize);
      return bp;
  }

  extendsize = MAX(asize,CHUNKSIZE);
  if ((bp = extend_heap(extendsize/WSIZE)) == NULL)
      return NULL;

  place(bp,asize);
  return bp;
}

/*
 * free - Freeing a block does nothing.
 */

void free(void *ptr) {
  if (ptr == 0)
      return;
  uint32_t size = GET_SIZE(HDRP(ptr));

  PUT(HDRP(ptr),PACK(size,0));
  PUT(FTRP(ptr),PACK(size,0));

  coalesce(ptr);
}

/*
 * realloc - Implemented simply in terms of mm_malloc and mm_free
 */
void *realloc(void *ptr, uint32_t size) {
  void *oldptr = ptr;
  void *newptr;

  newptr = malloc(size);
  if (newptr == NULL)
    return NULL;
  uint32_t oldsz = GET_SIZE(HDRP(oldptr));
  memcpy(newptr, oldptr, MIN(size, oldsz));
  free(oldptr);
  return newptr;
}
