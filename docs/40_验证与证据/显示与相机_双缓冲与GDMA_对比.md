# 显示与相机：双缓冲 + 共享 DW-GDMA 所有权（2026-11-06）

> 与 [触摸采样率改造_前后对比.md](触摸采样率改造_前后对比.md) 同一轮的显示侧工作。
> 判据全部来自串口日志与 JTAG 符号，不来自观感。

## 一、结论

| 项目 | 改前 | 改后 |
|---|---|---|
| 帧缓冲 | 单页，`yres_virtual = yres` | **双页，`yres_virtual = 2*yres`** |
| `pandisplay` | vtable 里**没有** | 有，且在帧边界换页 |
| 撕裂 | 无法消除（同一块内存在被写的同时被扫） | **消除**（换页发生在 ISR 的帧边界） |
| `ui:page-rendered`（自检） | PASS（但只比同一页） | **PASS，两页哈希不同**（`0x25bd936a` vs `0x661f4dd3`） |
| 相机退出后 DSI | 冻在最后一帧，需复位 | **`dsi_dead=0`，继续显示** |
| HAL 补丁 | 只在构建目录，删构建目录即丢 | **在 `patches/` 里，每次 configure 自动打** |
| `deskmate --selftest --camera` | — | **28/28 PASS** |
| 相机单帧转换 | 每帧都转（峰值 220 ms） | **每 2 帧转 1 次**，行映射去掉除法 |

## 二、双缓冲的四个坑（每一个都单独让功能静默失效）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| A | 以为开了双缓冲但照样撕裂 | `getplaneinfo()` 无视 `pinfo->display`，探测器拿到的"第二页"是第一页 | 按 `display` 返回，或（更符合 LVGL 约定）统一返回同一基址 |
| B | `LVGL could not attach to /dev/fb0` | 返回两个不同地址 → `is_consecutive=false` → LVGL 用整块 fblen mmap 已偏移的位置 → `fb_mmap` 的 `offset+length<=fblen` 失败 | **所有页返回同一基址 + 整块 fblen**，让上层自己按 `yres*stride` 算页 |
| C | `panel_beat` 归零、看门狗误判停摆、随后 Exception 7 | `bind_framebuffer()` 的 size 传了整块（2 页），DMA 每帧连扫两页 | 传**一页**；页数用 `esp_mipi_dsi_set_fb_pages()` 单独告知 |
| D | `commits` 卡在 2，LVGL 永久停刷 | `fblen` 对应的 **pan 队列（circbuf）没人消费** → `fb_poll()` 永远不报 POLLOUT → 刷新定时器每拍都 return | 帧完成回调里 `fb_remove_paninfo()` + `fb_notify_vsync()` |

## 三、共享 DW-GDMA

ESP32-P4 只有 **1 组 4 通道**，DSI 与 CSI 共用。两边的控制器级操作互不知情：

- `dw_gdma_hal_init()` 内部 `dw_gdma_ll_reset()` —— 抹掉 DSI 的通道配置
- `dw_gdma_hal_deinit()` + 关总线时钟 —— 相机退出时把正在推流的 DSI 打死

修法（HAL 补丁 `0003`）：acquire 时若总线时钟已在跑，判为 `foreign_owner`，
只绑定上下文不复位；release 时 `foreign_owner` 不做 deinit、不关时钟。

交叉确认改用驱动状态而不是硬件代理量：

```c
bool esp_mipi_dsi_video_dma_is_active(void);
bool esp_csi_video_dma_is_active(void);
```

## 四、补丁入库（P0 工程债）

| 之前 | 之后 |
|---|---|
| 补丁只在 `cmake_out/.../esp-hal-3rdparty` 工作树里 | `nuttx/arch/risc-v/src/esp32p4/patches/0003-*.patch`、`0004-*.patch` |
| 补丁应用代码在 `if(NOT IS_DIRECTORY ...)` **里面**，构建目录已存在时永不执行 | 挪到**外面**，每次 configure 都跑（函数本身幂等） |
| `rm -rf cmake_out` → 补丁消失且编译不报错 | 重新 configure 自动重打 |

验证方式：

```bash
# configure 阶段应看到
# -- HAL patch applied: .../0003-openvela-dw-gdma-foreign-owner.patch
# -- HAL patch applied: .../0004-openvela-csi-start-diag.patch

# 符号必须存在
riscv-none-elf-nm cmake_out/*/nuttx | grep g_dbg_gdma_release_calls
```

## 五、相机单帧转换降本

| 项 | 改前 | 改后 |
|---|---|---|
| 跳帧 | 无（每帧都转，转完常被丢弃） | `DM_CAMERA_DECIMATE=2`（每 2 帧转 1 次） |
| 行映射 | 每输出行一次 32 位除法 `(y*qh)/dh` | 增量累加（Bresenham），内层无除法 |
| 新计数 | — | `g_dbg_decim_skip`（因跳帧没转的帧数） |

> 跳帧**不是**降低画质：仍然每帧 `DQBUF`+`QBUF`（不交还缓冲会把驱动侧
> `dropped` 打满），只是不为"没人要的帧"付转换成本

## 六、回归结果

```
DM-SELFTEST RESULT: PASS (28/28)
esp_csi_data_uninit: CSI: DSI still streaming after camera teardown (expected)
esp_csi_data_uninit: CSI: uninit done (chen0 disable=00000002 del=00000002 dsi_dead=0)
```

相机 open→stream→close 之后 `dsi_dead=0` —— 面板没有被相机带走。

## 七、仍未做

- **`CONFIG_FB_SYNC` 未开**：`board_frame_done()` 里的 `fb_notify_vsync()` 目前
  没有等待者。钩子已就位，但"渲染等 vsync"这条链路没有真正启用。
- **PPA 硬件缩放**：代码注释里的实测结论仍是"比 CPU 慢 4 倍"（52.6 vs 12.6 ms/帧），
  未重新评估。
