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
static uint64_t min_vruntime;

static bool cfs_less(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  struct proc *pa = list_entry(a, struct proc, sched_elem);
  struct proc *pb = list_entry(b, struct proc, sched_elem);

  if (pa->vruntime != pb->vruntime)
    return pa->vruntime < pb->vruntime;
  return pa->pid < pb->pid;
}

static void refresh_min_vruntime_locked(void) {
  if (!list_empty(&run_queue)) {
    struct proc *leftmost = list_entry(list_front(&run_queue), struct proc, sched_elem);
    if (leftmost->vruntime > min_vruntime)
      min_vruntime = leftmost->vruntime;
  }
}

void rb_push_back(struct proc *p) {
  acquire(&rq_lock);
  if (!p->in_run_queue) {
    if (p->vruntime < min_vruntime)
      p->vruntime = min_vruntime;
    list_insert_ordered(&run_queue, &p->sched_elem, cfs_less, NULL);
    p->in_run_queue = true;
    refresh_min_vruntime_locked();
  }
  release(&rq_lock);
}

void rb_pop_front(struct proc *p) {
  acquire(&rq_lock);
  if (p->in_run_queue) {
    list_remove(&p->sched_elem);
    p->in_run_queue = false;
    refresh_min_vruntime_locked();
  }
  release(&rq_lock);
}

void rb_init() {
  list_init(&run_queue);
  initlock(&rq_lock, "run_queue");
  min_vruntime = 0;
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
    for (e = list_begin(&run_queue); e != list_end(&run_queue);) {
      struct list_elem *n = list_next(e);
      struct proc *p = list_entry(e, struct proc, sched_elem);
      if (p->status == RUNNABLE) {
        list_remove(e);
        p->in_run_queue = false;
        next = p;
        break;
      }
      list_remove(e);
      p->in_run_queue = false;
      e = n;
    }
    refresh_min_vruntime_locked();
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
