#include <types.h>
#include <param.h>
#include <spinlock.h>
#include <macro.h>
#include <stdio.h>
#include <list.h>
#include <proc.h>
#include <defs.h>
#include <vm.h>
#include <vfs.h>
#include <syscall.h>

static bool valid_user_arg(uint64_t addr) {
  struct proc *p = cur_proc();
  if (p == NULL || p->pagetable == 0) {
    #ifdef CONFIG_DEBUG
    log("valid_user_arg: bad proc/pagetable p=%p pagetable=%p addr=0x%p\n",
        p, p ? p->pagetable : 0, addr);
    #endif
    return false;
  }
  if (walk(p->pagetable, addr, 0) == NULL)
    return false;
  else
    return true;
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

uint64_t sys_create(void) {
  uint64_t path, mode;
  argaddr(&path, 0);
  argint(&mode, 1);

  if (!valid_user_arg(path)) {
    #ifdef CONFIG_DEBUG
    log("invalid user arg addr: 0x%08x\n", path);
    #endif
    return -1;
  }

  char filename[MAX_FILE_NAME_LEN];
  if (transstr(path, filename, MAX_FILE_NAME_LEN) < 0)
    return -1;
  if (filesys_create(cur_proc(), filename, mode))
    return -1;
  else
    return 0;
}

uint64_t sys_open(void) {
  uint64_t path, flags;
  argaddr(&path, 0);
  argint(&flags, 1);

  if (!valid_user_arg(path)) {
    #ifdef CONFIG_DEBUG
    log("invalid user arg addr: 0x%08x\n", path);
    #endif
    return -1;
  } else {
    char filename[MAX_FILE_NAME_LEN];
    if (transstr(path, filename, MAX_FILE_NAME_LEN) < 0)
      return -1;
    return filesys_open(cur_proc(), filename, flags);
  }
}

uint64_t sys_write(void) {
  uint64_t fd;
  uint64_t addr;
  uint64_t size;
  argint(&fd, 0);
  argaddr(&addr, 1);
  argint(&size, 2);
  if (!valid_user_arg(addr)) {
    #ifdef CONFIG_DEBUG
    log("invalid user arg addr: 0x%08x\n", addr);
    #endif
    return -1;
  } else {
    const uint64_t buflen = 1024;
    char *buf = kalloc(buflen, DEFAULT);
    if (buf == NULL)
      return -1;
    uint64_t total = 0;
    while (total < size) {
      uint64_t n = size - total;
      if (n > buflen - 1) n = buflen - 1;
      if (copyin(cur_proc()->pagetable, buf, addr + total, n) < 0) {
        kfree(buf, DEFAULT);
        return -1;
      }
      
      if (fd == STDOUT || fd == STDERR) {
        buf[n] = '\0';
        printf("%s", buf);
      } else {
        if (filesys_write(cur_proc(), fd, buf, n) != n) {
          kfree(buf, DEFAULT);
          return -1;
        }
      }
      total += n;
    }
    kfree(buf, DEFAULT);
    return total;
  }
}

uint64_t sys_read(void) {
  uint64_t fd;
  uint64_t addr;
  uint64_t size;
  argint(&fd, 0);
  argaddr(&addr, 1);
  argint(&size, 2);
  #ifdef CONFIG_DEBUG
  struct proc *p = cur_proc();
  log("sys_read pid=%d fd=%d addr=0x%p size=%d pagetable=%p\n",
      p ? p->pid : -1, fd, addr, size, p ? p->pagetable : 0);
  #endif

  if (!valid_user_arg(addr)) {
    #ifdef CONFIG_DEBUG
    log("invalid user arg addr: 0x%08x\n", addr);
    #endif
    return -1;
  } else {
    const uint64_t buflen = 1024;
    char *buf = kalloc(buflen, DEFAULT);
    if (buf == NULL)
      return -1;
    uint64_t total = 0;
    while (total < size) {
      uint64_t n = size - total;
      if (n > buflen) n = buflen;
      uint64_t nread = filesys_read(cur_proc(), fd, buf, n);
      if (nread <= 0) break;
      if (copyout(cur_proc()->pagetable, addr + total, buf, nread) < 0) {
        kfree(buf, DEFAULT);
        return -1;
      }
      total += nread;
      if (nread < n) break; // EOF or less than requested
    }
    kfree(buf, DEFAULT);
    return total;
  }
}

uint64_t sys_close(void) {
  uint64_t fd;
  argint(&fd, 0);

  if (fd < 2)
    return -1;
  else
    return filesys_close(cur_proc(), fd);
}

// uint64_t sys_chdir(void) {
//   uint64_t path;
//   argaddr(&path, 0);
//
//   if (!valid_user_arg(path)) {
#ifdef CONFIG_DEBUG
//     log("invalid user arg addr: 0x%08x\n", addr);
#endif
//     return -1;
//   } else {
//     struct proc *p = cur_proc();
//     char buf[size+1];
//     transtr(addr, buf, size, true);
//     struct dirent *old_cwd = p->cwd;
//     p->cwd = filesys_link(p, buf, buf, old_cwd->flag);
//     filesys_unlink(p, old_cwd);
//     return 0;
//   }
// }
