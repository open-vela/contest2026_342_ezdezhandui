/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/include/esp32p4_audio_es8311.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_INCLUDE_ESP32P4_AUDIO_ES8311_H
#define __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_INCLUDE_ESP32P4_AUDIO_ES8311_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_AUDIO_ES8311

/****************************************************************************
 * Name: esp32p4_audio_es8311_initialize
 *
 * Description:
 *   Bring up the board's ES8311 codec path and register it as an audio
 *   device:
 *
 *     I2C0 (GT911 / SC2336 / ES8311 shared control bus)  ->  codec control
 *     I2S0 + GDMA                                        ->  codec data
 *     GPIO53                                             ->  NS4150B PA
 *
 *   The codec is wrapped in the generic PCM decoder, so the result is a WAV
 *   playback device registered as /dev/audio/pcm0.
 *
 *   This is the single entry point for board audio; esp32p4_bringup.c calls
 *   it and nothing else in the board port needs to know how the codec is
 *   wired.
 *
 *   It is idempotent: repeated calls are ignored after the first success.
 *   The I2S and I2C handles are acquired lazily and are reference counted by
 *   the caller (the I2C handle is released again before returning).
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int esp32p4_audio_es8311_initialize(void);

/****************************************************************************
 * Name: esp32p4_audio_pa_set
 *
 * Description:
 *   Drive the NS4150B power-amplifier enable line (PA_CTRL, GPIO53).
 *
 *   esp32p4_audio_es8311_initialize() leaves the amplifier enabled once the
 *   codec register sequence has completed; the codec's own DAC mute is used
 *   to suppress turn-on transients rather than the PA enable.
 *
 *   This entry point exists for applications that want the amplifier off in
 *   the idle case, where it would otherwise amplify the idle noise floor.
 *   Callers should mute the codec first so the transition is not audible.
 *
 * Input Parameters:
 *   enable - true to enable the amplifier, false to disable it.
 *
 * Return Value:
 *   Zero (OK) on success; a negated errno value if GPIO53 could not be
 *   configured as an output.
 *
 ****************************************************************************/

int esp32p4_audio_pa_set(bool enable);

#endif /* CONFIG_ESP32P4_FUNCTION_EV_BOARD_AUDIO_ES8311 */

#endif /* __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_INCLUDE_ESP32P4_AUDIO_ES8311_H */
