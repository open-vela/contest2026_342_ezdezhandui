/* SPDX-License-Identifier: Apache-2.0 */

/* Master override for the NuttX port of the official esp-hosted host stack.
 *
 * eh_host_port_config.h includes this file first and then only fills in what is
 * still undefined, so everything set here wins over the ESP-IDF defaults.  The
 * values are the ones measured to work with this board's companion
 * (docs/02_驱动实现/WiFi_SDIO实现.md appendices AK/AN): slot 1 through the GPIO
 * matrix, CLK 18 / CMD 19 / D0..D3 14..17, 4-bit, identification clock at
 * bring-up and the transfer clock afterwards.
 */

#ifndef __EH_HOST_PORT_MASTER_CONFIG_H_
#define __EH_HOST_PORT_MASTER_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

/* ── Which transport this build talks to the companion over ───────────── */

#define EH_HOST_PORT_SDIO                       1
#define EH_HOST_PORT_SPI                        0
#define EH_HOST_PORT_UART                       0
#if !defined(EH_HOST_PORT_SPI_HD)
#  define EH_HOST_PORT_SPI_HD                   0
#endif

/* ── Board wiring (ESP32-P4 function EV board, SDIO slot 1) ───────────── */

#define EH_HOST_PORT_SDIO_SLOT                  1
#define EH_HOST_PORT_SDIO_CLK_GPIO              18
#define EH_HOST_PORT_SDIO_CMD_GPIO              19
#define EH_HOST_PORT_SDIO_D0_GPIO               14
#define EH_HOST_PORT_SDIO_D1_GPIO               15
#define EH_HOST_PORT_SDIO_D2_GPIO               16
#define EH_HOST_PORT_SDIO_D3_GPIO               17

/* The companion's enable/reset line.  Held low briefly at bring-up and then
 * high; see the note in appendix AR about what it does and does not reset.
 */

#define EH_HOST_PORT_SDIO_EN_GPIO               54

/* 4-bit bus, 40 MHz transfer clock -- the configuration the official stack runs
 * on this board.  The identification phase is handled by the SDIO host layer
 * itself (400 kHz per the SD specification).
 */

#define EH_HOST_PORT_SDIO_BUS_WIDTH             4
#define EH_HOST_PORT_SDIO_CLOCK_KHZ             40000

/* NuttX already provides every primitive group below (pthreads, semaphores,
 * condition variables, message queues, task creation, heap, logging).
 */

#define EH_HOST_PORT_HAS_TASK                   1
#define EH_HOST_PORT_HAS_MUTEX                  1
#define EH_HOST_PORT_HAS_COND                   1
#define EH_HOST_PORT_HAS_SEM                    1
#define EH_HOST_PORT_HAS_QUEUE                  1
#define EH_HOST_PORT_HAS_EVENT_GROUP            1
#define EH_HOST_PORT_HAS_TIMER                  1

#ifdef __cplusplus
}
#endif

#endif /* __EH_HOST_PORT_MASTER_CONFIG_H_ */
