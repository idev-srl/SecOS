/* m31_libc — exercises the SecOS libc (printf/string/stdlib) end to end, in
 * ring 3, signed. Prints PASS/FAIL lines on stdout (fd 1). SPDX: MIT */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static int fails = 0;
static void check(const char* what, int ok) {
    printf("[m31] %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}
static int int_cmp(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }

int main(void) {
    /* printf formatting */
    char b[128];
    snprintf(b, sizeof b, "%d %u %x %o %c %s %%", -42, 42u, 255, 8, 'Z', "hi");
    check("printf basic", strcmp(b, "-42 42 ff 10 Z hi %") == 0);
    snprintf(b, sizeof b, "%5d|%-5d|%05d|%+d", 7, 7, 7, 7);
    check("printf width/flags", strcmp(b, "    7|7    |00007|+7") == 0);
    snprintf(b, sizeof b, "%ld %lx %p", 1234567890L, 0xdeadbeefUL, (void*)0x1000);
    check("printf long/ptr", strcmp(b, "1234567890 deadbeef 0x1000") == 0);
    snprintf(b, sizeof b, "%.3s|%.5d", "abcdef", 7);
    check("printf precision", strcmp(b, "abc|00007") == 0);

    /* string.h */
    check("strcmp", strcmp("abc", "abc") == 0 && strcmp("abc", "abd") < 0);
    check("strncmp", strncmp("abcZ", "abcQ", 3) == 0);
    char s[32]; strcpy(s, "Hello"); strcat(s, ", world");
    check("strcpy/strcat", strcmp(s, "Hello, world") == 0);
    check("strchr/strrchr", strchr(s, 'o') == s + 4 && strrchr(s, 'o') == s + 8);
    check("strstr", strstr(s, "world") == s + 7);
    char m[4]; memset(m, 0xAB, 4);
    check("memcmp/memset", memcmp(m, "\xAB\xAB\xAB\xAB", 4) == 0);

    /* strtol / atoi */
    check("atoi", atoi("  -123abc") == -123);
    check("strtol hex", strtol("0xFF", 0, 0) == 255);
    check("strtol base2", strtol("1010", 0, 2) == 10);

    /* malloc / realloc / free */
    char* p = malloc(16); strcpy(p, "dynamic");
    p = realloc(p, 64);
    check("malloc/realloc", strcmp(p, "dynamic") == 0);
    free(p);

    /* qsort */
    int arr[] = {5, 2, 9, 1, 7, 3};
    qsort(arr, 6, sizeof(int), int_cmp);
    check("qsort", arr[0] == 1 && arr[5] == 9);

    /* ctype */
    check("ctype", isdigit('5') && isalpha('x') && isspace(' ') && toupper('a') == 'A');

    printf("[m31] DONE fails=%d\n", fails);
    return fails;
}
