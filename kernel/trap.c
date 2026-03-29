#include <types.h>
#include <param.h>
#include <spinlock.h>
#include <memlayout.h>
#include <riscv.h>
#include <trap.h>
#include <list.h>
#include <proc.h>
#include <macro.h>
#include <defs.h>
#include <schedule.h>
#include <stdio.h>
#include <timer.h>
#include <plic.h>
#include <virt.h>
#include <syscall.h>

uint32_t ticks;
struct spinlock tickslock;

extern char trampoline[], userret[], uservec[];
// 在 kernelvec.S 中
void    kernelvec(void);

void trapinit(void) {
  initlock(&tickslock, "time");
}

void trapinithart(void) {
  // 将异常的向量地址写入 stvec 寄存器
  w_stvec((uint64_t)kernelvec);
}

int dev_intr(void) {
  uint64_t scause = r_scause();

  if (scause == 0x8000000000000009L) {
    int irq = plic_claim();
    if (irq == VIRTIO0_IRQ)
      virtio_disk_intr();
    else if (irq == 0) {
      // ignore
    }
    else {
      printf("unexpected interrupt irq=%d\n", irq);
      return 0;
    }
    plic_complete(irq);
    return 1;
  } else if (scause == 0x8000000000000005L) {
    clock_intr();
    if (r_tp() == 0) {
      acquire(&tickslock);
      ++ticks;
      release(&tickslock);
    }
    return 2;
  } else {
    return 0;
  }
}

void kerneltrap(void) {
  // 处理时钟中断和设备中断
  // 发生中断时, SPIE 被设置为 SIE, SIE 被设置为 0
  // Determine if the interrupt source is from S mode
  uint64_t  sepc = r_sepc();
  uint64_t  sstatus = r_sstatus();
  if ((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap->the interrupt source is not from S mode");
  if (intr_get() != 0)
    panic("kerneltrap->interrupt enabled");

  int which_dev = 0;
  which_dev = dev_intr();
  if (which_dev == 0) {
    #ifdef CONFIG_DEBUG
    log("on cpu %d, unknown source: scause: %08x, sepc: %08x, stval: %08x\n", r_tp(), r_scause(), r_sepc(), r_stval());
    #endif
    panic("kerneltrap");
  }

  // 如果是时钟中断
  // 当前运行的进程就会放弃 cpu 资源
  if (which_dev == 2 && cur_proc() != NULL) {
    cur_proc()->vruntime += 100;
    yield();
  }

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;
  struct proc *p = cur_proc();

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64_t)kernelvec);

  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    // if(killed(p))
    //   exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();
  } else if((which_dev = dev_intr()) != 0){
    // ok
  } else {
    printf("usertrap(): unexpected scause 0x%x pid=%d\n", r_scause(), p->pid);
    printf("            sepc=0x%x stval=0x%x\n", r_sepc(), r_stval());
    // setkilled(p);
  }

  // if(killed(p))
  //   exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && cur_proc() != NULL) {
    cur_proc()->vruntime += 100;
    yield();
  }

  usertrapret();
}

void usertrapret(void) {
  struct proc *p = cur_proc();

  intr_off();

  uint64_t  trampoline_user_vec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_user_vec);

  p->trapframe->kernel_satp = r_satp();
  p->trapframe->kernel_sp = p->kstack + PGSIZE;
  p->trapframe->kernel_trap = (uint64_t)usertrap;
  p->trapframe->kernel_hartid = r_tp();

  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP;
  x |= SSTATUS_SPIE;
  w_sstatus(x);

  w_sepc(p->trapframe->epc);
  #ifdef CONFIG_DEBUG
  log("usertrapret pid=%d epc=0x%p sp=0x%p satp=0x%p\n",
      p->pid, p->trapframe->epc, p->trapframe->sp, p->pagetable);
  #endif

  uint64_t satp = MAKE_SATP(p->pagetable);

  uint64_t trampoline_user_ret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64_t))trampoline_user_ret)(satp);
}
