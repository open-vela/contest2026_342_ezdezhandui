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

/****************************************************************************
 * Name: esp_hosted_netdev_register
 *
 * Description:
 *   Create wlan0 and bind it to the stack's STA channel.  Called once the
 *   stack is connected to the companion.
 *
 ****************************************************************************/

int esp_hosted_netdev_register(void);

/* Give wlan0 the address the companion obtained over DHCP (it arrives in the
 * DhcpDnsStatus event: dhcp_ip / dhcp_nm / dhcp_gw, four bytes each).
 */

int esp_hosted_netdev_up(void);

int esp_hosted_netdev_apply_ip(FAR const uint8_t *ip, FAR const uint8_t *nm,
                               FAR const uint8_t *gw);

#endif /* CONFIG_ESPRESSIF_HOSTED_OFFICIAL */

#endif /* __EH_HOSTED_NUTTX_H */
