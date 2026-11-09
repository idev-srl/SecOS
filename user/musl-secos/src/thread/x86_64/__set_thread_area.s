/* SecOS overlay for musl: set the thread pointer (replaces musl's).
 * arch_prctl(ARCH_SET_FS, p) is SecOS syscall 512 + 158 (Linux-shaped). Runs
 * before TLS exists, so it calls the kernel directly. SPDX-License-Identifier: MIT */
.text
.global __set_thread_area
.hidden __set_thread_area
.type __set_thread_area,@function
__set_thread_area:
	mov %rdi,%rsi           /* addr */
	movl $0x1002,%edi       /* ARCH_SET_FS */
	movl $670,%eax          /* SYS_LX_ARCH_PRCTL = 512 + 158 */
	int $0x80
	ret
.size __set_thread_area,.-__set_thread_area
