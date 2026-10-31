/****************************************************************************
 * esp_hosted_official/port/os/nuttx/include/eh_hosted_nuttx.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __EH_HOSTED_NUTTX_H
#define __EH_HOSTED_NUTTX_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_ESPRESSIF_HOSTED_OFFICIAL

/****************************************************************************
 * Name: esp_hosted_official_start
 *
 * Description:
 *   Start the official esp-hosted host stack on a kernel thread.  Call after
 *   the SDIO link is up (esp_hosted_sdio_initialize()).
 *
 ****************************************************************************/

int esp_hosted_official_start(void);

#endif /* CONFIG_ESPRESSIF_HOSTED_OFFICIAL */

#endif /* __EH_HOSTED_NUTTX_H */
