#include <types.h>
#include <param.h>
#include <macro.h>
#include <stdio.h>
#include <string.h>
#include <riscv.h>
#include <memlayout.h>
#include <vm.h>
#include <pm.h>
#include <uvm.h>

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void uvmunmap(pagetable_t pagetable, uint64_t va, uint64_t npages, int do_free) {
  uint64_t a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64_t pa = (uint64_t)PTE2PA(*pte);
      kpmfree((void*)pa);
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64_t uvmalloc(pagetable_t pagetable, uint64_t oldsz, uint64_t newsz, int xperm) {
  char *mem;
  uint64_t a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kpmalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, (uint64_t)mem, PGSIZE, PTE_R|PTE_U|xperm) != 0){
      kpmfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64_t uvmdealloc(pagetable_t pagetable, uint64_t oldsz, uint64_t newsz) {
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

static void
uvmfree_recursive(pagetable_t pagetable, int level)
{
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if(pte & PTE_V){
      pagetable[i] = 0;
      if(level > 0 && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
        uvmfree_recursive(PTE2PA(pte), level - 1);
      } else if(pte & PTE_U){
        kpmfree((void*)PTE2PA(pte));
      }
    }
  }
  kpmfree((void*)pagetable);
}

void uvmfree(pagetable_t pagetable, uint64_t sz) {
  uvmfree_recursive(pagetable, 2);
}

int uvmcopy(pagetable_t old, pagetable_t new, uint64_t sz) {
  pte_t *pte;
  uint64_t pa, i;
  uint32_t flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    pte = walk(old, i, 0);
    if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
      continue;

    pa = (uint64_t)PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kpmalloc()) == 0)
      return -1;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, (uint64_t)mem, PGSIZE, flags & (PTE_R|PTE_W|PTE_X|PTE_U)) != 0){
      kpmfree(mem);
      return -1;
    }
  }
  return 0;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64_t va) {
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64_t walkaddr(pagetable_t pagetable, uint64_t va) {
  pte_t *pte;
  uint64_t pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = (uint64_t)PTE2PA(*pte);
  return pa;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t uvmcreate() {
  pagetable_t pagetable;
  pagetable = (pagetable_t) kpmalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}
