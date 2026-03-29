#include <param.h>
#include <types.h>
#include <memlayout.h>
#include <list.h>
#include <spinlock.h>
#include <riscv.h>
#include <proc.h>
#include <macro.h>
#include <defs.h>
#include <uvm.h>
#include <vm.h>
#include <pm.h>
#include <string.h>
#include <trap.h>
#include <stdio.h>
#include <file.h>
#include <buddy.h>
#include <schedule.h>
#include <sbi.h>
#include <vfs.h>
#include <dirent.h>
#include <fat32.h>
#include <fatfs.h>

extern char trampoline[];
void swtch(struct context *, struct context *);
struct cpu cpus[NCPU];
unsigned char pid_bitmap[MAX_PID / 8]; // 位图，每个字节包含8个位

static struct spinlock process_lock;
static struct list process_list;
static struct spinlock  pid_lock;

static int _flag2perm(int flag) {
  int perm = 0;
  if (flag & 0x1) perm |= PTE_X;
  if (flag & 0x2) perm |= PTE_W;
  return perm;
}

static void free_user_pagetable(pagetable_t pagetable) {
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, 0);
}

static uint64_t loadseg(pagetable_t pagetable, int fd, off_t off, uint64_t va, uint64_t filesz, uint64_t memsz,
                        int flag, uint64_t oldsz) {
  uint64_t newsz = va + memsz;
  uint64_t pa, n;
  struct proc *p = cur_proc();

  if ((newsz = uvmalloc(pagetable, oldsz, newsz, flag)) == 0) {
    return 0;
  }

  filesys_seek(p, fd, off, SEEK_SET);
  for (uint64_t i = 0; i < filesz; i += PGSIZE) {
    pa = walkaddr(pagetable, va + i);
    if (pa == 0)
      return 0;
    n = filesz - i;
    if (n > PGSIZE)
      n = PGSIZE;
    if (filesys_read(p, fd, (char *)pa, n) != n)
      return 0;
  }

  for (uint64_t bss = va + filesz, left = memsz - filesz; left > 0; bss += n, left -= n) {
    uint64_t page = PGROUNDDOWN(bss);
    pa = walkaddr(pagetable, page);
    if (pa == 0)
      return 0;
    uint64_t off_in_page = bss - page;
    n = PGSIZE - off_in_page;
    if (n > left)
      n = left;
    memset((void *)(pa + off_in_page), 0, n);
  }

  return newsz;
}

static bool setup_stack(pagetable_t pagetable, uint64_t *sp) {
  void *pa = kpmalloc();
  if (pa == NULL) {
    #ifdef CONFIG_DEBUG
    log("kpmalloc failed\n");
    #endif
    return false;
  }
  if (mappages(pagetable, USTACK - PGSIZE, (uint64_t)pa, PGSIZE, PTE_R | PTE_W | PTE_U) < 0) {
    kpmfree(pa);
    return false;
  }
  *sp = USTACK;
  return true;
}

struct cpu *cur_cpu(void) {
  uint64_t hartid = r_tp();
  return &cpus[hartid];
}

struct proc* cur_proc(void) {
  struct cpu* c = cur_cpu();
  return c->proc;
}

int getpid() {
  return cur_proc()->pid;
}

pagetable_t process_pagetable(struct proc *p) {
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, (uint64_t)trampoline, PGSIZE, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, (uint64_t)(p->trapframe), PGSIZE, PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

void process_init(void) {
  // 初始化保存 proc 的链表
  list_init(&process_list);
  // 初始化进程链表的锁
  initlock(&process_lock, "process_lock");
  // 初始化进程号的🔒
  initlock(&pid_lock, "pid_lock");
}

// free pid
void pid_free(pid_t pid) {
  acquire(&pid_lock);
  if (pid >= 0 && pid < MAX_PID) {
      pid_bitmap[pid / 8] &= ~(1 << (pid % 8)); // 释放该PID号，将该位设为0
  }
  release(&pid_lock);
}

// alloc pid
pid_t pid_alloc(void) {
  acquire(&pid_lock);
  for (pid_t i = 0; i < MAX_PID; i++) {
      if ((pid_bitmap[i / 8] & (1 << (i % 8))) == 0) {
          pid_bitmap[i / 8] |= (1 << (i % 8)); // 设置该位为1，表示已分配
          release(&pid_lock);
          return i;
      }
  }
  release(&pid_lock);
  return -1; // 没有可用的PID
}

struct proc *process_create(void) {
  // create a process
  // first alloc a page for save the information of process
  struct proc *p = (struct proc *)kalloc(sizeof(struct proc), PROC_MODE);
  if (p == NULL) {
    return NULL;
  }
  p->trapframe = kpmalloc();
  if (p->trapframe == NULL) {
    kfree(p, PROC_MODE);
    return NULL;
  }

  // alloc kernel stack
  uintptr_t *kstack = (uintptr_t *)kpmalloc();
  if (kstack == NULL) {
    kpmfree(p);
    return NULL;
  }
  p->pid      = pid_alloc();

  uint64_t pa = (uint64_t)kstack;
  uint64_t va = KSTACK(p->pid);
  if (mappages(kernel_pagetable, va, pa, PGSIZE, PTE_R | PTE_W) != 0) {
    kpmfree(kstack);
    kpmfree(p);
    return NULL;
  }
  p->kstack = va;
  // create a user page for the giving process
  p->pagetable = process_pagetable(p);
  // Set up new context to start executing at usertrapret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64_t)usertrapret;
  p->context.sp = p->kstack + PGSIZE;

  // Parameters to initialize the process structure
  p->vruntime = 0;
  p->priority = FIRST;
  p->status   = INITIAL;
  p->cwd      = NULL;
 
  p->parent = cur_proc();     // first proc should be NULL
  list_init(&p->child_list);
  list_init(&p->file_list);
  p->in_run_queue = false;

  acquire(&process_lock);
  list_push_back(&process_list, &p->elem);
  release(&process_lock);

  initlock(&p->lock, "proc");

  return p;
}

void forkret(void) {
  release(&cur_proc()->lock);
  usertrapret();
}

void run_first_task(void) {
  release(&cur_proc()->lock);
  #ifdef CONFIG_DEBUG
  log("entry first task\n");
  #endif
  ASSERT(intr_get());
  filesys_init();
  extern void fat32Test();
  fat32Test();
  const char *argv[] = {"init", NULL};
  if (process_execute("init", argv)) {
    #ifdef CONFIG_DEBUG
    log("run_first_task jump to user pid=%d epc=0x%p sp=0x%p\n",
        cur_proc()->pid, cur_proc()->trapframe->epc, cur_proc()->trapframe->sp);
    #endif
    usertrapret();
  }
  sbi_shutdown();
}
bool process_execute(const char *_path, const char *argv[]) {
  uint64_t ustack[MAXARG];
  struct proc *p = cur_proc();
  pagetable_t old_pagetable = p->pagetable;
  struct trapframe old_trapframe;
  uint64_t old_heap_start = p->heap_start;
  uint64_t old_heap_end = p->heap_end;
  memcpy(&old_trapframe, p->trapframe, sizeof(old_trapframe));

  pagetable_t new_pagetable = process_pagetable(p);
  if (new_pagetable == NULL)
    return false;

  p->pagetable = new_pagetable;
  #ifdef CONFIG_DEBUG
  log("exec begin pid=%d path=%s\n", p->pid, _path);
  #endif
  if (!loader(_path))
    goto bad;

  int argc;
  uint64_t sp = p->trapframe->sp;
  uint64_t stackbase = sp - PGSIZE;
  for (argc = 0; argv[argc]; argc++) {
    if (argc >= MAXARG)
      goto bad;
    size_t len = strlen(argv[argc]) + 1;
    sp -= len;
    if (sp < stackbase)
      goto bad;
    if (copyout(p->pagetable, sp, argv[argc], len) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc++] = 0;

  uint64_t ptr_size = sizeof(uint64_t);
  uint64_t align_len = (sp & 0x0f) + (0x10 - (argc * ptr_size & 0xf));
  sp -= align_len;
  sp -= argc * ptr_size;
  if (sp < stackbase)
    goto bad;
  if (copyout(p->pagetable, sp, (char *)ustack, argc * ptr_size) < 0)
    goto bad;

  p->trapframe->a1 = sp;
  p->trapframe->a0 = argc - 1;
  p->trapframe->sp = sp;
  strncpy(p->name, _path, sizeof(p->name) - 1);
  p->name[sizeof(p->name) - 1] = '\0';
  #ifdef CONFIG_DEBUG
  log("exec success pid=%d path=%s epc=0x%p sp=0x%p\n", p->pid, _path, p->trapframe->epc, p->trapframe->sp);
  #endif
  free_user_pagetable(old_pagetable);
  return true;

bad:
  #ifdef CONFIG_DEBUG
  log("exec fail pid=%d path=%s\n", p->pid, _path);
  #endif
  free_user_pagetable(new_pagetable);
  p->pagetable = old_pagetable;
  memcpy(p->trapframe, &old_trapframe, sizeof(old_trapframe));
  p->heap_start = old_heap_start;
  p->heap_end = old_heap_end;
  return false;
}

void process_exit(uint64_t state) {
  struct proc *p = cur_proc();

  // Close all open files
  struct list_elem *e;
  while (!list_empty(&p->file_list)) {
    e = list_pop_front(&p->file_list);
    struct file *f = list_entry(e, struct file, elem);
    free_fd(f->fd);
    file_close(f->dirent);
    kfree(f, FILE_MODE);
  }

  // Close current working directory
  if (p->cwd) {
    file_close(p->cwd->dirent);
    kfree(p->cwd, FILE_MODE);
    p->cwd = NULL;
  }

  // Free user memory (page table)
  if (p->pagetable) {
    uvmunmap(p->pagetable, TRAMPOLINE, 1, 0);
    uvmunmap(p->pagetable, TRAPFRAME, 1, 0);
    uvmfree(p->pagetable, 0);
    p->pagetable = NULL;
  }

  // Acquire process lock for state change
  acquire(&p->lock);

  // Set exit state and change status to ZIMBIE (zombie)
  p->exit_state = state;
  p->status = ZIMBIE;

  // Wake up parent if it's waiting
  if (p->parent) {
    wakeup(p->parent);
  }

  // Jump into the scheduler, never to return
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its PID.
// If pid == 0, wait for any child.
// If pid != 0, wait for that specific child.
// Returns -1 if no matching child exists.
pid_t process_wait(pid_t pid) {
  struct proc *p = cur_proc();
  struct list_elem *e;
  struct proc *child;
  int have_kids;

  acquire(&process_lock);
  for (;;) {
    have_kids = 0;
    for (e = list_begin(&process_list); e != list_end(&process_list);
         e = list_next(e)) {
      child = list_entry(e, struct proc, elem);
      if (child->parent == p) {
        // If pid != 0, only wait for that specific child
        if (pid != 0 && child->pid != pid) {
          continue;
        }
        acquire(&child->lock);
        if (child->status == ZIMBIE) {
          // Found a zombie child, clean it up
          pid_t child_pid = child->pid;
          // Free child's kernel stack
          if (child->kstack) {
            uvmunmap(kernel_pagetable, child->kstack, 1, 1);
            child->kstack = 0;
          }
          // Free child's trapframe
          if (child->trapframe) {
            kpmfree(child->trapframe);
            child->trapframe = NULL;
          }
          // Remove child from process list
          list_remove(&child->elem);
          // Free child's pid
          pid_free(child->pid);
          release(&child->lock);
          // Free child's proc structure
          kfree(child, PROC_MODE);
          release(&process_lock);
          return child_pid;
        }
        release(&child->lock);
        have_kids = 1;
      }
    }

    if (!have_kids) {
      release(&process_lock);
      return -1;
    }

    // If waiting for specific pid that exists but isn't zombie yet
    if (pid != 0 && !have_kids) {
      release(&process_lock);
      return -1;
    }

    // Wait for child to exit
    sleep(p, &process_lock);
  }
}

pid_t fork(void) {
  pid_t pid;
  struct proc *np, *p;
  pte_t *spte;
  char *stack_mem = NULL;
  uint64_t stack_va = USTACK - PGSIZE;

  p = cur_proc();
  if ((np = process_create()) == NULL) {
    return -1;
  }

  // copy on write
  if (uvmcopy(p->pagetable, np->pagetable, p->heap_start) < 0) {
    goto bad;
  }
  // user stack is fixed at a high virtual address and not covered by [0, heap_start)
  spte = walk(p->pagetable, stack_va, 0);
  if (spte == NULL || (*spte & (PTE_V | PTE_U)) != (PTE_V | PTE_U))
    goto bad;
  stack_mem = kpmalloc();
  if (stack_mem == NULL)
    goto bad;
  memmove(stack_mem, (char *)PTE2PA(*spte), PGSIZE);
  if (mappages(np->pagetable, stack_va, (uint64_t)stack_mem, PGSIZE,
               PTE_FLAGS(*spte) & (PTE_R | PTE_W | PTE_X | PTE_U)) != 0) {
    kpmfree(stack_mem);
    stack_mem = NULL;
    goto bad;
  }

  // Set return point for child to forkret
  np->context.ra = (uint64_t)forkret;
  np->context.sp = np->kstack + PGSIZE;

  //copy user register
  memcpy((void *)np->trapframe, (void *)p->trapframe, sizeof(struct trapframe));
  #ifdef CONFIG_DEBUG
  log("fork child pid=%d parent=%d child_a0=0x%p child_a1=0x%p child_a7=0x%p child_epc=0x%p\n",
      np->pid, p->pid, np->trapframe->a0, np->trapframe->a1, np->trapframe->a7, np->trapframe->epc);
  #endif
  // set the child process return value is zero
  np->trapframe->a0 = 0;
  np->heap_start = p->heap_start;
  np->heap_end = p->heap_end;

  // copy open file descriptor
  // np->file_list = p->file_list;
  list_init(&np->file_list);
  struct list_elem *e;
  for (e = list_begin(&p->file_list); e != list_end(&p->file_list); e = list_next(e)) {
    struct file *f = list_entry(e, struct file, elem);
    struct file *nf = kalloc(sizeof(struct file), FILE_MODE);
    memcpy(nf, f, sizeof(struct file));
    if (nf->dirent) {
      nf->dirent->linkcnt++;
    }
    list_push_back(&np->file_list, &nf->elem);
  }

  strncpy(np->name, p->name, sizeof(p->name));
  pid = np->pid;

  acquire(&np->lock);
  np->status = RUNNABLE;
  np->parent = p;
  rb_push_back(np);
  release(&np->lock);

  return pid;

bad:
  if (stack_mem != NULL)
    kpmfree(stack_mem);
  if (np->kstack)
    uvmunmap(kernel_pagetable, KSTACK(np->pid), 1, 1);
  if (np->pagetable)
    free_user_pagetable(np->pagetable);
  if (np->trapframe)
    kpmfree(np->trapframe);
  acquire(&process_lock);
  list_remove(&np->elem);
  release(&process_lock);
  pid_free(np->pid);
  kfree(np, PROC_MODE);
  return -1;
}

void init_first_proc(void) {
  struct proc *p = process_create();
  p->context.ra = (uint64_t)run_first_task;
  p->trapframe->a0 = 0;
  p->trapframe->sp = PGSIZE;
  p->trapframe->epc = 0;
  acquire(&p->lock);
  p->status = RUNNABLE;
  rb_push_back(p);
  release(&p->lock);
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = cur_proc();
  struct cpu *c = cur_cpu();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(cur_cpu()->noff != 1)
    panic("sched locks");
  if(p->status == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = cur_cpu()->intena;
  swtch(&p->context, &c->context);
  cur_cpu()->intena = intena;
}

void sleep(void *chan, struct spinlock *lk) {
  struct proc *p = cur_proc();

  acquire(&p->lock);
  release(lk);

  p->chan = chan;
  p->status = SLEEPING;
  sched();
  p->chan = NULL;

  acquire(lk);
  release(&p->lock);
}

void wakeup(void *chan) {
  struct proc *cur = cur_proc();

  // 需要唤醒所有睡在 chan 上的进程
  struct list_elem *e;
  for (e = list_begin(&process_list); e != list_end(&process_list); e = list_next(e)) {
    struct proc *p = list_entry(e, struct proc, elem);
    acquire(&p->lock);
    if (p != cur && p->status == SLEEPING && p->chan == chan) {
      p->status = RUNNABLE;
      rb_push_back(p);
    }
    release(&p->lock);
  }
}

// programe loader
// Format of an ELF executable file
#define ELF_MAGIC 0x464C457FU

// File header
struct elfhdr {
  uint32_t magic;  // must equal ELF_MAGIC
  uint8_t  elf[12];
  uint16_t type;
  uint16_t machine;
  uint32_t version;
  uint64_t entry;
  uint64_t phoff;
  uint64_t shoff;
  uint32_t flags;
  uint16_t ehsize;
  uint16_t phentsize;
  uint16_t phnum;
  uint16_t shentsize;
  uint16_t shnum;
  uint16_t shstrndx;
} __packed;

// Program section header
struct proghdr {
  uint32_t type;
  uint32_t flags;
  uint64_t off;
  uint64_t vaddr;
  uint64_t paddr;
  uint64_t filesz;
  uint64_t memsz;
  uint64_t align;
} __packed;

// Values for Proghdr type
#define PT_LOAD           1

// Flag bits for Proghdr flags
#define ELF_PROG_FLAG_EXEC      1
#define ELF_PROG_FLAG_WRITE     2
#define ELF_PROG_FLAG_READ      4

bool loader(const char *file) {
  bool success = false;
  uint64_t  sz = 0;
  struct proc *p = cur_proc();
  #ifdef CONFIG_DEBUG
  log("loader begin pid=%d file=%s\n", p->pid, file);
  #endif
  char path[strlen(file) + 1];
  strncpy(path, file, strlen(file));
  path[strlen(file)] = '\0';
  int fd = filesys_open(p, path, 0);
  // 如果打开文件失败就直接返回
  // 通过返回值表示程序是否加载成功
  if (fd < 0)
    return false;
  #ifdef CONFIG_DEBUG
  log("loader open ok pid=%d fd=%d\n", p->pid, fd);
  #endif

  // 然后读取程序头部信息
  struct elfhdr ehdr;
  ASSERT(filesys_read(p, fd, (char *)&ehdr, sizeof(struct elfhdr)) == sizeof(struct elfhdr));
  #ifdef CONFIG_DEBUG
  log("loader read ehdr ok pid=%d phnum=%d entry=0x%p\n", p->pid, ehdr.phnum, ehdr.entry);
  #endif

  #if defined(__ISA_AM_NATIVE__)
  # define EXPECT_TYPE EM_X86_64
  #elif defined(__ISA_X86__)
  # define EXPECT_TYPE EM_386
  #elif defined(__ISA_RISCV32__) || defined(__ISA_RISCV64__)
  # define EXPECT_TYPE EM_RISCV
  #elif defined(__ISA_MIPS32__)
  # define EXPECT_TYPE EM_MIPS
  #else
  # error Unsupported ISA
  #endif

  ASSERT(ehdr.magic == ELF_MAGIC);
  ASSERT(ehdr.machine == EXPECT_TYPE);

  struct proghdr phdr[ehdr.phnum];
  filesys_seek(p, fd, ehdr.phoff, SEEK_SET);
  size_t bytes_read = filesys_read(p, fd, (char *)phdr, sizeof (struct proghdr) * ehdr.phnum);
  ASSERT(bytes_read == sizeof(struct proghdr) * ehdr.phnum);
  #ifdef CONFIG_DEBUG
  log("loader read phdr ok pid=%d bytes=%d\n", p->pid, bytes_read);
  #endif

  for (int i = 0; i < ehdr.phnum; i++) {
    if(phdr[i].type != PT_LOAD)
      continue;
    if(phdr[i].memsz < phdr[i].filesz)
      goto done;
    if(phdr[i].vaddr + phdr[i].memsz < phdr[i].vaddr)
      goto done;
    if ((phdr[i].vaddr & PGMASK) != 0)
      goto done;

    if ((sz = loadseg(p->pagetable, fd, phdr[i].off, phdr[i].vaddr, phdr[i].filesz,
                      phdr[i].memsz, _flag2perm(phdr[i].flags), sz)) == 0)
      goto done;
    #ifdef CONFIG_DEBUG
    log("loader seg ok pid=%d idx=%d va=0x%p filesz=%d memsz=%d\n",
          p->pid,
          i, phdr[i].vaddr, phdr[i].filesz, phdr[i].memsz);
    #endif
  }

  p->heap_start = PGROUNDUP(sz);
  p->heap_end   = PGROUNDUP(sz);
  if (!setup_stack(p->pagetable, &p->trapframe->sp))
    goto done;
  p->trapframe->epc = ehdr.entry;
  success = true;
  #ifdef CONFIG_DEBUG
  log("loader success pid=%d epc=0x%p sp=0x%p heap=0x%p\n",
      p->pid, p->trapframe->epc, p->trapframe->sp, p->heap_start);
  #endif

done:
  if (!success)
    {
    #ifdef CONFIG_DEBUG
    log("loader fail pid=%d\n", p->pid);
    #endif
    }
  filesys_close(p, fd);
  return success;
}
