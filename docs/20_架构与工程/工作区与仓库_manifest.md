# 工作区与仓库（manifest）

openvela 是 `repo` 多仓工作区：`nuttx`、`apps`、`vendor/espressif`、`packages/ai_agent` 等各是一个 git 仓，
靠 `.repo/manifests/` 下的 xml 组合。本项目的交付是一份**自己的 manifest**，把作品仓的文件映射回工作区位置。

## 本项目的交付映射

- 作品仓 `contest2026_342_ezdezhandui` 作为 self-ref 项目出现在 manifest 里；
- 仓内 4 棵文件树映射回工作区：

| 作品仓内 | 工作区位置 |
|---|---|
| `nuttx/**` | `nuttx/**` |
| `board/esp32p4/**` | `vendor/espressif/boards/esp32p4/**` |
| `app/apps/**` | `apps/**` |
| `app/ai_agent/**` | `packages/ai_agent/**` |

- 映射由 `<copyfile src="..." dest="..."/>` 表达（`src` 相对作品仓、`dest` 相对工作区根）。

### 混合策略（2026-11-01 起）

`<copyfile>` **只支持文件**（repo 工具对目录直接报错），所以：

- **整棵子树都属于本项目**（该目录在工作区里 0 个 tracked 文件）、文件数 ≥ 20、且
  `src`/`dest` 去前缀后逐文件一致 → 用**一条 `<linkfile>`**（symlink）代替逐文件条目；
- 其余仍用 `<copyfile>` 逐文件声明。

当前结果：**3 条 linkfile 替代 489 条 copyfile**，清单从 754 条降到 **231 条 copyfile + 3 条 linkfile**：

| linkfile | 文件数 |
|---|---|
| `nuttx/arch/risc-v/src/esp32p4` → 同名 | 407 |
| `board/esp32p4/esp32p4-function-ev-board` → `vendor/espressif/boards/…` | 59 |
| `board/esp32p4/esp32p4-tab5` → `vendor/espressif/boards/…` | 23 |

关闭方式：`gen_manifest_copyfiles.py --no-linkfile`（回落为全 copyfile）；阈值可调
`--linkfile-threshold N`。模拟部署校验：copyfile 231 条内容全一致、linkfile 3 条解析正确。

## 两个必须知道的限制

1. **`<copyfile>` 不支持目录**：repo 工具对目录会直接报错（"copying from directory not supported"），
   所以"整文件夹替换"要靠逐个文件条目，或改用 `<linkfile>`（symlink，可指向目录）。
2. **manifest 不能表达"删除"**：交付只能新增/覆盖文件；所以共享层里被我们改过的 tracked 文件
   只能留在原地（不能"搬走"，否则交付会缺文件）。

## 只读保护区

另一条对话正在开发模拟器 UI，以下路径**不交付、不同步**：

```
apps/examples/deskmate
nuttx/drivers/net/wifi_sim.c
nuttx/boards/sim/sim/sim/configs/deskmate
vendor/openvela
```

对账与受控同步见 `tools/engineering/check_consistency.py` 与 `sync_and_restore.sh`。

## 公共层（`common/espressif`）最小交付集

**原则**：公共层的改动一律挪进独立文件（P4 侧 `arch/risc-v/src/esp32p4/**`）；只有当基线版本会导致构建/启动失败（真 BUG）时才在公共层保留交付。当前**例外清单为空**。

**判定依据**（脚本可复跑：`tools/engineering/shared_minimal.py`）：

1. 应用构建图（`build.ninja`）里真正编译的公共层源文件：**19 个**；
2. 这些编译单元 + 全部 P4 编译单元跑 `-MM` 得到的直属公共层头闭包：**16 个**；
3. 公共层构建文件（`Kconfig`/`Make.defs`/`CMakeLists.txt`/`Bootloader.*`/`Wireless.*`/`esp_ulp.*`/`ulp*`）：**11 个**；
4. ⇒ 最小集 **46 个**；不交付 **67 个**（既不被编译、也不被 include，评审端用基线版本）。

**永不交付**：`common/espressif/platform_include/sys/lock.h`（本项目新增的兼容副本；P4 侧 `esp32p4/esp_hosted/compat/platform_include/sys/lock.h` 内容相同且在 include 路径上，共享层没有任何构建文件引用它）。

**实测验证（2026-11-01）**：把这 67 个文件在工作区还原成基线版本（tracked 用 `git checkout HEAD --`，untracked 移走）后：

- 应用构建 3 秒通过 ⇒ 证明确实**不在构建图中**；
- 烧写（含 MCUboot 引导器路径）后真机三项复现：`MCUboot` → `NuttShell`、`connected to "Hello_World_2.4G" rssi=-49`、`DhcpDnsStatus iface=0 link=1 dhcp=1 ip=192.168.1.9`、`wlan0 address 192.168.1.9 mask 255.255.255.0 gw 192.168.1.1`（证据目录 `logs/20261101-211034_shared-minimal`）。

**结果**：manifest 从 231 条 copyfile 降到 **163 条**（另有 3 条 linkfile）；公共层条目 113 → **45**，且其中**没有任何我们改过的文件**。

**残余风险**：若评审端打开了某个我们关闭的 `CONFIG_ESPRESSIF_*`，被裁掉的文件会退回基线版本 —— 该功能可能不可用，但**不会导致构建失败**（基线版本存在）。

**回退**：`python3 tools/engineering/gen_manifest_copyfiles.py --no-shared-minimal`（一条命令恢复交付全部公共层文件）。

## 清空验证（clean-room，2026-11-01）

做法：把工作区**强制还原成评审端基线**（tracked 全部 `git checkout HEAD --`、untracked 全部删除、
linkfile 子树整棵移走），再**只按 manifest 部署**（`<copyfile>` 复制 + `<linkfile>` 建符号链接），
得到与评审端 `repo sync` 后一致的树。结果与经验：

- 部署 163 条 copyfile + 3 条 linkfile 后，工作区 `nuttx/arch/risc-v/src/esp32p4` 等三处变为
  指向作品仓的符号链接（与评审端一致），`check_consistency.py` ✅ 一致；
- 坑 1：**不要复用旧构建目录**再重跑 CMake —— 重跑时 `NUTTX_DEFCONFIG` 环境变量丢失，会在
  `cmake/nuttx_kconfig.cmake:48` 报 `string sub-command REGEX ... needs at least 5 arguments`。
  评审端是全新 configure，不会踩到；本机要重配就换一个全新构建目录；
- 坑 2：**全新构建目录需要重新 clone `esp-hal-3rdparty`（约 700 MB）**。本机今日实测
  ~30 MB / 4 分钟，无法在本轮内完成整个 clean-room 构建；hal 缓存已放回
  `cmake_out/esp32p4-function-ev-board_nsh/` 供下次配置复用；
- 内容层面的"编译 + 烧录 + 真机三项证据"已由共享层最小集实验完成（见上一节，证据目录
  `logs/20261101-211034_shared-minimal`），那次的树 = 交付内容（其余文件为基线版本）。
