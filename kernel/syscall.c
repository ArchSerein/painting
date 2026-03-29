#include <types.h>
#include <param.h>
#include <macro.h>
#include <list.h>
#include <spinlock.h>
#include <proc.h>
#include <defs.h>
#include <stdio.h>
#include <syscall.h>
#include <syscall-nr.h>

static uint64_t argraw(int n) {
  struct proc *p = cur_proc();
  switch (n) {
  case 0:
    return p->trapframe->a0;
  case 1:
    return p->trapframe->a1;
  case 2:
    return p->trapframe->a2;
  case 3:
    return p->trapframe->a3;
  case 4:
    return p->trapframe->a4;
  case 5:
    return p->trapframe->a5;
  }
  panic("argraw");
  return -1;
}

void argint(uint64_t *ptr, int n) {
  *ptr = argraw(n);
}
void argaddr(uint64_t *ptr, int n) {
  *ptr = argraw(n);
}

// An array mapping syscall numbers from syscall.h
// to the function that handles the system call.
static uint64_t (*syscalls[])(void) = {
[SYS_fork]    sys_fork,
[SYS_exit]    sys_exit,
[SYS_wait]    sys_wait,
// [SYS_pipe]    sys_pipe,
[SYS_read]    sys_read,
// [SYS_kill]    sys_kill,
[SYS_exec]    sys_exec,
// [SYS_fstat]   sys_fstat,
// [SYS_chdir]   sys_chdir,
// [SYS_dup]     sys_dup,
[SYS_getpid]  sys_getpid,
[SYS_sbrk]    sys_sbrk,
// [SYS_sleep]   sys_sleep,
// [SYS_uptime]  sys_uptime,
[SYS_open]    sys_open,
[SYS_write]   sys_write,
// [SYS_mknod]   sys_mknod,
// [SYS_unlink]  sys_unlink,
// [SYS_link]    sys_link,
// [SYS_mkdir]   sys_mkdir,
[SYS_close]   sys_close,
[SYS_create]  sys_create,
};

void syscall(void) {
  int num;
  struct proc *p = cur_proc();

  num = p->trapframe->a7;
  #ifdef CONFIG_DEBUG
  log("syscall enter pid=%d num=%d epc=0x%p\n", p->pid, num, p->trapframe->epc);
  #endif
  bool is_valid_syscall = num > 0 && num < array_size(syscalls) && syscalls[num];
  if (is_valid_syscall) {
    // Use num to lookup the system call function for num, call it,
    // and store its return value in p->trapframe->a0
    p->trapframe->a0 = syscalls[num]();
    #ifdef CONFIG_DEBUG
    log("syscall done pid=%d num=%d ret=0x%p\n", p->pid, num, p->trapframe->a0);
    #endif
  } else {
    #ifdef CONFIG_DEBUG
    log("%d %s: unknown system call %d\n", p->pid, p->name, num);
    #endif
    p->trapframe->a0 = -1;
  }
}
