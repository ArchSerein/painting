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

static int transtr(uint64_t addr, char *buf, uint64_t size, bool direction) {
  if (direction) {
    if (copyin(cur_proc()->pagetable, buf, addr, size) < 0) {
      #ifdef CONFIG_DEBUG
      log("copy data from user to kernel fault\n");
      #endif
      return -1;
    }
  } else {
    if (copyout(cur_proc()->pagetable, addr, buf, size) < 0) {
      #ifdef CONFIG_DEBUG
      log("copy data from kernel to user fault\n");
      #endif
      return -1;
    }
  }
  return 0;
}

static int transstr(uint64_t uaddr, char *buf, uint64_t maxlen) {
  struct proc *p = cur_proc();
  for (uint64_t i = 0; i < maxlen; i++) {
    if (copyin(p->pagetable, &buf[i], uaddr + i, 1) < 0) {
      #ifdef CONFIG_DEBUG
      log("copy string from user to kernel fault\n");
      #endif
      return -1;
    }
    if (buf[i] == '\0')
      return 0;
  }
  buf[maxlen - 1] = '\0';
  return 0;
}

uint64_t sys_getpid(void) {
  return cur_proc()->pid;
}

uint64_t sys_exec(void) {
  uint64_t ufile, uargv;
  uint64_t uaddr;
  argaddr(&ufile, 0);
  argaddr(&uargv, 1);
  #ifdef CONFIG_DEBUG
  log("sys_exec pid=%d ufile=0x%p uargv=0x%p\n", cur_proc()->pid, ufile, uargv);
  #endif

  char path[MAXLEN];
  char (*argbuf)[MAXLEN] = kalloc(MAXARG * MAXLEN, DEFAULT);
  const char *argv[MAXARG];
  memset(argv, 0, sizeof(argv));
  if (argbuf == NULL)
    return -1;

  if (transstr(ufile, path, MAXLEN) < 0) {
    #ifdef CONFIG_DEBUG
    log("sys_exec path copy fail pid=%d ufile=0x%p\n", cur_proc()->pid, ufile);
    #endif
    kfree(argbuf, DEFAULT);
    return -1;
  }

  for (int i = 0; i < MAXARG; i++) {
    if (transtr(uargv + sizeof (uint64_t) * i, (char *)&uaddr, sizeof(uint64_t), true) < 0) {
      kfree(argbuf, DEFAULT);
      return -1;
    }
    if (uaddr == 0) {
      argv[i] = NULL;
      break;
    }
    if (transstr(uaddr, argbuf[i], MAXLEN) < 0) {
      kfree(argbuf, DEFAULT);
      return -1;
    }
    argv[i] = argbuf[i];
  }

  bool ok = process_execute(path, argv);
  kfree(argbuf, DEFAULT);
  if (ok)
    return 0;
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
