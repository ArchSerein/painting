#include <types.h>
#include <memlayout.h>
#include <spinlock.h>
#include <macro.h>
#include <stdio.h>
#include <string.h>
#include <pm.h>

// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kpminit() {
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end) {
  char *p;
  p = (char*)PGROUNDUP((uint64_t)pa_start);
  // uint64_t pg_num = 0;
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE) {
    kpmfree(p);
    // ++pg_num;
    #ifdef CONFIG_DEBUG
    // log("pg_num: %d\n", pg_num);
    #endif
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kpmalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kpmfree(void *pa) {
  struct run *r;

  if(((uint64_t)pa % PGSIZE) != 0 || (char*)pa < end || (uint64_t)pa >= PHYSTOP)
    panic("kpmfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void * 
kpmalloc(void) {
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

void * kpm_multiple_alloc(int page_num) {
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  for (int i = 0; i < page_num; i++) {
    if (r)
      kmem.freelist = kmem.freelist->next;
    else {
      kmem.freelist = r;
      r = NULL;
      break;
    }
  }
  release(&kmem.lock);

  if (r)
    memset((char *)r, 5, PGSIZE * page_num);
  return (void *)r;
}

void kpm_multiple_free(void *pa, int page_num) {
  struct run *r;
  struct run *tmp;

  if(((uint64_t)pa % PGSIZE) != 0 || (char*)pa < end || (uint64_t)pa >= PHYSTOP)
    panic("kpmfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE * page_num);

  r = (struct run*)pa;
  tmp = r;

  acquire(&kmem.lock);
  for (int i = 0; i < page_num; i++) {
    tmp = tmp->next;
  }
  tmp->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}
