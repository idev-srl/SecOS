/* SecOS overlay for musl: the signal return trampoline (replaces musl's).
 * The SecOS kernel enters a handler with the address of this trampoline as
 * its return address; when the handler returns, rsp points at the kernel's
 * signal frame and SYS_SIGRETURN (45) restores the interrupted context.
 * SPDX-License-Identifier: MIT */
.text
.global __restore_rt
.hidden __restore_rt
.type __restore_rt,@function
__restore_rt:
	mov $45, %eax
	int $0x80
	ud2
.size __restore_rt,.-__restore_rt
