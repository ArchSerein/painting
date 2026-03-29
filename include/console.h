#ifndef CONSOLE_H__

void    console_init(void);
void    console_putc(int);
int     console_read(int, uint64_t, int);
int     console_write(int, uint64_t, int);

#endif //!CONSOLE_H__
