#!/usr/bin/env bash
# =============================================================================
# lock-revision.sh — 把 self-ref manifest 的 revision 锁定推进到最新 HEAD
#
# 背景
#   作品仓(contest2026_342_ezdezhandui)在 manifest 里以 self-ref project 出现,
#   revision 必须锁 SHA(不能用分支名,评审环境 repo sync 会退回旧版)。
#   每次作品仓有"影响 copyfile 文件树"的新提交后,需把锁更新到该提交,
#   并同步 .repo/manifests 里的官方副本,评审环境才能拿到最新改动。
#
# 用法
#   cd /home/ez/share/openvela/contest2026_342_ezdezhandui
#   board/esp32p4_vela/lock-revision.sh
#
# 效果(全部原子、可重复)
#   1. 作品仓  contest2026_342_ezdezhandui.xml  revision -> $(git rev-parse HEAD)
#   2. 作品仓  commit: chore(manifest): revision 锁定更新至 <short>
#   3. 同步    .repo/manifests/contest2026_342_ezdezhandui.xml(官方 manifest 工作树)
#   4. 提交    .repo/manifests: manifest: revision 同步 <short>
#
# 注意
#   - 锁的目标是"含 copyfile 文件树的提交",通常是作品仓最新 HEAD。
#   - 若在只读 FS/沙箱下运行,需先授予写权限。
#   - push 由用户按需执行:作品仓推 fork,manifests 推官方 gitee。
# =============================================================================
set -euo pipefail

# 脚本位于 <作品仓>/board/esp32p4_vela/lock-revision.sh
# 作品仓根 = 脚本所在目录上两级;repo 根   = 作品仓根上一级(即 /home/ez/share/openvela)
WORKTREE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REPO_ROOT="$(cd "$WORKTREE/.." && pwd)"
XML="$WORKTREE/contest2026_342_ezdezhandui.xml"
MANIFESTS_XML="$REPO_ROOT/.repo/manifests/contest2026_342_ezdezhandui.xml"

# --- 前置校验 ---------------------------------------------------------------
[ -f "$XML" ] || { echo "错误: 找不到 $XML"; exit 1; }
[ -f "$MANIFESTS_XML" ] || { echo "错误: 找不到 $MANIFESTS_XML(需在 repo 工作区内)"; exit 1; }

cd "$WORKTREE"
git diff --quiet || { echo "错误: 作品仓有未提交改动,先提交或 stash 再运行"; exit 1; }

# --- 1. 取最新 HEAD 并更新作品仓 xml -----------------------------------------
NEW_SHA="$(git rev-parse HEAD)"
NEW_SHORT="${NEW_SHA:0:7}"
OLD_SHA="$(grep -oP 'revision="\K[0-9a-f]{40}' "$XML" | head -1 || true)"

if [ "$NEW_SHA" = "$OLD_SHA" ]; then
    echo "已是最新: revision 已锁定 $NEW_SHORT($NEW_SHA),无需更新"
    exit 0
fi

echo "更新 revision 锁定: $OLD_SHA -> $NEW_SHA"
sed -i "s/revision=\"[0-9a-f]\{40\}\"/revision=\"$NEW_SHA\"/" "$XML"

# --- 2. 提交作品仓 -----------------------------------------------------------
git add contest2026_342_ezdezhandui.xml
git commit -m "chore(manifest): revision 锁定更新至 $NEW_SHORT(最新 HEAD)"

# --- 3. 同步官方 manifest 工作树副本 ------------------------------------------
sed -i "s/revision=\"[0-9a-f]\{40\}\"/revision=\"$NEW_SHA\"/" "$MANIFESTS_XML"

# --- 4. 提交 .repo/manifests --------------------------------------------------
git -C "$REPO_ROOT/.repo/manifests" add contest2026_342_ezdezhandui.xml
git -C "$REPO_ROOT/.repo/manifests" commit -m "manifest: revision 同步 $NEW_SHORT" || \
    echo "提示: .repo/manifests 无变化或提交失败,请检查"

echo ""
echo "完成:"
echo "  作品仓 HEAD       : $(git -C "$WORKTREE" rev-parse --short HEAD)"
echo "  revision 锁定     : $NEW_SHORT"
echo ""
echo "下一步(按需 push):"
echo "  git -C $WORKTREE push fork dev-ai-contest-2026:dev-ai-contest-2026"
echo "  git -C $REPO_ROOT/.repo/manifests push origin HEAD:dev-ai-contest-2026"
