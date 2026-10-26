/* Shim for <linux/types.h>, which upstream eh_common_interface.h includes.
 *
 * The NuttX cross-build has no linux/types.h, and the upstream file is kept
 * byte-for-byte identical to esp-hosted, so the missing header is supplied
 * here instead. This directory is added to the include path *after* the
 * component's own includes, and NuttX has nothing else that looks for
 * linux/types.h, so the shadowing is confined to this component.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
