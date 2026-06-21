/* SecOS libc - setjmp/longjmp (non-local jumps).
 * Saves the callee-saved registers + stack pointer + return address. Required by
 * lua/sqlite-style error handling. setjmp MUST be a real call (never inlined)
 * for the saved frame to be valid. */
#ifndef _SETJMP_H
#define _SETJMP_H

/* rbx, rbp, r12, r13, r14, r15, rsp, rip */
typedef long jmp_buf[8];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

/* C does not have sigjmp_buf semantics here; alias them for portability. */
typedef jmp_buf sigjmp_buf;
#define sigsetjmp(env, save) setjmp(env)
#define siglongjmp(env, val) longjmp(env, val)

#endif
