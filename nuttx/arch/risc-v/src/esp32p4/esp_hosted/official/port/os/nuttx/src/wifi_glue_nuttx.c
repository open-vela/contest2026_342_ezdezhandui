/****************************************************************************
 * esp_hosted_official/port/os/nuttx/src/wifi_glue_nuttx.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The three globals IDF's esp_wifi exports and the official stack's Wi-Fi
 * feature references:
 *
 *   g_wifi_osi_funcs                 the OSI function table
 *   g_wifi_default_wpa_crypto_funcs  the default WPA crypto table
 *   WIFI_EVENT                       the Wi-Fi event base
 *
 * There is no local radio here -- the companion runs the Wi-Fi stack -- so the
 * tables only have to exist and stay stable: the init configuration carrying
 * them goes over RPC and the companion uses its own.  Without them the feature
 * does not link (undefined reference to all three, appendix BY.14).
 *
 * esp_wifi.h is deliberately NOT included: it declares the tables with types
 * that are opaque in the injected headers (wifi_osi_funcs_t has no size), so a
 * definition with the declared type cannot be compiled.  At link time only the
 * symbol matters, and the arrays below are larger than the real structures
 * (both are tables of a few dozen 32-bit slots), so the reader's copy stays
 * inside them.  All-zero function tables are never called: no code on this host
 * runs a local Wi-Fi driver.
 */

#include <stddef.h>

/* Storage for the opaque tables.  Generously sized on purpose (see above). */

unsigned char g_wifi_osi_funcs[512];

unsigned char g_wifi_default_wpa_crypto_funcs[512];

/* The event base is a string pointer; its declared type is "const char *const",
 * which is exactly this.
 */

const char *const WIFI_EVENT = "WIFI_EVENT";
