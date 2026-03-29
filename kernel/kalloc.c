#include <types.h>
#include <param.h>
#include <buddy.h>
#include <spinlock.h>
#include <list.h>
#include <proc.h>
#include <macro.h>
#include <string.h>
#include <defs.h>
#include <stdio.h>
#include <vfs.h>
#include <schedule.h>

#define WSIZE             4
#define PGSIZE            4096
#define PAGE_ALLOC_SIZE   (PGSIZE - 4 * ALIGN)
#define PROC_SIZE         (ROUNDUP(sizeof (struct proc), WSIZE))
#define RBNODE_SIZE       (ROUNDUP(sizeof (struct rbNode), WSIZE))
#define FILE_SIZE         (ROUNDUP(sizeof (struct file), WSIZE))
#define PROC_BLOCK_NUM    (PAGE_ALLOC_SIZE / (PROC_SIZE))
#define RB_BLOCK_SIZE     (PAGE_ALLOC_SIZE / (RBNODE_SIZE))
#define FILE_BLOCK_SIZE   (PAGE_ALLOC_SIZE / (FILE_SIZE))
#define PACK(ptr,lev)     ((uint64_t)(ptr) | (lev))
#define UNPACK(addr)      (*(uint64_t *)(addr) & (~0xff))
#define GET_LEVEL(addr)   (*(uint64_t *)(addr) & 0xff)
#define PUT(bp,val,type)  (*(type *)(bp) = val);
#define GET(bp,type)      (*(type *)(bp));
#define HDRP(bp)          ((void *)((uint8_t *)(bp)-ALIGN))
#define FTRP(bp)          ((void *)((uint8_t *)(bp)+ALIGN))
static int find_lowest_zero_bit(uint64_t num, int width) {
  for (int pos = 0; pos < width; pos++) {
    if ((num & (1ULL << pos)) == 0)
      return pos;
  }
  return -1;
}
struct blockret {
  void      *list;
  uint32_t  off;
} ;

static char *proc_heap_list;
static char *rb_heap_list;
static char *file_heap_list;

static void init_bitmap_helper(char *ptr, uint32_t block_num) {
  uint32_t block_num_align = block_num >> 3;
  uint32_t i;
  for (i = 0; i < MIN(block_num_align, 8); i++) {
    PUT(ptr+i, 0, char);
  }
  char mask = (1 << (block_num & 0x07)) - 1;
  char val = 0xff & (~mask);
  PUT(ptr+i, val, char);
  // 其余的 bit 需要置为1
  for (i = i + 1; i < 16; i++) {
    PUT(ptr+i, (char)0xff, char);
  }
}

static struct blockret get_block_index_helper(char *bp, char *list, uint32_t block_size) {
  while (true) {
    if (bp >= list && bp < (list + PAGE_ALLOC_SIZE))
      break;
    else {
      uint64_t val;
      val = GET(list+PAGE_ALLOC_SIZE, uint64_t);
      list = (char *)UNPACK(val);
    }
  }
  uint32_t off = (uint32_t)(bp - list) / block_size;
  return (struct blockret){.list = list, .off = off};
}
// 在对应的 list 找到空闲的空间
static void *find_match_space(char *list, size_t size) {
  uint64_t free_bitmap;
  uint64_t  free_bitmap_extend;

  free_bitmap = GET(HDRP(HDRP(list)), uint64_t);
  free_bitmap_extend = GET(HDRP(list), uint64_t);

  uint32_t off = 0;
  if (free_bitmap == (uint64_t)(-1)) {
    if (free_bitmap_extend == (uint64_t)(-1))
      return NULL;
    else
      off = find_lowest_zero_bit(free_bitmap_extend, 64);
    if (off < 0)
      return NULL;
    free_bitmap_extend |= (1ULL << off);
    off += 64;
  } else {
    off = find_lowest_zero_bit(free_bitmap, 64);
    if (off < 0)
      return NULL;
    free_bitmap |= (1ULL << off);
  }

  PUT(HDRP(list), free_bitmap_extend, uint64_t);
  PUT(HDRP(HDRP(list)), free_bitmap, uint64_t);
  return list + off * size;
}

static void heap_list_init(void **heap_list, void *prev, uint32_t num) {
  void *ptr = buddy_alloc(PGSIZE);
  uint8_t level = *(uint8_t *)ptr;
  PUT(ptr, PACK(prev, level), uint64_t);
  if (prev != NULL)
    PUT(prev+PAGE_ALLOC_SIZE, PACK(ptr, level), uint64_t);
  ptr = FTRP(ptr);
  init_bitmap_helper(ptr, num);
  *heap_list = FTRP(FTRP(ptr));
}

void slab_init(void) {
  heap_list_init((void **)&proc_heap_list, 0, PROC_BLOCK_NUM);
  heap_list_init((void **)&rb_heap_list, 0, RB_BLOCK_SIZE);
  heap_list_init((void **)&file_heap_list, 0, FILE_BLOCK_SIZE);
}
void *kalloc(size_t size, mode_t mode) {
  void *bp  = NULL;
  void *ptr = NULL;
  switch (mode) {
    case PROC_MODE:
      if ((bp = find_match_space(proc_heap_list, PROC_SIZE)) == NULL) {
        heap_list_init(&ptr, (void *)proc_heap_list, PROC_BLOCK_NUM);
        proc_heap_list = ptr;
        bp = find_match_space(proc_heap_list, PROC_SIZE);
      }
      return bp;
    case RB_MODE:
      if ((bp = find_match_space(rb_heap_list, RBNODE_SIZE)) == NULL) {
        heap_list_init(&ptr, (void *)rb_heap_list, RB_BLOCK_SIZE);
        rb_heap_list = ptr;
        bp = find_match_space(rb_heap_list, RBNODE_SIZE);
      }
      return bp;
    case FILE_MODE:
      if ((bp = find_match_space(file_heap_list, FILE_SIZE)) == NULL) {
        heap_list_init(&ptr, (void *)file_heap_list, FILE_BLOCK_SIZE);
        file_heap_list = ptr;
        bp = find_match_space(file_heap_list, FILE_SIZE);
      }
      return bp;
    default:
      if (size == 0)
        return NULL;
      size_t need = size + ALIGN;
      if (need < size)
        return NULL;
      size_t sz = next_pow_of_2(need);
      ptr = buddy_alloc(sz);
      if (ptr == NULL)
        return NULL;
      return FTRP(ptr);
  }
}

void kfree(void *addr, mode_t mode) {
  if (addr == NULL) {
    #ifdef CONFIG_DEBUG
    log("warning: Trying to free a null pointer\n");
    #endif
    return;
  }
  char *list;
  size_t size;
  switch (mode) {
    case PROC_MODE:
      list = proc_heap_list;
      size = PROC_SIZE;
      goto do_free;
    case RB_MODE:
      list = rb_heap_list;
      size = RBNODE_SIZE;
      goto do_free;
    case FILE_MODE:
      list = file_heap_list;
      size = FILE_SIZE;
      goto do_free;
    default:
      uint8_t level = GET(HDRP(addr), uint8_t);
      if (level > MAX_LEVEL)
        panic("kfree: invalid buddy level");
      size = 1UL << level;
      if (size > ALIGN)
        memset(addr, 0xc, size - ALIGN);
      buddy_free(HDRP(addr));
      return;
  }

  // 对于 slab cache 的内存块实际释放的操作
do_free:
  memset(addr, 0xc, size);
  struct blockret ret = get_block_index_helper(addr, list, size);
  if (ret.off >= 64) {
    uint64_t val = GET(HDRP(ret.list), uint64_t);
    val &= (~(1ULL << (ret.off - 64)));
    PUT(HDRP(ret.list), val, uint64_t);
  } else {
    uint64_t val = GET(HDRP(HDRP(ret.list)), uint64_t);
    val &= (~(1ULL << ret.off));
    PUT(HDRP(HDRP(ret.list)), val, uint64_t);
  }
  uint64_t val1 = GET(HDRP(HDRP(ret.list)), uint64_t);
  uint64_t val2 = GET(HDRP(ret.list), uint64_t);
  uint64_t prev_val = GET(HDRP(HDRP(HDRP(ret.list))), uint64_t);
  uint64_t next_val = GET(ret.list+PAGE_ALLOC_SIZE, uint64_t);
  if (val1 == (uint64_t)(-1) && val2 == (uint64_t)(-1)) {
    void *prev = (void *)UNPACK(prev_val);
    void *next = (void *)UNPACK(next_val);
    PUT(prev+PAGE_ALLOC_SIZE, next_val, uint64_t);
    PUT(HDRP(HDRP(HDRP(next))), prev_val, uint64_t);
    buddy_free(HDRP(HDRP(HDRP(ret.list))));
  }
}
