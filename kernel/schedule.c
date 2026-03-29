#include <types.h>
#include <param.h>
#include <spinlock.h>
#include <macro.h>
#include <list.h>
#include <proc.h>
#include <defs.h>
#include <riscv.h>
#include <schedule.h>
#include <memlayout.h>

static struct list run_queue;
static struct spinlock rq_lock;

void rb_push_back(struct proc *p) {
  acquire(&rq_lock);
  if (!p->in_run_queue) {
    list_push_back(&run_queue, &p->sched_elem);
    p->in_run_queue = true;
  }
  release(&rq_lock);
}

void rb_pop_front(struct proc *p) {
  acquire(&rq_lock);
  if (p->in_run_queue) {
    list_remove(&p->sched_elem);
    p->in_run_queue = false;
  }
  release(&rq_lock);
}

void rb_init() {
  list_init(&run_queue);
  initlock(&rq_lock, "run_queue");
}

void yield() {
  struct proc *p = cur_proc();
  acquire(&p->lock);
  p->status = RUNNABLE;
  rb_push_back(p);
  sched();
  release(&p->lock);
}

extern void swtch(struct context *old, struct context *new);
void schedule(void) {
  struct cpu *c = cur_cpu();

  intr_on();
  for (;;) {
    acquire(&rq_lock);
    struct proc *next = NULL;
    struct list_elem *e;
    for (e = list_begin(&run_queue); e != list_end(&run_queue); e = list_next(e)) {
      struct proc *p = list_entry(e, struct proc, sched_elem);
      if (p->status == RUNNABLE) {
        list_remove(e);
        p->in_run_queue = false;
        next = p;
        break;
      }
    }
    release(&rq_lock);

    if (next != NULL) {
      acquire(&next->lock);
      next->status = RUNNING;
      c->proc = next;
      swtch(&c->context, &next->context);
      c->proc = 0;
      release(&next->lock);
    } else {
      intr_on();
      asm volatile("wfi");
    }
  }
}
