/* SecOS overlay for musl: __unmapself (replaces musl's). Only a detached
 * thread exits through it; without threads (U9) this is the process exit.
 * SPDX-License-Identifier: MIT */
.text
.global __unmapself
.type   __unmapself,@function
__unmapself:
	xor %edi,%edi
	mov $743,%eax           /* SYS_LX_EXIT_GROUP = 512 + 231 */
	int $0x80
	ud2
