# 14_DSI 双缓冲与共享 DW-GDMA 所有权

> 写于 2026-11-06。**"画面撕裂 / 用过相机后面板冻住 / 看门狗误判停摆反复重建"**
> 读这篇。两个问题在这里被拆开：撕裂是**单缓冲**，冻屏是**共享控制器被复位**。

## 一、双缓冲：撕裂的根因是"同一块内存在被写的同时被扫"

原状态：`yres_virtual = yres`，vtable 里**没有 `pandisplay`**。于是 LVGL 直接在
DMA 正在扫描的那块 1.2 MB 上渲染 —— 面板扫到上半屏时 CPU 已经在改下半屏。

### 三个必须同时满足的条件

1. `yres_virtual == yres * 2`
   —— `lv_nuttx_fbdev.c` 判断双缓冲的**唯一**条件，写死在源码里。
2. vtable 里有 `pandisplay`，且它**不在函数里改 DMA 源地址**。
3. 帧边界上才换页。

### 踩到的三个坑

**坑 A：`getplaneinfo()` 无视 `pinfo->display`**

`fbdev_init_mem2()` 会拿 `pinfo.display + 1` 再取一次 planeinfo 来探测第二页。
如果驱动永远返回同一页，探测器拿到的"第二页"和第一页是同一块内存 ——
**双缓冲静默失效**，表现为"以为开了双缓冲但照样撕裂"。

**坑 B：第二页必须"连续"，且 fblen 是整块**

`fbdev_init_mem2()` 里：

```c
offset = phy_mem2 - phy_mem1;
bool is_consecutive = offset == 0;
```

只有 `offset == 0`（两页返回**同一基址**）时，它才把 `fblen` 减半、并自己用
`yres * stride` 作 mmap 偏移去取第二页。

反过来，如果按 display 返回两个不同地址，`offset = 1228800`，
`is_consecutive` 为 false，它就拿**整块 fblen** 去 mmap 一个已经偏移过的位置 ——
落到 `fb_mmap()` 的 `offset + length <= fblen` 检查上直接 `-EINVAL`，
表现为 **`LVGL could not attach to /dev/fb0`**（实测）。

所以正确做法是：

```c
/* 所有页返回同一基址 + 整块 fblen */
memcpy(pinfo, &g_board_plane, sizeof(*pinfo));   /* fblen = 2 页 */

/* 只把 yres_virtual 设成 2 * yres，让上层自己算页 */
.yres_virtual = BOARD_FB_HEIGHT * BOARD_FB_COUNT,
```

**坑 C：`bind_framebuffer()` 的 size 是"一帧"，不是"整块"**

`bind_framebuffer(fb, fb_size, ...)` 的 `fb_size` 同时决定 DW-GDMA 每帧搬多少
字节**和**桥的 `num_pixel_bits`。双缓冲时如果传整块（2 页 = 2.4 MB），
DMA 每帧会连扫两页 —— 面板只显示前 600 行，每帧多搬 1.2 MB。

实测症状：帧完成中断不再规律、`panel_beat` 归零、看门狗 6 秒后误判停摆并去
重建 DSI，然后在共享 GDMA 上撞 Exception 7。

修法：传**一页**，页数通过 `esp_mipi_dsi_set_fb_pages()` 单独告知，
`queue_frame_buffer()` 的越界检查按 `[base, base + pages*frame)` 判定。

### 关键实现：换页在 ISR 里，不在 ioctl 里

```c
/* board_pandisplay(): 只登记"想显示哪页"，不碰硬件 */
(void)esp_mipi_dsi_flush_framebuffer(page, BOARD_FB_PAGE_SIZE);
return esp_mipi_dsi_queue_frame_buffer(page, BOARD_FB_PAGE_SIZE);

/* DSI ISR: 帧边界上才真的换源地址 */
if (priv->pending_fb != NULL && priv->lli_nc != NULL) {
    dw_gdma_ll_lli_set_src_addr(priv->lli_nc, (uint32_t)priv->pending_fb);
    ...
}
```

如果在 `pandisplay()` 里直接改，桥正在读的地址会在帧中间变化 —— 那就是撕裂本身。

### 坑 D：不消费 pan 队列会让 LVGL 永久停刷

LVGL 每帧发一次 `FBIOPAN_DISPLAY`，`fb.c` 把它压进一个 **circbuf**。
而 `lv_nuttx_fbdev` 的刷新定时器用 `poll()` 的 POLLOUT 判断"有没有空闲页"：

```c
if (poll(...) == 0 && 没 POLLOUT && dsc->pinfo.yres_virtual == yres * 2)
    return;     /* 双缓冲且无空闲页：下一拍再试 */
```

而 `fb_poll()` 只在 `!circbuf_is_full(panbuf)` 时报 POLLOUT ——
**没人消费队列时它会一直是满的**。

所以每帧完成回调里必须把队列消费掉：

```c
static void board_frame_done(FAR void *arg)
{
  (void)fb_remove_paninfo(&g_board_vtable, FB_NO_OVERLAY);
  (void)fb_notify_vsync(&g_board_vtable);
}
```

漏掉这一步的实测症状：`commits` 卡在 2、`panel_beat` 归零、看门狗误判停摆
→ 重建 DSI → 在共享 GDMA 上 Exception 7 崩溃。

## 二、共享 DW-GDMA：所有权必须显式，不能靠初始化顺序

ESP32-P4 的 DW-GDMA 只有 **1 组 4 通道**，DSI 和 CSI 共用。两边的心智模型不同：

| | MIPI-DSI | MIPI-CSI |
|---|---|---|
| 通道 | 硬编码 `ESP_MIPI_DSI_DMA_CHAN` | 走 HAL `dw_gdma_new_channel()` 分配 |
| 控制器初始化 | 裸 LL 直接配 | `dw_gdma_hal_init()`（**内部做控制器级 reset**）|
| 释放 | 不释放 | `dw_gdma_hal_deinit()` + **关总线时钟** |

`dw_gdma_hal_init()` 和 `dw_gdma_hal_deinit()` 都是**控制器级**操作：
前者 `dw_gdma_ll_reset()` 会抹掉 DSI 的通道配置，后者会把总线时钟关掉。
真机现象就是"相机退出后面板冻在最后一帧"。

### 解法：HAL 层记 foreign_owner（补丁 0003）

```c
bool already_running = dw_gdma_ll_is_bus_clock_enabled(group_id);
if (!already_running) {
    dw_gdma_ll_reset_register(group_id);
    dw_gdma_hal_init(&group->hal, &hal_config);
} else {
    /* 别人先起来：只绑定上下文，不复位、不关时钟 */
    group->hal.dev = DW_GDMA_LL_GET_HW(group_id);
    dw_gdma_ll_enable_controller(group->hal.dev, true);
    dw_gdma_ll_enable_intr_global(group->hal.dev, true);
    group->foreign_owner = true;
}
```

释放时 `if (!group->foreign_owner) { hal_deinit(); 关时钟; }`。

### 补丁必须进仓库，否则"改了等于没改"

这两个补丁原本只在 `cmake_out/.../esp-hal-3rdparty` 的**工作树**里：

- `rm -rf cmake_out` → 重新 clone → 干净 HAL → 补丁消失
- 换机器 / 另一条对话先建过构建目录 → 同样消失
- 而且**编译不会报错**，只在"相机退出后冻屏"时暴雷

现在它们是 `nuttx/arch/risc-v/src/esp32p4/patches/0003-*.patch` 和 `0004-*.patch`，
由 `vela_apply_hal_patch()` 应用。

**并且应用位置从 `if(NOT IS_DIRECTORY ...)` 里面挪到了外面**：
原来那整段（含补丁应用）只在 HAL 第一次被克隆时执行，构建目录已存在时
新补丁永远不会被应用，而 `cmake .` 也不报错。`vela_apply_hal_patch()` 本身
幂等（先正向 `git apply --check`，失败再反向 check），每次 configure 都跑是安全的。

### 交叉确认：问驱动，不要猜硬件

DSI 侧原来是：

```c
if (!dw_gdma_ll_is_bus_clock_enabled(0)) { 关->复位->开 }
```

总线时钟只是个**代理量**：HAL 只要持有 group handle 就会一直开着它，
"时钟在跑"和"兄弟通道正在推流"不是同一个问题。

现在两边互相导出状态：

```c
bool esp_mipi_dsi_video_dma_is_active(void);   /* DSI 在推流吗 */
bool esp_csi_video_dma_is_active(void);        /* CSI 在采集吗 */
```

DSI 初始化时问 CSI，CSI 初始化时问 DSI，双方都"是"才需要复位。

### 相机退出后的健康检查

不重拉 DSI（实测重拉会 Exception 7），但**必须检查并报告**：

```
CSI: DSI still streaming after camera teardown (expected)
CSI: uninit done (chen0 disable=00000002 del=00000002 dsi_dead=0)
```

`dsi_dead` 非 0 就说明"相机一走，显示就没了"，不再需要靠肉眼发现。

## 三、验证结果

| 指标 | 改前 | 改后 |
|---|---|---|
| `ui:page-rendered`（自检） | — | **PASS**（两页哈希不同） |
| `display:fed` | 5~6 commits / 4 s | 与改前相当，但**无撕裂** |
| 相机开→关 5 轮 | 面板冻住需复位 | **`dsi_dead=0`，面板继续显示** |
| `deskmate --selftest --camera` | — | **28/28 PASS** |
| HAL 补丁可复现性 | 仅存在于构建目录 | 在 `patches/`，每次 configure 自动打 |

## 三点五、多副本陷阱（这一轮又踩了一次）

`nuttx/boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_display.c`
**不是**被编译的那一份（它的 include 是 `"espressif/esp_mipi_dsi.h"`，
而实际编译的 `vendor/espressif/boards/...` 用的是 `"drivers/esp_mipi_dsi.h"`）。

我在编辑板级文件时误用了这个副本，结果：

- `cmake .` 成功、`ninja` 成功、烧录成功 —— **什么都没变**
- 只有 `riscv-none-elf-nm` 查 `board_pandisplay` 不存在才暴露

确认方法（每次都该跑）：

```bash
grep -oE "[/A-Za-z0-9_.-]*esp32p4_display\.c" cmake_out/*/build.ninja | sort -u
# 只应出现 vendor/espressif/... 那一份
```

修法不是"记得改对文件"，而是**把没被编译的副本回退掉** —— 留着它只会让下一个
人再踩一次（`git -C nuttx checkout -- boards/risc-v/esp32p4/esp32p4-function-ev-board/`）。

## 四、复现顺序

```bash
# 1) 清构建目录也能过（补丁在仓库里）
cd /home/ez/share/openvela
./build.sh vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/nsh --cmake -j8

# 2) 打补丁的证据（configure 阶段）
#    -- HAL patch applied: .../0003-openvela-dw-gdma-foreign-owner.patch
#    -- HAL patch applied: .../0004-openvela-csi-start-diag.patch

# 3) 双缓冲是否生效（串口）
#    [dm] display: yres=600 yres_virtual=1200 => fbdev double buffering

# 4) 共享控制器健康度（JTAG）
#    g_dbg_gdma_deinit_calls / _clockoff_calls 在相机启停后不应增长
#    g_dbg_csi_dsi_dead 应为 0
```
