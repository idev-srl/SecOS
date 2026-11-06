# Host fuzz harness for fs/exfat.c (ASan + UBSan). See exfat_fuzz.c.
CC ?= gcc
CFLAGS = -O1 -g -std=gnu99 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
         -Istub -I../../fs -I../../mm

exfat_fuzz: exfat_fuzz.c ../../fs/exfat.c ../../fs/exfat.h stub/debugcon.h stub/heap.h
	$(CC) $(CFLAGS) -o $@ exfat_fuzz.c ../../fs/exfat.c

clean:
	rm -f exfat_fuzz
.PHONY: clean
