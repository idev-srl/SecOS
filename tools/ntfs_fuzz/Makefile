# Host harness for fs/ntfs.c (ASan + UBSan). See ntfs_fuzz.c.
CC ?= gcc
CFLAGS = -O1 -g -std=gnu99 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
         -Istub -I../../fs -I../../mm

ntfs_fuzz: ntfs_fuzz.c ../../fs/ntfs.c ../../fs/ntfs.h ../../fs/vfs.h stub/debugcon.h stub/heap.h stub/klock.h
	$(CC) $(CFLAGS) -o $@ ntfs_fuzz.c ../../fs/ntfs.c

clean:
	rm -f ntfs_fuzz
.PHONY: clean
