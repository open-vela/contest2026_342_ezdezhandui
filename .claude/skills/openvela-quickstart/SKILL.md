---
name: openvela-quickstart
description: "openvela RTOS 开发环境快速搭建。自动检测环境、安装依赖、智能选择源、下载编译源码、运行模拟器。Trigger: 首次搭建 openvela 环境、repo sync/init 失败、编译报错、模拟器无法启动、询问 openvela 环境问题。"
compatibility: "Ubuntu 22.04 only. 不支持 WSL/Docker。需要 16GB RAM, 40GB 磁盘。"
---

# openvela Quickstart

从零搭建 openvela 开发环境，直到模拟器成功运行。

## Quick Start

```bash
# 一键检测环境
bash scripts/detect-env.sh

# 一键安装依赖
bash scripts/install-deps.sh

# 智能初始化仓库（自动选择最优源）
mkdir openvela && cd openvela
bash scripts/init-repo.sh

# 同步代码
repo sync -c -j8

# 编译
./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/ --cmake -j$(nproc)

# 运行模拟器
./emulator.sh cmake_out/vela_goldfish-arm64-v8a-ap/
```

**成功标志**: 出现 `goldfish-armv8a-ap>` 提示符

## 环境要求

| 项目 | 要求 | 检测命令 |
|------|------|----------|
| 操作系统 | Ubuntu 22.04 | `cat /etc/os-release` |
| 内存 | ≥ 16 GB | `free -h` |
| 磁盘 | ≥ 40 GB | `df -h .` |
| 网络 | GitHub/Gitee 可访问 | `curl -s https://gitee.com` |

**限制**: 不支持 WSL、Docker 环境。

## Workflow

### Step 1: 环境检测

```bash
bash scripts/detect-env.sh
```

检测项: 操作系统、WSL/Docker、内存、磁盘、工具链、代码源测速。

脚本会输出各源的响应时间和推荐源，AI 根据结果询问用户选择。

### Step 2: 安装依赖

```bash
bash scripts/install-deps.sh
```

或手动安装:

| 组件 | 命令 |
|------|------|
| 基础工具 | `sudo apt install -y git cmake python3 build-essential curl` |
| Git LFS | `curl -s https://packagecloud.io/install/repositories/github/git-lfs/script.deb.sh \| sudo bash && sudo apt-get install -y git-lfs && git lfs install` |
| Repo | `curl -sSL "https://storage.googleapis.com/git-repo-downloads/repo" > repo && chmod +x repo && sudo mv repo /usr/local/bin/` |

**重要**: Git LFS 必须安装，否则大文件损坏（只有几 KB 指针文件）。

### Step 3: 初始化仓库

**流程: AI 先检测 SSH → 引导用户选择协议 → 执行初始化**

1) 检测 SSH 状态:
```bash
bash scripts/init-repo.sh <source> check-ssh
```

脚本输出 `SSH_STATUS=OK|NO_KEY|KEY_NOT_ADDED`，AI 根据结果引导：

| SSH 状态 | AI 引导 |
|----------|---------|
| `OK` | 推荐 SSH，询问用户确认 |
| `NO_KEY` | 教用户生成 SSH Key 并添加到平台，询问用户选 SSH 还是 HTTPS |
| `KEY_NOT_ADDED` | 教用户添加公钥到平台，询问用户选 SSH 还是 HTTPS |

2) 用户选择后执行初始化:
```bash
mkdir openvela && cd openvela
bash scripts/init-repo.sh <source> <branch> <protocol>
```

示例:
```bash
bash scripts/init-repo.sh gitee dev ssh       # Gitee dev 分支 SSH
bash scripts/init-repo.sh gitee trunk https   # Gitee trunk 分支 HTTPS
bash scripts/init-repo.sh github dev ssh      # GitHub dev 分支 SSH
```

手动初始化:

| 源 | 命令 |
|-----|------|
| Gitee SSH (推荐) | `repo init -u ssh://git@gitee.com/open-vela/manifests.git -b dev -m openvela.xml --repo-url=https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/ --git-lfs` |
| Gitee HTTPS | `repo init -u https://gitee.com/open-vela/manifests.git -b dev -m openvela.xml --repo-url=https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/ --git-lfs` |
| GitHub SSH | `repo init -u ssh://git@github.com/open-vela/manifests.git -b dev -m openvela.xml --git-lfs` |
| GitHub HTTPS | `repo init -u https://github.com/open-vela/manifests.git -b dev -m openvela.xml --git-lfs` |

### Step 4: 同步代码

**注意**: repo sync 耗时较长（30分钟-2小时），AI 不能直接执行（会超时）。使用后台方式运行：

```bash
nohup repo sync -c -j8 > repo_sync.log 2>&1 &
echo "PID: $!"
```

检查同步进度：
```bash
tail -20 repo_sync.log
```

检查是否完成：
```bash
ps aux | grep "repo sync" | grep -v grep && echo "同步中..." || echo "同步已结束"
```

| 网络环境 | 预估时间 | 代码量 |
|----------|----------|--------|
| 国内 Gitee | 30-60 分钟 | ~20 GB |
| GitHub (直连) | 1-2 小时 | ~20 GB |
| GitHub (代理) | 30-60 分钟 | ~20 GB |

首次同步耗时较长，中断后可重复执行（支持断点续传）。如果长时间无输出，不是卡住，是在下载大文件。

### Step 5: 编译

AI 需询问用户选择模拟器版本：

| 模拟器 | 配置路径 | 说明 |
|--------|----------|------|
| **goldfish-arm64 (v8a)** | `vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/` | 64 位，推荐 |
| goldfish-arm32 (v7a) | `vendor/openvela/boards/vela/configs/goldfish-armeabi-v7a-ap/` | 32 位 |

**注意**: 编译耗时较长（15-60分钟），使用后台方式运行：

```bash
nohup ./build.sh <config> --cmake -j$(nproc) > build.log 2>&1 &
echo "PID: $!"
```

检查编译进度：
```bash
tail -20 build.log
```

检查是否完成：
```bash
ps aux | grep "build.sh" | grep -v grep && echo "编译中..." || echo "编译已结束"
```

检查编译结果：
```bash
tail -5 build.log | grep -i "error" && echo "编译失败" || echo "编译成功"
ls cmake_out/vela_<target>/nuttx && echo "产物存在" || echo "产物不存在"
```

| 配置 | 预估时间 |
|------|----------|
| 8 核 16GB | 15-30 分钟 |
| 4 核 8GB | 30-60 分钟 |

产物位置: `cmake_out/vela_<target>/nuttx`

其他编译目标见 [references/build-targets.md](references/build-targets.md)

### Step 6: 运行模拟器

```bash
./emulator.sh cmake_out/vela_<target>/
```

## Error Handling

| 错误 | 症状 | 快速解决 |
|------|------|----------|
| SSH 认证失败 | `Permission denied (publickey)` | 改用 HTTPS 或配置 SSH Key |
| Google 源不可访问 | `Failed to connect to gerrit.googlesource.com` | 加 `--repo-url=https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/` |
| 网络中断 | `fatal: early EOF` | `repo sync -c -j4` 减少并发重试 |
| 内存不足 | 进程被 kill | `sudo systemctl stop systemd-oomd` 或减少 `-j` 数 |
| LFS 文件损坏 | 大文件只有几 KB | `git lfs install && git lfs pull` |
| Qt 插件失败 | `No Qt platform plugin` | 源码路径不能有中文 |
| Kconfig 语法错误 | `unknown statement 'osource'` | `sudo pip3 install kconfiglib` |
| 缺少 libpulse | `cannot open shared object file: libpulse.so.0` | `sudo apt install libpulse-dev` |
| dev 分支不稳定 | 编译报错或产物不完整 | 切换 trunk 分支: `bash scripts/init-repo.sh <source> trunk` |
| 模拟器找不到 | `run_emulator.sh: 没有那个文件或目录` | 使用 `./emulator.sh cmake_out/vela_<target>/` |
| WSL 模拟器报错 | `getsockopt: Invalid argument (22)` | WSL 不支持，用原生 Ubuntu 或加 `-no-window` |
| 编译失败 | 各种错误 | `rm -rf cmake_out/` 清理后重编 |

详细排查见 [references/troubleshooting.md](references/troubleshooting.md)

## Command Reference

| 任务 | 命令 |
|------|------|
| 检测环境 | `bash scripts/detect-env.sh` |
| 安装依赖 | `bash scripts/install-deps.sh` |
| 初始化仓库 | `bash scripts/init-repo.sh` |
| 同步代码 | `repo sync -c -j8` |
| 编译 | `./build.sh <config> --cmake -j$(nproc)` |
| 运行模拟器 | `./emulator.sh <output_dir>` |
| 清理编译 | `rm -rf cmake_out/` |
| 配置内核 | `./build.sh <config> --cmake menuconfig` |
| 保存配置 | `./build.sh <config> --cmake savedefconfig` |

## Resources

**Scripts:**
- [scripts/detect-env.sh](scripts/detect-env.sh) - 环境检测
- [scripts/install-deps.sh](scripts/install-deps.sh) - 依赖安装
- [scripts/init-repo.sh](scripts/init-repo.sh) - 仓库初始化

**References:**
- [references/troubleshooting.md](references/troubleshooting.md) - 详细问题排查
- [references/build-targets.md](references/build-targets.md) - 编译目标说明

**Official Docs:**
- [快速入门](https://doc.openvela.com/document?id=847&version=dev&language=cn)
- [常见问题](https://doc.openvela.com/document?id=590&version=dev&language=cn)
- [GitHub](https://github.com/open-vela) | [Gitee](https://gitee.com/open-vela)
