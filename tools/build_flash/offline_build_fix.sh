#!/bin/bash
# offline_build_fix.sh — 让构建在「无法 clone MCUboot / esp-hal」的环境里也能跑完
#
# 背景：openvela 的 CMake 会为 bootloader（MCUboot）和 esp-hal 建 ExternalProject，
# 其中若干步要 `git clone` / `git submodule update` 上 GB 的仓库。在离线或
# 网络不稳的环境里这些步骤会挂住，构建永远到不了编译阶段。
#
# 本脚本把这些步骤在**构建目录内**替换成 no-op 并推进 stamp：
#   · 屏蔽 *gitclone.cmake / *gitupdate.cmake
#   · 把 build.ninja 里 bootloader 的 update/patch/configure/build/install
#     换成 touch 对应 stamp
#   · 去掉 mbedtls 的 `git submodule update` 前缀
#
# ⚠️ 只作用于构建目录（cmake_out/...），**交付树一个字节都不动**。
# ⚠️ 每次 CMake 重新生成 build.ninja 之后都要再跑一次（这是它存在的原因：
#    之前靠手工执行，反复丢失，所以固化成一个脚本）。
#
# 用法: tools/build_flash/offline_build_fix.sh [构建目录]
set -u

BUILD="${1:-/home/ez/share/openvela/cmake_out/esp32p4-function-ev-board_nsh}"
[ -d "$BUILD" ] || { echo "没有这个构建目录: $BUILD" >&2; exit 1; }

# 1) 屏蔽 clone/update 脚本
n=0
while IFS= read -r f; do
  printf '%s\n' '# offline build: sources already present' 'message(STATUS "clone skipped")' > "$f"
  n=$((n+1))
done < <(find "$BUILD" -name "*gitclone.cmake" -o -name "*gitupdate.cmake" 2>/dev/null)
echo "屏蔽 clone/update 脚本: $n"

# 2) build.ninja: 把 bootloader 的克隆相关步骤换成 touch stamp
python3 - "$BUILD" <<'PY'
import re, sys, os
build = sys.argv[1]
ninja = os.path.join(build, "build.ninja")
if not os.path.exists(ninja):
    print("没有 build.ninja，跳过"); sys.exit(0)
stamp = os.path.join(build, "arch/risc-v/src/exclude_chip/espressif/"
                            "bootloader-prefix/src/bootloader-stamp")
s = open(ninja).read()
total = 0
for step in ("update", "patch", "configure", "build", "install"):
    s, k = re.subn(
        r'(build [^\n]*bootloader-' + step + r': CUSTOM_COMMAND[^\n]*\n)\s*COMMAND = [^\n]*\n',
        lambda m, st=step: m.group(1) + "  COMMAND = cd " + build.rsplit('/',1)[0] +
                           " && touch " + stamp + "/bootloader-" + st + "\n", s)
    total += k
before = s.count("git submodule --quiet update --init --recursive ext/mbedtls-3.6.0 &&")
s = s.replace("git submodule --quiet update --init --recursive ext/mbedtls-3.6.0 &&", "true &&")
open(ninja, "w").write(s)
print(f"build.ninja: 改写步骤 {total} 处, mbedtls 子模块命令替换 {before} 处")
os.makedirs(stamp, exist_ok=True)
for st in ("mkdir","download","update","patch","configure","build","install","done"):
    p = os.path.join(stamp, "bootloader-" + st)
    open(p, "a").close()
    os.utime(p)
print("stamp 已推进")
PY
