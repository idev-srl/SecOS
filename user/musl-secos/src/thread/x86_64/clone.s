/* SecOS overlay for musl: __clone (replaces musl's). SecOS has no threads
 * yet (U9): clone is -ENOSYS. fork goes through SYS_fork, posix_spawn through
 * fork + execve (patches/posix_spawn-fork.patch). SPDX-License-Identifier: MIT */
.text
.global __clone
.hidden __clone
.type   __clone,@function
__clone:
	mov $-38,%rax           /* -ENOSYS */
	ret
