#ifndef _PORT_SYS_WAIT_H
#define _PORT_SYS_WAIT_H
#define WNOHANG 1
#define WUNTRACED 2
#define WIFEXITED(s)   (((s)&0x7f)==0)
#define WEXITSTATUS(s) (((s)>>8)&0xff)
#define WIFSIGNALED(s) (((signed char)(((s)&0x7f)+1)>>1)>0)
#define WTERMSIG(s)    ((s)&0x7f)
#define WIFSTOPPED(s)  (((s)&0xff)==0x7f)
#define WSTOPSIG(s)    (((s)>>8)&0xff)
#define WCOREDUMP(s)   ((s)&0x80)
int waitpid(int pid, int* status, int options);
int wait(int* status);
#endif
