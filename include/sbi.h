#ifndef SBI_H__
#define SBI_H__
#include <sbi_ecall_interface.h>


struct sbiret {
  long  error;
  long  value;
};

// ext save into a7 register as extension id
// func save into a6 register as function id
static struct sbiret SBI_CALL(int ext, int fid, unsigned long arg0,
            unsigned long arg1, unsigned long arg2,
            unsigned long arg3, unsigned long arg4,
            unsigned long arg5)
{
    struct sbiret ret;

    register uintptr_t a0 asm ("a0") = (uintptr_t)(arg0);
    register uintptr_t a1 asm ("a1") = (uintptr_t)(arg1);
    register uintptr_t a2 asm ("a2") = (uintptr_t)(arg2);
    register uintptr_t a3 asm ("a3") = (uintptr_t)(arg3);
    register uintptr_t a4 asm ("a4") = (uintptr_t)(arg4);
    register uintptr_t a5 asm ("a5") = (uintptr_t)(arg5);
    register uintptr_t a6 asm ("a6") = (uintptr_t)(fid);
    register uintptr_t a7 asm ("a7") = (uintptr_t)(ext);
    asm volatile ("ecall"
              : "+r" (a0), "+r" (a1)
              : "r" (a2), "r" (a3), "r" (a4), "r" (a5), "r" (a6), "r" (a7)
              : "memory");
    ret.error = a0;
    ret.value = a1;

    return ret;
}

static struct sbiret SBI_CALL_0(unsigned long ext, unsigned long funct) {
  return SBI_CALL(ext, funct, 0, 0, 0, 0, 0, 0);
}

static struct sbiret SBI_CALL_1(unsigned long ext, unsigned long funct, unsigned long args0) {
  return SBI_CALL(ext, funct, args0, 0, 0, 0, 0, 0);
}

/*static struct sbiret SBI_CALL_2(unsigned long ext, unsigned long funct, unsigned long args0,*/
/*                                unsigned long args1) {*/
/*  return SBI_CALL(ext, funct, args0, args1, 0, 0, 0, 0);*/
/*}*/

static struct sbiret SBI_CALL_3(unsigned long ext, unsigned long funct, unsigned long args0, unsigned long args1, unsigned long args2) {
  return SBI_CALL(ext, funct, args0, args1, args2, 0, 0, 0);
}

/*static struct sbiret SBI_CALL_4(unsigned long ext, unsigned long funct, unsigned long args0, unsigned long args1, unsigned long args2, unsigned long args3) {*/
/*  return SBI_CALL(ext, funct, args0, args1, args2, args3, 0, 0);*/
/*}*/
/**/
/*static struct sbiret SBI_CALL_5(unsigned long ext, unsigned long funct, unsigned long args0, unsigned long args1, unsigned long args2, unsigned long args3, unsigned long args4) {*/
/*  return SBI_CALL(ext, funct, args0, args1, args2, args3, args4, 0);*/
/*}*/
/**/
/*static struct sbiret SBI_CALL_6(unsigned long ext, unsigned long funct, unsigned long args0, unsigned long args1, unsigned long args2, unsigned long args3, unsigned long args4, unsigned long args5) {*/
/*  return SBI_CALL(ext, funct, args0, args1, args2, args3, args4, args5);*/
/*}*/



static inline void sbi_error_handler(long);
static inline struct sbiret get_spec_version() {
  return SBI_CALL_0(SBI_EXT_BASE, SBI_EXT_BASE_GET_SPEC_VERSION);
}

static inline struct sbiret sbi_get_impl_id(void) {
  return SBI_CALL_0(SBI_EXT_BASE, SBI_EXT_BASE_GET_IMP_ID);
}

static inline void sbi_shutdown(void) {
  SBI_CALL_0(SBI_EXT_0_1_SHUTDOWN, SBI_SRST_RESET_TYPE_SHUTDOWN);
}

static inline long sbi_console_putchar(int ch) {
  // Legacy SBI v0.1 console putchar: a7=extension, a0=character.
  register uintptr_t a0 asm("a0") = (uintptr_t)ch;
  register uintptr_t a7 asm("a7") = (uintptr_t)SBI_EXT_0_1_CONSOLE_PUTCHAR;
  asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
  return (long)a0;
}

static inline long sbi_console_getchar(void) {
  // Legacy SBI v0.1 console getchar: returns char or -1 in a0.
  register uintptr_t a0 asm("a0");
  register uintptr_t a7 asm("a7") = (uintptr_t)SBI_EXT_0_1_CONSOLE_GETCHAR;
  asm volatile("ecall" : "=r"(a0) : "r"(a7) : "memory");
  return (long)a0;
}

static inline struct sbiret sbi_hart_start(unsigned long hartid,
                unsigned long start_addess, unsigned long opaque) {
  return SBI_CALL_3(SBI_EXT_HSM, SBI_EXT_HSM_HART_START, hartid, start_addess, opaque);
}

static inline struct sbiret sbi_set_timer(unsigned long ticks) {
  return SBI_CALL_1(SBI_EXT_TIME, SBI_EXT_TIME_SET_TIMER, ticks);
}

static inline void sbi_error_handler(long error) {
  if (error != SBI_ERR_IO) {
    // error_table[error];
    // 输出 error description
  }
  // 然后关机
  /*sbi_shutdown();*/
}
#endif // SBI_H__
