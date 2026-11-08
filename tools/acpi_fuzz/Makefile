# Host harness for arch/x86/acpi_dmar.c (ASan + UBSan). See acpi_fuzz.c.
CC ?= gcc
CFLAGS = -O1 -g -std=gnu99 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
         -Istub -I../../arch/x86

acpi_fuzz: acpi_fuzz.c ../../arch/x86/acpi_dmar.c ../../arch/x86/acpi_dmar.h ../../arch/x86/acpi.h stub/debugcon.h
	$(CC) $(CFLAGS) -o $@ acpi_fuzz.c ../../arch/x86/acpi_dmar.c

clean:
	rm -f acpi_fuzz
.PHONY: clean
