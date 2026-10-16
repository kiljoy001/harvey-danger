CC = clang
CFLAGS = -Wall -Wextra -O2
PAYLOAD_CFLAGS = -m32 -ffreestanding -nostdlib -fno-pie -no-pie

all: 9vm test_payload.elf

9vm: 9vm.c hostfs.c hostfs.h vmdisplay.h
	$(CC) $(CFLAGS) -pthread -o 9vm 9vm.c hostfs.c

test_payload.elf: test_payload.S
	$(CC) $(PAYLOAD_CFLAGS) -Wl,-Ttext,0x100000 -o test_payload.elf test_payload.S

clean:
	rm -f 9vm test_payload.elf

.PHONY: all clean
