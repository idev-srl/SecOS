/*
 * selftest.c — M4 minimal isolation selftest suite.
 *
 * Tests user_range_valid() and copy_from_user/copy_to_user() boundary
 * conditions without requiring a live user process.  Positive byte-copy
 * tests are limited to range-validation only; actual copy of user bytes
 * requires a mapped user page (deferred to M5, see M4.md §TASK 3).
 *
 * Output goes to both terminal (VGA) and debugcon (port 0xE9).
 */
#include "selftest.h"
#if M4_SELFTEST_ENABLE

#include "terminal.h"
#include "../lib/debugcon.h"
#include "../mm/user_copy.h"
#include "../mm/vmm.h"   /* USER_CODE_BASE, VMM_PHYSMAP_BASE */
#include <stdint.h>
#include <stddef.h>

static int st_pass;
static int st_fail;

/* Internal hex printer for selftest summary (avoids dependency on print_hex) */
static void st_print_dec(int v) {
    if (v >= 10) st_print_dec(v / 10);
    char c = '0' + (char)(v % 10);
    debugcon_putchar(c);
    terminal_putchar(c);
}

static void st_check(const char* name, int cond) {
    if (cond) {
        st_pass++;
        debugcon_writestring("[M4][SELFTEST] PASS: ");
        terminal_writestring("[M4][SELFTEST] PASS: ");
    } else {
        st_fail++;
        debugcon_writestring("[M4][SELFTEST] FAIL: ");
        terminal_writestring("[M4][SELFTEST] FAIL: ");
    }
    debugcon_writestring(name);
    terminal_writestring(name);
    debugcon_writestring("\n");
    terminal_writestring("\n");
}

void m4_run_selftests(void) {
    st_pass = 0;
    st_fail = 0;

    debugcon_writestring("[M4][SELFTEST] --- begin ---\n");
    terminal_writestring("[M4][SELFTEST] --- begin ---\n");

    /* ----------------------------------------------------------
     * Test A: user_range_valid()
     * ---------------------------------------------------------- */

    /* A1: valid small buffer inside user range */
    st_check("A1 valid user buffer",
             user_range_valid((void*)0x200000000ULL, 64) == 1);

    /* A2: NULL pointer must be rejected */
    st_check("A2 NULL rejected",
             user_range_valid(NULL, 1) == 0);

    /* A3: kernel-canonical VA rejected */
    st_check("A3 kernel VA rejected",
             user_range_valid((void*)0xFFFF800000000000ULL, 1) == 0);

    /* A4: end overflows USER_ADDR_MAX: start = MAX-4, len = 0x10 */
    st_check("A4 overflow (end > USER_ADDR_MAX)",
             user_range_valid((void*)(USER_ADDR_MAX - 4ULL), 0x10) == 0);

    /* A5: start == USER_ADDR_MAX is non-canonical for user space */
    st_check("A5 USER_ADDR_MAX rejected",
             user_range_valid((void*)USER_ADDR_MAX, 1) == 0);

    /* A6: zero-length transfer rejected */
    st_check("A6 zero-len rejected",
             user_range_valid((void*)0x200000000ULL, 0) == 0);

    /* A7: len alone > USER_ADDR_MAX (size_t wrap) */
    st_check("A7 huge len rejected",
             user_range_valid((void*)0x1000ULL, (size_t)-1) == 0);

    /* ----------------------------------------------------------
     * Test B: copy_from_user / copy_to_user (negative + range)
     * ---------------------------------------------------------- */

    static uint8_t kbuf[16];  /* static: no heap needed */

    /* B1: kernel physmap VA as copy source */
    st_check("B1 copy_from_user(physmap VA)=-EFAULT",
             copy_from_user(kbuf, (void*)VMM_PHYSMAP_BASE, 8) == -EFAULT);

    /* B2: NULL as copy source */
    st_check("B2 copy_from_user(NULL)=-EFAULT",
             copy_from_user(kbuf, NULL, 8) == -EFAULT);

    /* B3: kernel VA as copy destination */
    st_check("B3 copy_to_user(kernel VA)=-EFAULT",
             copy_to_user((void*)0xFFFF800000000000ULL, kbuf, 8) == -EFAULT);

    /* B4: overflow in copy_from_user (end > USER_ADDR_MAX) */
    st_check("B4 copy_from_user(overflow)=-EFAULT",
             copy_from_user(kbuf, (void*)(USER_ADDR_MAX - 4ULL), 0x10) == -EFAULT);

    /* B5: range validation for a valid user address (no actual copy —
     *     byte-copy test deferred to M5 when user pages are mapped) */
    st_check("B5 user_range_valid(USER_CODE_BASE,64)=1",
             user_range_valid((void*)USER_CODE_BASE, 64) == 1);

    /* ----------------------------------------------------------
     * Summary
     * ---------------------------------------------------------- */
    int total = st_pass + st_fail;
    debugcon_writestring("[M4][SELFTEST] --- ");
    terminal_writestring("[M4][SELFTEST] --- ");
    st_print_dec(st_pass);
    debugcon_writestring("/");
    terminal_writestring("/");
    st_print_dec(total);
    if (st_fail == 0) {
        debugcon_writestring(" PASS ---\n");
        terminal_writestring(" PASS ---\n");
    } else {
        debugcon_writestring(" FAIL ---\n");
        terminal_writestring(" FAIL ---\n");
    }
}

#endif /* M4_SELFTEST_ENABLE */
