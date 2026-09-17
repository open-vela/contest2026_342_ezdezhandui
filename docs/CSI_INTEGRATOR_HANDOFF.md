# ESP32-P4 MIPI-CSI camera bring-up — integrator handoff

Everything below is **build-verified** but **not** hardware-verified. The camera
has never been powered on by this workstream.

---

## 1. Files created / modified

### Created (new files — safe to commit as-is)

| Path | What |
|---|---|
| `nuttx/arch/risc-v/src/common/espressif/esp_csi.c` | NuttX CSI capture driver — V4L2 `imgdata_s` lower half wrapping the vendored ESP-IDF CSI HAL |
| `nuttx/arch/risc-v/src/common/espressif/esp_csi.h` | Its public API (`esp_csi_config_s`, `esp_csi_get_default_config`, `esp_csi_imgdata_initialize/uninitialize`, `esp_csi_frames`) |
| `nuttx/arch/risc-v/src/common/espressif/freertos_compat/freertos/FreeRTOS.h` | FreeRTOS compat shim (headers only) |
| `nuttx/arch/risc-v/src/common/espressif/freertos_compat/freertos/queue.h` | ditto |
| `nuttx/arch/risc-v/src/common/espressif/freertos_compat/freertos/task.h` | ditto |
| `nuttx/arch/risc-v/src/common/espressif/freertos_compat/freertos/idf_additions.h` | ditto |
| `nuttx/drivers/video/sc2336.c` | SC2336 sensor driver — `imgsensor_s` lower half, SCCB/I2C only |
| `nuttx/include/nuttx/video/sc2336.h` | Its public API |
| `nuttx/boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_camera.c` | Board bring-up: probe sensor, register `/dev/video0` |

### Modified (small, additive edits — mine only)

| Path | Change |
|---|---|
| `nuttx/arch/risc-v/src/common/espressif/Kconfig` | `menuconfig ESPRESSIF_MIPI_CSI` + 6 sub-options, inserted right after the `ESPRESSIF_MIPI_DSI` block |
| `nuttx/arch/risc-v/src/common/espressif/Make.defs` | `ifeq ($(CONFIG_ESPRESSIF_MIPI_CSI),y) CHIP_CSRCS += esp_csi.c` |
| `nuttx/arch/risc-v/src/common/espressif/CMakeLists.txt` | `if(CONFIG_ESPRESSIF_MIPI_CSI) list(APPEND SRCS esp_csi.c)` + `freertos_compat` include dir |
| `nuttx/arch/risc-v/src/esp32p4/hal_esp32p4.cmake` | CSI include paths, CSI `HAL_SRCS` block, `upper_hal_dma/include/esp_private`, and **`components/hal/color_hal.c` added to the base source list** |
| `nuttx/arch/risc-v/src/esp32p4/hal_esp32p4.mk` | Same four changes for the Make build |
| `nuttx/drivers/video/Kconfig` | `config VIDEO_SC2336` block, inserted before `config VIDEO_OV2640` |
| `nuttx/drivers/video/Make.defs` | `ifeq ($(CONFIG_VIDEO_SC2336),y) CSRCS += sc2336.c` |
| `nuttx/drivers/video/CMakeLists.txt` | `if(CONFIG_VIDEO_SC2336) list(APPEND SRCS sc2336.c)` |

**Not touched:** board `defconfig`, `esp32p4_bringup.c`, board `Kconfig`,
board `src/CMakeLists.txt`, board `src/Make.defs`, board `include/board.h`,
`esp32p4-function-ev-board.h`, and everything under `esp-hal-3rdparty/`.

---

## 2. HAL sources that must be compiled in

`esp-hal-3rdparty` at the pinned commit already contains the whole stack — **no
HAL bump, no vendored-file edits**. Seven sources have to be added when
`CONFIG_ESPRESSIF_MIPI_CSI=y`:

```
esp_hal_cam/mipi_csi_hal.c                        CSI PHY + host HAL
esp_hal_cam/esp32p4/mipi_csi_periph.c             SoC PLL range table
esp_hw_support/mipi_csi_share_hw_ctrl.c           CSI-bridge claim/declaim (CSI vs ISP)
upper_hal_cam/esp_cam_ctlr.c                      esp_cam_ctlr_* dispatcher
upper_hal_cam/csi/src/esp_cam_ctlr_csi.c          the P4 CSI controller driver
upper_hal_dma/src/dw_gdma.c                       DW-GDMA upper driver (CSI's DMA)
components/hal/color_hal.c                        RAW/RGB/YUV bit-depth lookup  ← was MISSING
```

`components/hal/dw_gdma_hal.c` is also required but it is already pulled in by
the MIPI-DSI block; the two blocks must not both add it (double definition).

### Exact `hal_esp32p4.cmake` block (CMake — this is the one that matters)

Inserted after the existing `if(CONFIG_ESPRESSIF_MIPI_DSI)` block:

```cmake
# DW-GDMA HAL is shared by the MIPI-DSI framebuffer path and the MIPI-CSI
# capture path; add it only once when either is enabled.

if(CONFIG_ESPRESSIF_MIPI_DSI OR CONFIG_ESPRESSIF_MIPI_CSI)
  list(
    APPEND
    HAL_SRCS
    ${ESP_HAL_3RDPARTY_REPO}/components/esp_hal_dma/dw_gdma_hal.c)
endif()

if(CONFIG_ESPRESSIF_MIPI_CSI)
  list(
    APPEND
    HAL_SRCS
    ${ESP_HAL_3RDPARTY_REPO}/components/esp_hal_cam/mipi_csi_hal.c
    ${ESP_HAL_3RDPARTY_REPO}/components/esp_hal_cam/${CHIP_SERIES}/mipi_csi_periph.c
    ${ESP_HAL_3RDPARTY_REPO}/components/esp_hw_support/mipi_csi_share_hw_ctrl.c
    ${ESP_HAL_3RDPARTY_REPO}/components/upper_hal_cam/esp_cam_ctlr.c
    ${ESP_HAL_3RDPARTY_REPO}/components/upper_hal_cam/csi/src/esp_cam_ctlr_csi.c
    ${ESP_HAL_3RDPARTY_REPO}/components/upper_hal_dma/src/dw_gdma.c)
endif()
```

and the DSI block **loses** its trailing `dw_gdma_hal.c` line (moved into the
shared block above).

Include paths, added to the `ESP32P4_INCLUDES` list:

```cmake
    ${ESP_HAL_3RDPARTY_REPO}/components/upper_hal_cam/include
    ${ESP_HAL_3RDPARTY_REPO}/components/upper_hal_cam/csi/include
    ${ESP_HAL_3RDPARTY_REPO}/components/upper_hal_cam/interface
```
plus, immediately after the existing `upper_hal_dma/include` entry:
```cmake
    ${ESP_HAL_3RDPARTY_REPO}/components/upper_hal_dma/include/esp_private
```

and **one line in the base (unconditional) source list**, next to
`components/hal/cache_hal.c`:

```cmake
  ${ESP_HAL_3RDPARTY_REPO}/components/hal/color_hal.c
```

`components/esp_hal_cam/{include,${CHIP_SERIES}/include}` and
`components/esp_hal_dma/include` are already present and need no change.

### Why the FreeRTOS shim exists

`esp_cam_ctlr_csi.c` and `dw_gdma.c` are ESP-IDF code: they include
`freertos/FreeRTOS.h`, `freertos/idf_additions.h`, `freertos/task.h` and call
`xQueueCreateWithCaps`, `xQueueSend`, `xQueueReceiveFromISR`,
`vQueueDeleteWithCaps`, `portMUX_TYPE`, `portENTER_CRITICAL`, `MAX()`.

NuttX already has an ESP-IDF OS abstraction layer in the HAL
(`esp-hal-3rdparty/nuttx/include/platform/os.h` + `nuttx/src/platform/os.c`
providing `esp_os_queue_*`). The four new headers are a thin FreeRTOS-flavoured
facade over that existing layer — **no new `.c` file, no runtime code**. Two
details worth knowing:

* `portMUX_TYPE` must be `rspinlock_t` in *both* SMP and single-core builds.
  On single core the critical section still compiles into an interrupt disable,
  but the object must exist for `portMUX_INITIALIZE(&lock)` to be type-safe.
* `MAX()` is used by `esp_cam_ctlr_csi.c` but never defined for this target;
  the shim defines it behind `#ifndef`.

The include dir is added **only** when `CONFIG_ESPRESSIF_MIPI_CSI=y`:

```cmake
if(CONFIG_ESPRESSIF_MIPI_CSI)
  target_include_directories(arch PRIVATE
                             ${CMAKE_CURRENT_SOURCE_DIR}/freertos_compat)
endif()
```

---

## 3. Snippets to apply to the shared files (you own these)

### 3.1 `configs/nsh/defconfig` — append

```kconfig
CONFIG_DRIVERS_VIDEO=y
CONFIG_VIDEO_STREAM=y
CONFIG_VIDEO_REQBUFS_COUNT_MAX=3
CONFIG_ESPRESSIF_MIPI_CSI=y
CONFIG_ESPRESSIF_MIPI_CSI_H_RES=1920
CONFIG_ESPRESSIF_MIPI_CSI_V_RES=1080
CONFIG_ESPRESSIF_MIPI_CSI_LANES=2
CONFIG_ESPRESSIF_MIPI_CSI_LANE_BITRATE_MBPS=405
CONFIG_VIDEO_SC2336=y
CONFIG_SC2336_I2CADDR=0x30
CONFIG_SC2336_FREQUENCY=100000
CONFIG_ESP32P4_FUNCTION_EV_BOARD_CAMERA=y
```

Notes:
* `CONFIG_ESPRESSIF_MIPI_CSI` already `select`s `ESPRESSIF_LDO`,
  `DRIVERS_VIDEO` and `VIDEO_STREAM`, so those three lines are belt-and-braces.
* **`CONFIG_VIDEO_REQBUFS_COUNT_MAX` must be written explicitly.** It is a
  prompt-less `int` inside `if VIDEO_STREAM`, and a NuttX `defconfig` does not
  record defaulted prompt-less ints — without the line the symbol is undefined
  and `drivers/video/v4l2_cap.c` fails to compile with
  `'CONFIG_VIDEO_REQBUFS_COUNT_MAX' undeclared`. This bit me during the build
  test.
* `CONFIG_SC2336_FREQUENCY=100000` (100 kHz) is deliberate: the camera SCCB
  shares I2C0 with the GT911 touch controller. Raise only after the link works.
* The lane bit rate **must be 405**, matching the sensor table. 200 (the value
  in Espressif's `mipi_isp_dsi` IDF example) silently gives no frames.

### 3.2 board `Kconfig` — add inside `if ARCH_BOARD_ESP32P4_FUNCTION_EV_BOARD`

```kconfig
config ESP32P4_FUNCTION_EV_BOARD_CAMERA
	bool "MIPI-CSI camera (SC2336) and /dev/video0"
	default n
	depends on ESPRESSIF_MIPI_CSI && VIDEO_SC2336
	select I2C
	---help---
		Bring up the MIPI-CSI capture path of the ESP32-P4X-C5-Function-EV
		Board and register it as a V4L2 capture device (/dev/video0).

		The board carries an AS-AG638A32M2-50 module built around the
		SmartSens SC2336 (1/3", 1920x1080) on a 2-lane MIPI CSI-2 link.

		The module has its own 24 MHz oscillator and its reset line is hard
		pulled to 3.3 V, so no clock or reset GPIO is driven.  The MIPI PHY
		supply (on-chip LDO channel 3 @ 2500 mV, shared with the DSI PHY) is
		acquired by esp_csi_imgdata_initialize(); it is reference counted,
		so enabling both the LCD and the camera is safe.

		The sensor SCCB interface shares I2C0 (SDA GPIO7, SCL GPIO8) with
		the GT911 touch controller.

		The CSI receiver window is fixed at the resolution configured by
		CONFIG_ESPRESSIF_MIPI_CSI_H_RES/_V_RES; resolution changes at
		run time are not supported yet.

config ESP32P4_FUNCTION_EV_BOARD_CAMERA_DEVPATH
	string "Camera capture device path"
	default "/dev/video0"
	depends on ESP32P4_FUNCTION_EV_BOARD_CAMERA
```

### 3.3 board `src/CMakeLists.txt` — add

```cmake
if(CONFIG_ESP32P4_FUNCTION_EV_BOARD_CAMERA)
  list(APPEND SRCS esp32p4_camera.c)
endif()
```

### 3.4 board `src/Make.defs` — add

```makefile
ifeq ($(CONFIG_ESP32P4_FUNCTION_EV_BOARD_CAMERA),y)
  CSRCS += esp32p4_camera.c
endif
```

### 3.5 board `src/esp32p4-function-ev-board.h` — add the prototype

```c
/****************************************************************************
 * Name: esp32p4_camera_initialize
 *
 * Description:
 *   Register the MIPI-CSI camera (SC2336 on the AS-AG638A32M2-50 module) as
 *   a V4L2 capture device.  Returns 0 on success, a negated errno otherwise.
 *
 ****************************************************************************/

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_CAMERA
int esp32p4_camera_initialize(void);
int esp32p4_camera_uninitialize(void);
#endif
```

### 3.6 board `src/esp32p4_bringup.c` — add near the end of `esp_bringup()`

Place it **after** the display block and after the touchscreen block, so the
shared I2C0 bus and the display are already up:

```c
#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_CAMERA
  /* MIPI-CSI camera.  esp_i2cbus_initialize() is reference counted, so this
   * is safe whether or not the touchscreen brought I2C0 up first.
   */

  ret = esp32p4_camera_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to initialize the camera: %d\n", ret);
    }
#endif
```

---

## 4. Build status

### Proven

* **Scratch CMake build from clean, `/tmp/csi_build`, separate from `cmake_out/`:**
  `ninja` exits 0, **0 errors**, and **no warnings originating in any of the
  new files or in the six HAL sources**.
* All seven HAL sources, `esp_csi.c` and `sc2336.c` appear in `build.ninja`
  and produce objects.
* **The whole chain really links.** Forcing the entry points with
  `-Wl,--undefined=esp_csi_imgdata_initialize -Wl,--undefined=sc2336_initialize
  -Wl,--undefined=esp_csi_get_default_config` (otherwise `--gc-sections` drops
  the board file, since nothing calls it yet) yields a binary containing
  `esp_csi_imgdata_initialize`, `sc2336_initialize`, `esp_cam_new_csi_ctlr`,
  `dw_gdma_new_channel`, `mipi_csi_hal_init`,
  `color_hal_pixel_format_fourcc_get_bit_depth` and `esp_ldo_channel_acquire` —
  so there are no unresolved references anywhere along the path.
* **A no-CSI baseline also builds** (`/tmp/csi_base`), giving a size delta.

| Build | text | data | bss | dec |
|---|---|---|---|---|
| baseline (no CSI, no video stream) | 502096 | 6657 | 509139 | 1017892 |
| **with CSI + SC2336** | 493012 | 6425 | 441939 | 941376 |

The CSI build is *smaller* — that is not a mistake: enabling `DRIVERS_VIDEO`
changes which other subsystems are in play and the two configs differ in more
than the camera. **Do not read this as "the camera is free."** The honest
per-object cost of the camera stack, from `riscv-none-elf-size` on the actual
objects, is:

| Object | text | data | bss |
|---|---|---|---|
| `esp_cam_ctlr_csi.c.o` | 2806 | 0 | 8 |
| `sc2336.c.o` | 2157 | 0 | 44 |
| `esp_csi.c.o` | 1348 | 0 | 124 |
| `dw_gdma.c.o` | 3494 | 0 | 12 |
| `mipi_csi_hal.c.o` | 396 | 0 | 0 |
| `mipi_csi_periph.c.o` | 460 | 0 | 0 |
| `esp_cam_ctlr.c.o` | 240 | 0 | 0 |
| `mipi_csi_share_hw_ctrl.c.o` | 332 | 12 | 0 |
| `color_hal.c.o` | 290 | 0 | 0 |
| **total** | **~11.5 KB** | 12 B | ~190 B |

Plus, at run time and **only when capture is actually started**:
one backup frame buffer owned by the HAL (~2.5 MB in PSRAM for RAW10 1080p,
**PSRAM only** — see §6) and whatever the application allocates through
`VIDIOC_REQBUFS`. **This matters for the flash-XIP budget work: the camera adds
~11.5 KB of flash-resident code, not a multi-megabyte static image.**

* **checkpatch clean** on `esp_csi.c`, `esp_csi.h`, `sc2336.c`, `sc2336.h`,
  `esp32p4_camera.c` (0 errors, 0 warnings).
* The SC2336 register table in `sc2336.c` was diffed **pair-by-pair against the
  upstream vendor table**: 149 pairs, values and order identical.

### Not proven — requires the board

* Anything at all about the sensor: no SCCB transaction has ever been issued.
* The CSI PHY link, lane rate, or whether the receiver locks.
* Whether `esp_cam_ctlr_*` behaves correctly under NuttX's task/ISR model
  (see the risk list below).
* Whether the LDO acquisition succeeds on this board.
* No test image, no `VIDIOC_DQBUF` ever returned.

---

## 5. Sensor initialization sequence

### Identification (verified verbatim against Espressif's `esp_cam_sensor` v2.5.0)

| Item | Value |
|---|---|
| SCCB address | `0x30` (7-bit) |
| Framing | 16-bit register address + 8-bit data, **no page/bank select, no dummy write, no preceding soft reset** |
| ID registers | `0x3107` → `0xcb`, `0x3108` → `0x3a` (PID `0xcb3a`) |
| Reset | none — hard pulled to 3.3 V on this module |
| XCLK | none — the module has its own 24 MHz oscillator. Espressif's own driver has `SC2336_ENABLE_OUT_XCLK` as an **empty macro**, confirming the host never drives XCLK. |

`sc2336_chipid()` is a standalone helper so the board can probe and log the ID
before anything else comes up.

### Mode programming (`sc2336_start_capture()`)

Applied in this order, on the first `VIDIOC_STREAMON`:

1. Read back `0x3107`/`0x3108`, require `0xcb3a`. Mismatch → `-ENODEV`.
2. Write the **149-entry 1920x1080@30 RAW10 2-lane table** in order.
3. `0x0100 = 0x01` → leave sleep / stream on.
4. Wait 100 ms (the first frames after leaving sleep are not valid).

`sc2336_stop_capture()` writes `0x0100 = 0x00`.

**Source of the table** — this is the part you should be able to audit:
Espressif `esp-video-components`, component `esp_cam_sensor` v2.5.0,
file `sensors/sc2336/private_include/sc2336_mipi_2lane_24Minput_1920x1080_raw10_30fps.h`,
blob `11875772d8d7fab10150f3f914bab6610acee520`. Note the sensor is **not** in
`espressif/esp-idf` — it lives in the separate `esp-video-components` repo; in
an installed IDF project it shows up as
`managed_components/espressif__esp_cam_sensor/sensors/sc2336/`. The file's own
comment identifies the tuning set as
`cleaned_0x02_SC2336_MIPI_24Minput_2lane_405Mbps_10bit_1920x1080_30fps`.

Two things about that table that look like bugs and are not:

* `0x36e9` and `0x37f9` are each written **twice**, `0x80` early and `0x53` at
  the end (`{0x36e9,0x80}, {0x37f9,0x80}, ... , {0x36e9,0x53}, {0x37f9,0x53}`).
  That is the PLL unlock/lock pair and is intentional.
* `0x3e01`/`0x3e02` carry the power-on exposure `0x37e`. An upstream commit in
  Nov 2025 changed these in *every* SC2336 table; any pre-Nov-2025 copy you find
  (blogs, older IDF) has different values.

Timing constants the table establishes: HTS 2250 (`0x320c/d` = `0x08/0xca`),
VTS 1200 (`0x320e/f` = `0x04/0xb0`) → 81 MHz pclk / (2250×1200) = 30.0 fps,
tline 27.78 µs. Bayer pattern **BGGR**.

Runtime-tunable controls implemented: `IMGSENSOR_ID_EXPOSURE_ABSOLUTE`
(3-register packing `0x3e00[3:0]` / `0x3e01` / `0x3e02[7:4]`, clamped to
[8, VTS−6]=[8,1194]), `IMGSENSOR_ID_HFLIP_VIDEO` and
`IMGSENSOR_ID_VFLIP_VIDEO` (`0x3221` bits [2:1] / [6:5]).

**Deliberately not implemented:** AE/AWB. The ESP-IDF driver derives gain from
`0x3e06`/`0x3e07`/`0x3e09` and runs 3A in the ISP block; here the sensor runs at
its table-programmed exposure and gain. A fixed exposure indoors will look dark.

---

## 6. Blockers, uncertainties, and what still needs hardware

### Things I know will need attention before the first capture

1. **RAW10 + NuttX V4L2 is a poor fit and I could not fully fix it.** The
   NuttX V4L2 upper half has no 10-bit packed format: `get_bufsize()` falls
   through to `width*height*2`, and its format conversion table has no RAW
   entry. My driver works around it by **clamping the frame length it hands to
   the CSI HAL to `ceil(w*h*10/8)` rounded up to the cache line** — the HAL
   cache-syncs the transaction length on every transfer, and using the
   upper half's 4 MiB instead of the true 2.5 MiB would invalidate 1.6 MiB of
   unrelated memory per frame. **Net effect: capture works, but the V4L2 buffer
   that the application dequeues is a 4 MiB allocation of which only the first
   ~2.5 MiB are the frame, and `bytesused` is not meaningful for RAW10.**
   If the consumer is a preview/vision pipeline, the clean fix is to have the
   ISP produce RGB565/YUV422 (a proper V4L2 format) rather than RAW10.
2. **No ISP.** Espressif's real SC2336 path is CSI → ISP → RGB/YUV. The CSI
   bridge can only convert RGB↔YUV (`mipi_csi_brg_ll_set_input_color_format`
   `HAL_ASSERT`s on anything else), so a RAW sensor stream **must** be
   pass-through. `upper_hal_isp` is vendored but not wired up — it is a second
   driver of the same size and was explicitly out of scope for first bring-up.
3. **Frame buffer memory: PSRAM, and that is forced.** The HAL's backup buffer
   is `heap_caps_aligned_alloc(..., MALLOC_CAP_SPIRAM)` and the frame buffers
   come from the `imgdata_s` `alloc` callback, which I also make PSRAM-backed.
   `heap_caps_*` in this port ignores capability flags and calls `kmm_memalign`,
   and in this configuration `kmm_*` collapses to `kumm_*`, i.e. the user heap —
   which is exactly where `esp_allocateheap.c` places PSRAM
   (`CONFIG_ESPRESSIF_SPIRAM_USER_HEAP=y`, `CONFIG_MM_KERNEL_HEAP` unset). So
   PSRAM allocations will succeed. **But it also means the "SPIRAM" capability
   is not actually being honoured** — if that config ever changes to a kernel
   heap, these allocations start coming out of internal SRAM and will fail at
   multi-MB sizes. Worth a follow-up.
4. **Cache alignment.** `esp_cache_msync()` hard-rejects an unaligned buffer and
   the HAL `assert()`s on the result. The frame size is therefore rounded up to
   the cache line and every buffer is allocated with that alignment, queried at
   run time via `esp_cache_get_alignment(MALLOC_CAP_SPIRAM|MALLOC_CAP_DMA)`.
   `CONFIG_CACHE_L2_CACHE_LINE_SIZE=64` and the CSI HAL queries `EXT_MEM`, so
   the effective value should be 64 — **but L2 in this board config is 256 KB
   @ 64 B line, and I have not verified at run time that the two agree.** If
   bring-up stops inside `esp_cache_msync`, check this first.
5. **`portYIELD_FROM_ISR()` is a no-op in this port.** The ESP-IDF stack expects
   `xQueueReceiveFromISR` to be able to wake a higher-priority task immediately.
   My shim maps the queue to NuttX message queues, and `OS_PORT_YIELD_FROM_ISR`
   is empty. **Consequence: with the queue-based `esp_cam_ctlr_receive()` path
   there would be up to one tick (10 ms at `CONFIG_USEC_PER_TICK=10000`, i.e.
   100 Hz) of latency.** This is exactly why `esp_csi.c` does **not** use the
   HAL transaction queue at all: it uses the `on_get_new_trans` callback, so no
   task ever waits on a queue in the capture path. The queue is only allocated
   (`queue_items=1`) and never used for data.
6. **`esp_cam_ctlr_csi.c` calls a task-context function from ISR.** In
   `s_ctlr_csi_start()` → no; but `esp_cache_msync()` reached from the DMA ISR
   goes through `s_acquire_mutex_from_task_context()`, which is written to skip
   the lock only when it detects ISR context — and its detection reads
   `xPortInIsrContext()`/`esp_os_*`-adjacent helpers that I have **not** verified
   exist under NuttX. If it takes a mutex from ISR, capture will hang or assert
   on the first frame. **This is the single most likely place for bring-up to
   stop.** It is also why I want a serial console on the first attempt.
7. **Dropped frames are silent by design.** If the application does not return a
   buffer in time, the callback hands the HAL an empty transaction and the frame
   goes to the HAL backup buffer; it is *not* reported upward, and
   `esp_csi_frames()` counts it. Data corruption is therefore impossible, at the
   cost of invisible frame drops. Use `esp_csi_frames()` to see them.
8. **Resolution is fixed at init.** `esp_csi_data_validate_frame_setting()`
   rejects anything other than the configured `H_RES`×`V_RES`. Runtime
   reconfiguration is a real piece of work (tear down and rebuild the
   controller), not a register write.

### What to do on the first hardware session, in order

1. Flash a build with **only** `CONFIG_ESP32P4_FUNCTION_EV_BOARD_CAMERA=y` and
   nothing else new; keep the known-good fallback image.
2. Watch for `SC2336 chip ID: 0xcb3a` on the console. **That single line proves
   the module is powered, the 24 MHz oscillator is running, I2C0 is wired
   correctly, and the 16-bit SCCB framing is right.** If it prints `0x0000` you
   have an I2C problem, not a sensor problem; if it times out, suspect the
   module's power rail before the code.
3. Confirm `/dev/video0` exists.
4. Only then attempt a capture. If it hangs, the prime suspects in order are
   risk 6 (`esp_cache_msync` from ISR), risk 4 (cache alignment), then the lane
   bit rate.
5. If capture runs but the image is wrong, the expected RAW10 layout is a
   2.5 MiB BGGR packed-10 buffer at the start of the dequeued 4 MiB buffer.

### Uncertainties in what I wrote

* **`sc2336_set_flip()` writes `0x3221` bits [2:1] and [6:5] as one mask.**
  Espressif documents the *bit positions* but never exercises them (the driver
  has no flip ioctl), so the exact polarity of "mirror on" is unverified. If
  flip comes out inverted, invert the mask — two lines.
* **`0x3e08` vs `0x3e09`.** The init table writes `0x3e08=0x1f` but the vendor
  driver's gain path writes `0x3e09`, `0x3e06`, `0x3e07` and never touches
  `0x3e08`. So `0x3e08`'s role is inferred, not documented. It does not affect
  this driver because gain is never programmed.
* **`esp_csi_config_s` defaults to RAW10 RAW10.** With no ISP, colours will not
  appear — the data is a Bayer mosaic. That is expected, not a bug.
* I have **not** run `checkpatch` on the four shim headers expecting zero
  warnings; they intentionally use mixed-case FreeRTOS identifiers
  (`QueueHandle_t`, `portMUX_TYPE`, `TickType_t`) so that the vendored ESP-IDF
  sources compile unchanged. The residual findings are that class of false
  positive only.

### Explicitly not claimed

**The camera does not work. It has never been powered on.** What is proven is
that the NuttX-side wiring is complete and that the whole ESP32-P4 MIPI-CSI
stack — vendored HAL, shim, NuttX driver, sensor driver, board file — compiles
and links into a firmware image with no errors and no warnings of our own.
