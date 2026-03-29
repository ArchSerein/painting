#include <types.h>
#include <param.h>
#include <spinlock.h>
#include <riscv.h>
#include <list.h>
#include <proc.h>
#include <console.h>
#include <sbi.h>
#include <memlayout.h>
#include <macro.h>
#include <stdio.h>
#include <vm.h>
#include <schedule.h>
#include <defs.h>

#define BACKSPACE (0x100)
#define UART_RHR 0
#define UART_THR 0
#define UART_IER 1
#define UART_FCR 2
#define UART_LCR 3
#define UART_LSR 5

#define UART_IER_RX_ENABLE 0x01
#define UART_IER_TX_ENABLE 0x02
#define UART_LCR_BAUD_LATCH 0x80
#define UART_LSR_RX_READY 0x01
#define UART_LSR_TX_IDLE 0x20

static inline uint8_t uart_read_reg(int reg) {
  return *(volatile uint8_t *)(UART0 + reg);
}

static inline void uart_write_reg(int reg, uint8_t v) {
  *(volatile uint8_t *)(UART0 + reg) = v;
}

struct spinlock cons;
void console_init(void) {
  initlock(&cons, "cons");
  // 16550 init (same layout as QEMU virt UART)
  uart_write_reg(UART_IER, 0x00);
  uart_write_reg(UART_LCR, UART_LCR_BAUD_LATCH);
  uart_write_reg(0, 0x03);
  uart_write_reg(1, 0x00);
  uart_write_reg(UART_LCR, 0x03);
  uart_write_reg(UART_FCR, 0x07);
  uart_write_reg(UART_IER, UART_IER_RX_ENABLE | UART_IER_TX_ENABLE);
}

void console_putc(int ch) {
  if (ch == BACKSPACE) {
    // 处理退格
    while ((uart_read_reg(UART_LSR) & UART_LSR_TX_IDLE) == 0) {}
    uart_write_reg(UART_THR, '\b');
    while ((uart_read_reg(UART_LSR) & UART_LSR_TX_IDLE) == 0) {}
    uart_write_reg(UART_THR, ' ');
    while ((uart_read_reg(UART_LSR) & UART_LSR_TX_IDLE) == 0) {}
    uart_write_reg(UART_THR, '\b');
  }
  else {
    while ((uart_read_reg(UART_LSR) & UART_LSR_TX_IDLE) == 0) {}
    uart_write_reg(UART_THR, ch);
  }
}

int console_read(int user_dst, uint64_t dst, int n) {
  int i;
  for (i = 0; i < n; i++) {
    int c;
    while ((uart_read_reg(UART_LSR) & UART_LSR_RX_READY) == 0) {
      yield();
    }
    c = uart_read_reg(UART_RHR);
    if (c == 0x04) { // EOF (Ctrl-D)
      if (i == 0) return 0;
      break;
    }
    char ch = (char)c;
    if (user_dst) {
      if (copyout(cur_proc()->pagetable, dst + i, &ch, 1) < 0)
        break;
    } else {
      ((char *)dst)[i] = ch;
    }
    if (ch == '\n' || ch == '\r') {
       if (ch == '\r') {
         // Echo \n for \r
         console_putc('\n');
       } else {
         console_putc(ch);
       }
       i++;
       break;
    }
    // Echo the character
    console_putc(ch);
  }
  return i;
}

int console_write(int user_dst, uint64_t src, int n) {
  int i;
  for (i = 0; i < n; i++) {
    char ch;
    if (user_dst) {
      if (copyin(cur_proc()->pagetable, &ch, src + i, 1) < 0)
        break;
    } else {
      ch = ((char *)src)[i];
    }
    console_putc(ch);
  }
  return i;
}

void console_intr(int c) {
  // Not used yet, since we are polling in console_read
}
