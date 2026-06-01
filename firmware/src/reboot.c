#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#include "app.h"

void reboot_now(void)
{
    sys_reboot(SYS_REBOOT_COLD);
}
