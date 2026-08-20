# Build Targets

openvela 支持的编译目标说明。

## 模拟器目标 (Goldfish)

| 目标 | 架构 | 配置路径 |
|------|------|----------|
| **goldfish-arm64** | ARM64 | `vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/` |
| goldfish-arm32 | ARM32 | `vendor/openvela/boards/vela/configs/goldfish-armeabi-v7a-ap/` |

### 编译命令

```bash
# ARM64 (推荐)
./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/ --cmake -j$(nproc)

# ARM32
./build.sh vendor/openvela/boards/vela/configs/goldfish-armeabi-v7a-ap/ --cmake -j$(nproc)
```

### 运行命令

```bash
# ARM64
./emulator.sh cmake_out/vela_goldfish-arm64-v8a-ap/

# ARM32
./emulator.sh cmake_out/vela_goldfish-armeabi-v7a-ap/
```

---

## NuttX 原生目标

openvela 兼容 NuttX 原生目标，可使用 `build.sh` 或 NuttX 原生方式编译。

### 查找可用目标

```bash
cd nuttx
./tools/configure.sh -L | grep <keyword>
```

### 编译方式

**方式 1: build.sh (推荐)**
```bash
./build.sh qemu-armv7a:nsh -j$(nproc)
```

**方式 2: NuttX 原生**
```bash
cd nuttx
./tools/configure.sh -l qemu-armv7a:nsh
make -j$(nproc)
```

### 常用 NuttX 目标

| 目标 | 说明 |
|------|------|
| `qemu-armv7a:nsh` | QEMU ARMv7-A NSH |
| `qemu-armv8a:nsh_smp` | QEMU ARMv8-A SMP |
| `sim:nsh` | 本地模拟器 |

---

## 编译选项

| 选项 | 说明 |
|------|------|
| `--cmake` | 使用 CMake 构建 (推荐) |
| `-j$(nproc)` | 并行编译 |
| `menuconfig` | 打开配置界面 |
| `savedefconfig` | 保存配置 |
| `distclean` | 完全清理 |

### 配置内核

```bash
./build.sh <config> --cmake menuconfig
```

- 按 `/` 搜索配置项
- 按空格切换选中状态
- 选择 Save 保存退出

### 保存配置

```bash
./build.sh <config> --cmake savedefconfig
```

---

## 编译产物

| 构建类型 | 产物目录 | 主要文件 |
|----------|----------|----------|
| CMake (Vela) | `cmake_out/vela_<config>/` | `nuttx`, `nuttx.bin` |
| CMake (NuttX) | `build/` | `nuttx`, `nuttx.bin` |
| Makefile | `nuttx/` | `nuttx`, `nuttx.bin` |

---

## 清理编译

```bash
# 清理 Vela 编译
rm -rf cmake_out/

# 清理 NuttX 编译
cd nuttx && make distclean
```
