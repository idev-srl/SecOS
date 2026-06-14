/* SecOS user probe (PROC_TYPE_USER via signed manifest). A normal user process
 * has no driver rights: every SYS_DRIVER call must be refused (DRV_ERR_NOTDRV),
 * proving the Driver Space boundary. SPDX-License-Identifier: MIT */
#include "libsecos.h"

int drvprobe_run(const char* tag);

int main(void) {
    puts("[userprobe] signed user program probing SYS_DRIVER (expect all denied)");
    return drvprobe_run("user");
}
