#include <types.h>
#include <param.h>
#include <memlayout.h>
#include <riscv.h>
#include <vm.h>
#include <pm.h>
#include <sbi.h>
#include <macro.h>
#include <string.h>
#include <stdio.h>

// kernel.ld provide
extern char trampoline[];
extern char etext[];

pagetable_t   kernel_pagetable;

void kvmmap(pagetable_t pgtble, uint64_t va, uint64_t pa, uint64_t sz, int mode) {
  if (mappages(pgtble, va, pa, sz, mode) < 0) {
    #ifdef CONFIG_DEBUG
    log("kvminit fault and will shutdown\n");
    #endif
    sbi_shutdown();
  }
}

pagetable_t kvmmake(void) {
  pagetable_t kpgtbl;
  // alloc a physical page as kernel page
  // to record virtual address mapping information
  kpgtbl = (pagetable_t)kpmalloc();
  memset(kpgtbl, 0, PGSIZE);

  // serial address: UART0
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);
  // virtio_mmio
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_W | PTE_R);
  // plic
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);
  // kernel text
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64_t)etext - KERNBASE, PTE_R | PTE_X);
  // kernel data and the physical RAM
  kvmmap(kpgtbl, (uint64_t)etext, (uint64_t)etext, PHYSTOP - (uint64_t)etext, PTE_W | PTE_R);
  // trampoine
  kvmmap(kpgtbl, TRAMPOLINE, (uint64_t)trampoline, PGSIZE, PTE_R | PTE_X);

  return kpgtbl;
}

void kvminit(void) {
  kernel_pagetable = kvmmake();
}

// V(Valid)：仅当位 V 为 1 时，页表项才是合法的；
//
// R(Read)/W(Write)/X(eXecute)：分别控制索引到这个页表项的对应虚拟页面是否允许读/写/执行；
//
// U(User)：控制索引到这个页表项的对应虚拟页面是否在 CPU 处于 U 特权级的情况下是否被允许访问；
//
// G：暂且不理会；
//
// A(Accessed)：处理器记录自从页表项上的这一位被清零之后，页表项的对应虚拟页面是否被访问过；
//
// D(Dirty)：处理器记录自从页表项上的这一位被清零之后，页表项的对应虚拟页面是否被修改过。
pte_t *walk(pagetable_t pgtble, uint64_t va, int alloc) {
  if (va >= MAXVA)
    panic("walk");

  pte_t *pte;
  for (int level = 2; level > 0; level--) {
    pte = &pgtble[IDX(level, va)];
    if (*pte & PTE_V) {
      pgtble = PTE2PA(*pte);
    } else {
      // 如果 va 对应的页表条目不存在
      // 那么根据 alloc 的值判断是否需要分配一个新的页面
      if (!alloc || (pgtble = (pte_t *)kpmalloc()) == 0) {
        return 0;
      }
      // 分配成功将该页面映射到相应的虚拟地址
      memset(pgtble, 0, PGSIZE);
      *pte = PA2PTE(pgtble) | PTE_V;
    }
  }

  return &pgtble[IDX(0, va)];
}

int mappages(pagetable_t pgtble, uint64_t va, uint64_t pa, uint64_t sz, int mode) {
  if ((va & (PGSIZE-1)) != 0)
    panic("va not aligned");
  if ((sz & (PGSIZE-1)) != 0)
    panic("sz not aligned");

  uint64_t a = va;
  uint64_t end = va + sz - PGSIZE;
  pte_t *pte;
  while(true) {
    pte = walk(pgtble, a, 1);
    if (pte == 0)
      return -1;
    if (*pte & PTE_V) {
      panic("mappages: remap");
    }
    *pte = PA2PTE(pa) | mode | PTE_V;
    if (a == end)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      pagetable_t child = PTE2PA(pte);
      freewalk(child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kpmfree((void*)pagetable);
}

int copyout(pagetable_t pagetable, uint64_t dst, const char *src, uint64_t len) {
  uint64_t n, va0, pa0;
  pte_t *pte;

  while (len > 0) {
    va0 = PGROUNDDOWN(dst);
    if (va0 > MAXVA)
      return -1;
    pte = walk(pagetable, va0, 0);
    if (pte == NULL || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0 || (*pte & PTE_W) == 0)
      return -1;
    pa0 = (uint64_t)PTE2PA(*pte);
    n = PGSIZE - (dst - va0);
    n = n > len ? len : n;
    memmove((void *)(pa0 + (dst - va0)), src, n);

    len -= n;
    src += n;
    dst = va0 + PGSIZE;
  }
  return 0;
}

int copyin(pagetable_t pagetable, char *dst, uint64_t src, uint64_t len) {
  uint64_t n, va0, pa0;
  pte_t *pte;

  while(len > 0) {
    va0 = PGROUNDDOWN(src);
    if (va0 > MAXVA)
      return -1;
    pte = walk(pagetable, va0, 0);
    if (pte == NULL || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
      return -1;
    pa0 = (uint64_t)PTE2PA(*pte);
    n = PGSIZE - (src - va0);
    n = n > len ? len : n;
    memmove(dst, (void *)(pa0 + (src - va0)), n);

    len -= n;
    dst += n;
    src = va0 + PGSIZE;
  }
  return 0;
}

void vmprint(pagetable_t pgtbl, int level) {
  if (level > 2)
    return;
  for (int i = 0; i < 512; i++) {
    pte_t pte = pgtbl[i];
    if (pte & PTE_V) {
      for (int j = level; j; j--) {
        printf(".. ");
      }
      printf("..%d: pte %p pa %p\n", i, pte, (pagetable_t)PTE2PA(pte));
      vmprint((pagetable_t)PTE2PA(pte), level+1);
    }
  }
}
