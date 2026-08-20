# Troubleshooting

详细问题排查与解决方案。来源：官方文档 + 社区 Issues 实际案例。

## 1. SSH 认证失败

**症状**: `Permission denied (publickey)`

**诊断**:
```bash
ssh -T git@github.com
ssh -T git@gitee.com
```

**解决方案**:

方案 A - 配置 SSH Key:
```bash
ssh-keygen -t ed25519 -C "your_email@example.com"
cat ~/.ssh/id_ed25519.pub
```
将公钥添加到 [GitHub](https://docs.github.com/en/authentication/connecting-to-github-with-ssh) 或 [Gitee](https://gitee.com/help/articles/4191)

方案 B - 改用 HTTPS:
```bash
bash scripts/init-repo.sh gitee https
```

---

## 2. 无法访问 Google 源

**症状**: `Failed to connect to gerrit.googlesource.com`

**解决方案**:

```bash
# 方法1: 使用清华镜像 (init-repo.sh 已自动处理)
repo init xxx --repo-url=https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/

# 方法2: 永久修改 repo
sudo sed -i 's#https://gerrit.googlesource.com/git-repo#https://mirrors.tuna.tsinghua.edu.cn/git/git-repo#' /usr/local/bin/repo

# 方法3: 中科大源 (备用)
repo init xxx --repo-url=https://mirrors.ustc.edu.cn/aosp/git-repo
```

---

## 3. repo sync 网络中断

**症状**: `fatal: early EOF`、下载中断

**解决方案**:

```bash
# 1. 切换到 HTTPS
bash scripts/init-repo.sh gitee https

# 2. 减少并发
repo sync -c -j4

# 3. 断点续传
repo sync -c -j8

# 4. 强制同步
repo sync -c -j8 --force-sync
```

---

## 4. 内存不足 (OOM)

**症状**: 进程被终止，`Killed`

**诊断**:
```bash
free -h
grep -i "oom\|killed" /var/log/syslog | tail -20
```

**解决方案**:

```bash
# 临时关闭 OOM 守护进程
sudo systemctl stop systemd-oomd systemd-oomd.socket

# 完成后重新启用
sudo systemctl start systemd-oomd systemd-oomd.socket
```

或添加 swap:
```bash
sudo fallocate -l 8G /swapfile
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile
```

---

## 5. Git LFS 文件损坏

**症状**: 大文件只有几 KB（指针文件）

**诊断**:
```bash
repo --version          # 需要 >= v2.10
git lfs status
find . -name "*.bin" -size -10k
```

**解决方案**:

```bash
# 重新安装并初始化
sudo apt install git-lfs
git lfs install

# 重新拉取
cd .repo/manifests
git lfs pull
cd ../..
repo sync -c
```

---

## 6. Qt 插件失败

**症状**: `No Qt platform plugin could be initialized`

**原因**: 源码路径包含中文

**解决方案**:
```bash
mv /home/用户/openvela /home/user/openvela
```

---

## 7. Kconfig / kconfiglib 相关错误

> 社区高频问题 (Issues #52, #207, #442)

### 7.1 osource 语法错误

**症状**: `syntax error: unknown statement 'osource'` 或 Kconfig 解析失败

**原因**: kconfig-frontends 不兼容 openvela 的 Kconfig 语法，必须用 kconfiglib

**解决方案**:
```bash
sudo pip3 install kconfiglib
```

如果 pip 安装的版本仍有问题:
```bash
git clone https://github.com/ulfalizer/Kconfiglib.git
cd Kconfiglib
sudo python3 setup.py install
```

### 7.2 缺少 python3-pip

**症状**: `pip3: command not found`

**解决方案**:
```bash
sudo apt install python3-pip
pip3 install kconfiglib
```

安装后**需要重启终端**或重新设置环境变量。

---

## 8. 缺少依赖库

> 社区高频问题 (Issues #10, #90)

### 8.1 缺少 i386 库 (32 位编译)

**症状**: `WARNING: no packages found matching libncurses5` 等一系列 WARNING

**解决方案**:
```bash
sudo dpkg --add-architecture i386
sudo apt-get install -y software-properties-common
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt-get update
sudo apt-get install -y libncurses5 lib32ncurses5-dev libx11-dev:i386 \
    libxext-dev:i386 libpulse-dev:i386 libasound2-dev:i386
```

### 8.2 缺少 libpulse (音频库)

**症状**: `error while loading shared libraries: libpulse.so.0: cannot open shared object file`

**解决方案**:
```bash
sudo apt install libpulse-dev libpulse0
```

### 8.3 缺少工具链

**症状**: `arm-none-eabi-gcc: command not found`

**说明**: openvela 工具链在源码树内，不需要额外安装。检查 PATH 是否正确。

```bash
find . -name "arm-none-eabi-gcc" -o -name "*gcc" | grep prebuilts
```

---

## 9. 编译失败

### 9.1 通用诊断

```bash
# 确认目录
pwd && ls build.sh

# 清理重编
rm -rf cmake_out/
./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/ --cmake -j$(nproc)

# 单线程查看错误
./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/ --cmake -j1 2>&1 | tee build.log
```

### 9.2 dev 分支编译失败

> 社区高频问题 (Issues #442, #456)

**症状**: dev 分支编译报错或编译产物不完整

**原因**: dev 分支包含最新特性，可能不稳定

**解决方案**: 切换到 trunk 分支
```bash
rm -rf .repo
bash scripts/init-repo.sh gitee trunk
repo sync -c -j8
```

### 9.3 编译内存不足

**症状**: `g++: fatal error: Killed signal terminated program`

```bash
# 减少并行数
./build.sh <config> --cmake -j4

# 或添加 swap (见第 4 节)
```

### 9.4 CMake 构建错误

**症状**: `Re-running cmake...` 后报错

**解决方案**:
```bash
# 完全清理后重新编译
rm -rf cmake_out/
./build.sh <config> --cmake -j$(nproc)
```

---

## 10. 模拟器启动失败

### 10.1 run_emulator.sh 找不到

> 社区案例 (Issue #456)

**症状**: `run_emulator.sh: 没有那个文件或目录`

**原因**: 文档更新后编译方式变化，旧的 emulator 路径不适用

**解决方案**:
```bash
# 使用新的 emulator.sh
./emulator.sh cmake_out/vela_goldfish-arm64-v8a-ap/
```

### 10.2 getsockopt: Invalid argument (22)

> 社区案例 (Issue #48)

**症状**: `getsockopt: Invalid argument (22)`

**原因**: 在 WSL 环境中运行模拟器

**解决方案**: WSL 不支持图形界面模拟器。
- 方案 A: 使用原生 Ubuntu
- 方案 B: 无窗口模式 `./emulator.sh <output_dir> -no-window`（仅命令行）

### 10.3 Docker 中模拟器失败

> 社区案例 (Issue #20)

**症状**: Docker 中运行模拟器报错

**解决方案**: Docker 无图形界面，使用无窗口模式:
```bash
./emulator.sh <output_dir> -no-window
```

### 10.4 找不到编译产物

**症状**: `No such file or directory`

```bash
# 确认编译产物存在
ls cmake_out/vela_goldfish-arm64-v8a-ap/nuttx

# 不存在则重新编译
./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/ --cmake -j$(nproc)
```

### 10.5 缺少 QEMU

**症状**: `qemu-system-aarch64: command not found`

```bash
sudo apt install qemu-system-arm qemu-system-aarch64
```

---

## 11. WSL/Docker 环境

> 社区高频问题 (Issues #10, #19, #20, #48)

**说明**: openvela 官方不支持 WSL/Docker 编译和运行。

**常见表现**:
- WSL: 编译可能成功但模拟器报 `getsockopt: Invalid argument`
- Docker: 缺少图形界面，模拟器无法正常启动
- WSL/Docker: 缺少 i386 库导致编译失败

**解决方案**:
- 使用原生 Ubuntu 22.04
- 或在 VMware/VirtualBox 中安装 Ubuntu 22.04
- Docker/WSL 下可尝试 `-no-window` 模式仅使用命令行

---

## 快速诊断命令

```bash
bash scripts/detect-env.sh
```

## 获取帮助

- [快速入门常见问题](https://doc.openvela.com/document?id=590&version=dev&language=cn)
- [开发者常见问题](https://doc.openvela.com/document?id=861&version=dev&language=cn)
- [GitHub Issues](https://github.com/open-vela/docs/issues)
