/* SecOS overlay for musl: __unmapself(base, size) (replaces musl's). A
 * detached thread unmaps its own stack and exits without touching it again:
 * munmap = SecOS 15 (rdi, rsi already hold base, size), then exit(0) of this
 * thread = SecOS 512 + 60. SPDX-License-Identifier: MIT */
.text
.global __unmapself
.type   __unmapself,@function
__unmapself:
	mov $15,%eax            /* SYS_MUNMAP(base, size) */
	int $0x80
	xor %edi,%edi
	mov $572,%eax           /* SYS_LX_EXIT(0) */
	int $0x80
	ud2
.size __unmapself,.-__unmapself
