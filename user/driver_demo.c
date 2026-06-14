/* SecOS user-space driver demo (PROC_TYPE_DRIVER via signed manifest).
 * Granted dev 0 caps READ|WRITE|GET_INFO — proves mediated HW access works and
 * that an un-granted capability (MAP_MEM) is refused. SPDX-License-Identifier: MIT */
#include "libsecos.h"

int drvprobe_run(const char* tag);

int main(void) {
    puts("[driver_demo] signed driver running in ring 3");
    return drvprobe_run("driver");
}
