/****************************************************************************
 * esp_hosted_official/port/os/nuttx/src/eh_host_port_task_nuttx.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The one timing primitive the POSIX port layer does not provide.
 *
 * port/os/posix/src implements delays in milliseconds (eh_host_port_task_delay_ms);
 * the SDIO bus driver also needs a microsecond delay.  NuttX has both, so this
 * is a one-line mapping rather than another port group.
 */

#include <nuttx/config.h>

#include <nuttx/signal.h>

#include "eh_host_port_task.h"

#ifdef CONFIG_ESPRESSIF_HOSTED_OFFICIAL

void eh_host_port_task_delay_us(uint32_t us)
{
  nxsig_usleep((useconds_t)us);
}

#endif /* CONFIG_ESPRESSIF_HOSTED_OFFICIAL */
