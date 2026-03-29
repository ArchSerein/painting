#include <types.h>
#include <param.h>
#include <riscv.h>
#include <macro.h>
#include <spinlock.h>
#include <stdio.h>
#include <list.h>
#include <proc.h>
#include <defs.h>

void initlock(struct spinlock *lk, char *name) {
  lk->locked = 0;
  lk->cpu = 0;
  lk->name = name;
}

void acquire(struct spinlock *lk) {
  push_off(); // disable interrupts to avoid deadlock.
  if(holding(lk)) {
    panic("acquire");
  }

  // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
  //   a5 = 1
  //   s1 = &lk->locked
  //   amoswap.w.aq a5, a5, (s1)
  while(__sync_lock_test_and_set(&lk->locked, 1) != 0)
    ;

  // Tell the C compiler and the processor to not move loads or stores
  // past this point, to ensure that the critical section's memory
  // references happen strictly after the lock is acquired.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();
  lk->cpu = cur_cpu();
}

void release(struct spinlock *lk) {
  if(!holding(lk)) {
    #ifdef CONFIG_DEBUG
    log("cur_proc: %p, lock: %s\n", cur_proc(), lk->name);
    #endif
    panic("release");
  }

  lk->cpu = NULL;
  // Tell the C compiler and the CPU to not move loads or stores
  // past this point, to ensure that all the stores in the critical
  // section are visible to other CPUs before the lock is released,
  // and that loads in the critical section occur strictly before
  // the lock is released.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();

  // Release the lock, equivalent to lk->locked = 0.
  // This code doesn't use a C assignment, since the C standard
  // implies that an assignment might be implemented with
  // multiple store instructions.
  // On RISC-V, sync_lock_release turns into an atomic swap:
  //   s1 = &lk->locked
  //   amoswap.w zero, zero, (s1)
  __sync_lock_release(&lk->locked);
  pop_off();
}
uint32_t holding(struct spinlock *lk) {
  return lk->cpu == cur_cpu() && lk->locked;
}

void
push_off(void)
{
  int old = intr_get();

  intr_off();
  if(cur_cpu()->noff == 0)
    cur_cpu()->intena = old;
  cur_cpu()->noff += 1;
}

void
pop_off(void)
{
  struct cpu *c = cur_cpu();
  if(intr_get())
    panic("pop_off - interrupt enable");
  if(c->noff < 1)
    panic("pop_off");
  c->noff -= 1;
  if(c->noff == 0 && c->intena)
    intr_on();
}
