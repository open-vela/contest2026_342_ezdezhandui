# 踩坑笔记 #01：openvela quickstart（goldfish 模拟器）国内环境实录

> 2026-08-06 于 Ubuntu 22.04.5 / 6GB RAM / 中国网络环境。目标：`goldfish-armv8a-ap>` 提示符。

## 结论先行

✅ 已跑通：`repo init`（gitee 源）→ `repo sync`（48GB/265 仓）→ `./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/ --cmake -j4`（12:54）→ 模拟器启动出现 `NuttShell (NSH) / goldfish-armv8a-ap>`

## 坑 1：系统 repo 2.17 不支持 `--git-lfs`

- 现象：`repo init ... --git-lfs` → `error: no such option: --git-lfs`
- 根因：Ubuntu apt 的 repo 2.17 太老
- 解决：下载新版 launcher（GCS 直连可用）：`curl -sSL https://storage.googleapis.com/git-repo-downloads/repo -o ~/.local/bin/repo && chmod +x`
- ⚠️ `~/.local/bin` 不在默认 PATH，用全路径

## 坑 2：repo init 卡死（gerrit 自举被墙）

- 现象：`repo init` 无输出挂死，`.repo/repo.tmp` 只有 116K
- 根因：repo init 默认从 `gerrit.googlesource.com/git-repo` 自举，国内不可达；`mirrors.tuna.tsinghua.edu.cn/git/git-repo` 的 git clone 实测也挂死（12 分钟 116K）
- 解决：`--repo-url=https://ghfast.top/https://github.com/GerritCodeReview/git-repo.git`（⚠️ 不是 android/git-repo，该仓库已不存在）

## 坑 3：repo sync 部分仓库损坏

- 现象：sync 报 `fatal: 不是 git 仓库：'.repo/projects/vendor/xiaomi/vela.git'`，反复失败
- 根因：同步被中断（此前多次 kill）导致某项目 git 目录损坏
- 解决：`rm -rf .repo/projects/vendor/xiaomi/vela.git vendor/xiaomi/vela` 后重新 `repo sync` 即可（断点续传，48GB 不用重下）

## 坑 4：`/usr/local/bin/python` 是 python2.7 → cmake 找不到 Python3

- 现象：`Could NOT find Python3 ... Wrong major version for the interpreter "/usr/local/bin/python"`
- 根因：机器上 `/usr/local/bin/python → python2`（root 所有，需 sudo 才能改）
- 解决：构建时前置 PATH：`PATH=/usr/bin:$PATH ./build.sh ...`（不动系统文件）

## 坑 5：build 缓存残留 `could not find CMAKE_PROJECT_NAME in Cache`

- 解决：`rm -rf cmake_out` 后重新构建（失败过一次的 configure 会留下坏缓存）

## 坑 6：模拟器 `No initial vela_system image`

- 现象：`ERROR | No initial vela_system image for this configuration!` 后退出
- 解决：把构建产物软链到源码树 `nuttx/` 目录（run_emulator.sh 从 `$TOP_DIR/nuttx/` 找镜像）：
  ```bash
  cd openvela
  ln -sf ../cmake_out/vela_goldfish-arm64-v8a-ap/nuttx        nuttx/nuttx
  ln -sf ../cmake_out/vela_goldfish-arm64-v8a-ap/vela_system.bin nuttx/vela_system.bin
  ln -sf ../cmake_out/vela_goldfish-arm64-v8a-ap/vela_data.bin   nuttx/vela_data.bin
  ```
- ⚠️ 注意相对路径：软链在 `nuttx/` 内，指向 `../cmake_out/...`（不是 `cmake_out/...`）

## 坑 7：无显示器环境跑模拟器

- 环境无 X11 → 用无窗口模式 + 串口输出到 stdout：
  ```bash
  ./emulator.sh vela -no-window -no-audio -gpu off -qemu -nographic > emu.log 2>&1
  ```
- 启动约 40-60s（arm64 TCG 仿真）后日志出现：
  ```
  kvdbd / servicemanager / adbd / telnetd
  NuttShell (NSH)
  goldfish-armv8a-ap>
  ```
- 停止：`kill <qemu PID>`（不要用 `pkill -f` 匹配会出现在自己命令行里的模式——已三次误杀自身）

## 复现命令速查

```bash
export PATH=~/.local/bin:$PATH   # repo 2.65
# 已同步的树在 openvela/
cd openvela
PATH=/usr/bin:$PATH ./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/ --cmake -j4
./emulator.sh vela -no-window -no-audio -gpu off -qemu -nographic > /tmp/emu.log 2>&1 &
```

## 性能备注

- 6GB RAM 机器 `-j4` 编译无压力（峰值 ~1.5GB）
- 全量 sync 48GB / 磁盘需 ≥60GB 余量
- gitee 源全量 sync 实际耗时：约 40 分钟（含两次中断重试）
