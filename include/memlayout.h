#ifndef MEMLAYOUT_H__
#define MEMLAYOUT_H__

#define   PGSIZE      4096

#define   UART0       0x10000000L
#define   UART0_IRQ   10

#define   VIRTIO0     0x10001000L
#define   VIRTIO0_IRQ 1

#define   PLIC            0x0c000000L
// 中断优先级
// base = PLIC, BASE + 0X0: Reserved (interrupt source 0 does not exist)
// base + 0x4: Interrupt source 1 priority
// .....
// base + 0xFFC: Interrupt source 1023 priority
#define   PLIC_PRIORITY           (PLIC + 0x0)
// Interrupt pending bit 0-31
#define   PLIC_PENDING            (PLIC + 0x1000)
// Interrupt send Enable
// Each hart has two modes, S/M, and requires two Enable contexts.
// the size of context is 0x80
#define   PLIC_SENABLE(hartid)    (PLIC + 0x2080 + (hartid) * 0x100)
#define   PLIC_STHRESHOLD(hartid) (PLIC + 0x201000 + (hartid) * 0x2000)
#define   PLIC_SCLAIM(hartid)     (PLIC + 0x201004 + (hartid) * 0x2000)

// one beyond the highest possible virtual address.
// MAXVA is actually one bit less than the max allowed by
// Sv39, to avoid having to sign-extend virtual addresses
// that have the high bit set.
#define MAXVA (1L << (9 + 9 + 9 + 12 - 1))

// the kernel expects there to be RAM
// for use by the kernel and user pages
// from physical address 0x80000000 to PHYSTOP.
#define KERNBASE 0x80200000L
#define PHYSTOP (KERNBASE + 128*1024*1024 - 0x200000L)

// map the trampoline page to the highest address,
// in both user and kernel space.
#define TRAMPOLINE (MAXVA - PGSIZE)

// map kernel stacks beneath the trampoline,
// each surrounded by invalid guard pages.
#define KSTACK(p) (TRAMPOLINE - ((p)+1)* 2*PGSIZE)

// User memory layout.
// Address zero first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
//   ...
//   TRAPFRAME (p->trapframe, used by the trampoline)
//   TRAMPOLINE (the same page as in the kernel)
#define TRAPFRAME (TRAMPOLINE - PGSIZE)
#define USTACK    (TRAPFRAME - PGSIZE)
#endif //MEMLAYOUT_H__
