TAR = target
TOOLPREFIX = riscv64-linux-gnu-

CC = $(TOOLPREFIX)gcc
AS = $(TOOLPREFIX)gas
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump

CFLAGS = -Wall -Werror -O -fno-omit-frame-pointer -ggdb -gdwarf-2
CFLAGS += -MD
CFLAGS += -mcmodel=medany
CFLAGS += -ffreestanding -fno-common -nostdlib -mno-relax
CFLAGS += -fno-builtin-strncpy -fno-builtin-strncmp -fno-builtin-strlen -fno-builtin-memset
CFLAGS += -fno-builtin-memmove -fno-builtin-memcmp -fno-builtin-log -fno-builtin-bzero
CFLAGS += -fno-builtin-strchr -fno-builtin-exit -fno-builtin-malloc -fno-builtin-putc
CFLAGS += -fno-builtin-free
CFLAGS += -I./include
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)

LDFLAGS = -z max-page-size=4096

$(shell mkdir -p $(TAR))

UPROGS = \
						$(TAR)/init	\
						$(TAR)/sh	\
						$(TAR)/echo

ULIB = $(TAR)/lib.o \
			 $(TAR)/entry.o	\
			 $(TAR)/start.o	\
			 $(TAR)/stdio.o	\
			 $(TAR)/string.o	\
			 $(TAR)/stdlib.o

$(TAR)/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TAR)/%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

$(TAR)/%: $(TAR)/%.o $(ULIB)
	$(LD) $(LDFLAGS) -T ./user.ld -o $@ $^
	$(OBJDUMP) -S $@ > $(TAR)/$*.asm
	$(OBJDUMP) -t $@ \
		| sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $(TAR)/$*.sym

fs.img: $(UPROGS)
	@if [ ! -f "../fs.img" ]; then \
		echo "making fs image..."; \
		dd if=/dev/zero of=fs.img bs=1M count=512; \
		mkfs.vfat -F 32 fs.img;	\
		mv fs.img	../; fi
	@sudo mount ../fs.img ../mnt
	@for file in $(UPROGS); do \
		sudo cp $$file ../mnt/`basename $$file`;\
		echo "Copied `basename $$file` to fs.img"; \
	done
	@sudo cp text.txt ../mnt/
	@sudo umount ../mnt

clean: 
	rm -f *.asm *.sym \
	rm -rf $(TAR)
