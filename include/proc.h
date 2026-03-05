#ifndef PROC_H__
#define PROC_H__

enum process_state {
  ZIMBIE,
  INITIAL,
  RUNNABLE,
  BLOCK,
  RUNNING,
  SLEEPING
};

enum process_priority { FIRST, SECOND, THIRD };

// 上下文切换时, 只需要保存 callee 寄存器
// 在 riscv 的调用约定规定除了 callee 之外的寄存器的值允许被破坏
struct context {
  uint64_t ra;
  uint64_t sp;

  // callee-saved
  uint64_t s0;
  uint64_t s1;
  uint64_t s2;
  uint64_t s3;
  uint64_t s4;
  uint64_t s5;
  uint64_t s6;
  uint64_t s7;
  uint64_t s8;
  uint64_t s9;
  uint64_t s10;
  uint64_t s11;
};

// 异常处理的过程并不是函数调用
// 所以需要保存所有寄存器的状态
struct trapframe {
  /*   0 */ uint64_t kernel_satp;   // kernel page table
  /*   8 */ uint64_t kernel_sp;     // top of process's kernel stack
  /*  16 */ uint64_t kernel_trap;   // usertrap()
  /*  24 */ uint64_t epc;           // saved user program counter
  /*  32 */ uint64_t kernel_hartid; // saved kernel tp
  /*  40 */ uint64_t ra;
  /*  48 */ uint64_t sp;
  /*  56 */ uint64_t gp;
  /*  64 */ uint64_t tp;
  /*  72 */ uint64_t t0;
  /*  80 */ uint64_t t1;
  /*  88 */ uint64_t t2;
  /*  96 */ uint64_t s0;
  /* 104 */ uint64_t s1;
  /* 112 */ uint64_t a0;
  /* 120 */ uint64_t a1;
  /* 128 */ uint64_t a2;
  /* 136 */ uint64_t a3;
  /* 144 */ uint64_t a4;
  /* 152 */ uint64_t a5;
  /* 160 */ uint64_t a6;
  /* 168 */ uint64_t a7;
  /* 176 */ uint64_t s2;
  /* 184 */ uint64_t s3;
  /* 192 */ uint64_t s4;
  /* 200 */ uint64_t s5;
  /* 208 */ uint64_t s6;
  /* 216 */ uint64_t s7;
  /* 224 */ uint64_t s8;
  /* 232 */ uint64_t s9;
  /* 240 */ uint64_t s10;
  /* 248 */ uint64_t s11;
  /* 256 */ uint64_t t3;
  /* 264 */ uint64_t t4;
  /* 272 */ uint64_t t5;
  /* 280 */ uint64_t t6;
};

struct chidl_entry {
  int               exit_state;
  struct list_elem  elem;
  bool              is_waiting;
  pid_t             pid;
};

struct proc {
  struct spinlock     lock;
  enum  process_state status;
  pid_t               pid;
  uint64_t            kstack;
  struct trapframe *  trapframe;
  struct list         child_list;
  struct list         file_list;
  struct list_elem    elem;
  int                 priority;
  struct proc *       parent;
  uint64_t            vruntime;
  pagetable_t         pagetable;
  struct context      context;
  void *              chan;         // sleep channel
  struct file *       cwd;
  uint64_t            heap_start;
  uint64_t            heap_end;
  int                 exit_state;   // exit status for wait()

  // debug
  char                name[MAXLEN];
};

// Structures used to describe cpu information, recording information about
// the proc currently running on the cpu, information about context switches,
// and interrupt status and number of nested levels.
struct cpu {
  struct proc *proc;
  struct context context;

  int noff;   // depth of push_off
  int intena; // interrupt enable status
};

#endif //!PROC_H__
