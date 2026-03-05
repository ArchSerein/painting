#include <types.h>
#include <param.h>
#include <riscv.h>
#include <spinlock.h>
#include <list.h>
#include <proc.h>
#include <macro.h>
#include <defs.h>
#include <vm.h>
#include <uvm.h>
#include <syscall.h>
#include <string.h>

static void transtr(uint64_t addr, char *buf, uint64_t size, bool direction) {
  // The implementation of checking the legitimacy of an addr is in the valid_user_arg
  if (direction) {
    if (copyin(cur_proc()->pagetable, buf, addr, size) < 0) {
      log("copy data from user to kernel fault\n");
      process_exit(-1);
    }
  } else {
    if (copyout(cur_proc()->pagetable, addr, buf, size) < 0) {
      log("copy data from user to kernel fault\n");
      process_exit(-1);
    }
  }
}

uint64_t sys_getpid(void) {
  return cur_proc()->pid;
}

uint64_t sys_exec(void) {
  uint64_t ufile, uargv;
  uint64_t uaddr;
  argaddr(&ufile, 0);
  argaddr(&uargv, 1);

  char path[MAXLEN];
  static char argv[MAXARG][MAXLEN];

  transtr(ufile, path, MAXLEN, true);
  for (int i = 0; i < MAXARG; i++) {
    transtr(uargv + sizeof (uint64_t) * i, (char *)&uaddr, sizeof(uint64_t), true);
    if (uaddr == 0) {
      memset(argv[i], 0, MAXLEN);
      break;
    }
    transtr(uaddr, argv[i], MAXLEN, true);
  }

  if (process_execute(path, (const char **)argv))
    return 0;
  else
    return -1;
}

uint64_t sys_exit(void) {
  uint64_t state;
  argint(&state, 0);

  process_exit(state);
}

static uint64_t incr_heap_space(uint64_t incr) {
  struct proc *p = cur_proc();
  uint64_t ret = p->heap_end;

  if (uvmalloc(p->pagetable, p->heap_end, p->heap_end + incr, PTE_W | PTE_V) == 0)
    return -1;
  else {
    p->heap_end += incr;
    return ret;
  }
}

static uint64_t desc_heap_space(uint64_t incr) {
  struct proc *p = cur_proc();
  uint64_t ret = p->heap_end;

  if (uvmdealloc(p->pagetable, p->heap_end, p->heap_end + incr) == 0)
    return -1;
  else {
    p->heap_end -= incr;
    return ret;
  }
}

uint64_t sys_sbrk(void) {
  uint64_t incr;
  struct proc *p = cur_proc();
  argint(&incr, 0);

  if (incr == 0)
    return p->heap_end;
  else if (incr > 0)
    return incr_heap_space(incr);
  else
    return desc_heap_space(-incr);
}

uint64_t sys_fork(void) {
  return fork();
}

uint64_t sys_wait(void) {
  uint64_t pid;
  argint(&pid, 0);
  return process_wait((pid_t)pid);
}
