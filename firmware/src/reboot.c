#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#include "app.h"

/* A restart the firmware chose.  The watchdog and the fault handler go
 * straight to sys_reboot(): they cannot wait for a radio. */
void reboot_now(void)
{
    /* What the backlog still holds does not survive the restart; the log
     * lines do, but are better with the server than in RAM. */
    if (CONFIG_APP_DATABUF_REBOOT_DRAIN_S > 0) {
        data_send_logs(RESPONSE_TIMEOUT_MS, 4);
        databuf_deliver(RESPONSE_TIMEOUT_MS, DRAIN_REBOOT_MS, NULL);
    }
    sys_reboot(SYS_REBOOT_COLD);
}
