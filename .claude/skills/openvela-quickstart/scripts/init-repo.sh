#!/bin/bash
# openvela 仓库初始化脚本
# Usage: bash init-repo.sh <source> <branch> <protocol>
#   source:   gitee | github | gitcode
#   branch:   dev | trunk
#   protocol: ssh | https
#
# SSH 检测: bash init-repo.sh <source> check-ssh
#   输出 SSH 状态供 AI 判断，不执行 repo init
#
# Example:
#   bash init-repo.sh gitee check-ssh     # 检测 SSH 状态
#   bash init-repo.sh gitee dev ssh       # 执行初始化
#   bash init-repo.sh gitee trunk https   # 执行初始化

set -e

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m'

info() { echo -e "${GREEN}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }

SOURCE="${1:-}"
SECOND="${2:-}"
PROTOCOL="${3:-}"

if [ -z "$SOURCE" ]; then
    echo "Usage:"
    echo "  bash init-repo.sh <source> check-ssh          # 检测 SSH 状态"
    echo "  bash init-repo.sh <source> <branch> <protocol> # 执行初始化"
    echo ""
    echo "  source:   gitee | github | gitcode"
    echo "  branch:   dev | trunk"
    echo "  protocol: ssh | https"
    echo ""
    echo "  dev   - 功能开发、学习体验（推荐新手）"
    echo "  trunk - 开发板适配、项目开发（稳定版本）"
    echo ""
    echo "Example:"
    echo "  bash init-repo.sh gitee check-ssh"
    echo "  bash init-repo.sh gitee dev ssh"
    echo "  bash init-repo.sh gitee trunk https"
    exit 1
fi

case "$SOURCE" in
    gitee)   SSH_HOST="gitee.com" ;;
    github)  SSH_HOST="github.com" ;;
    gitcode) SSH_HOST="gitcode.com" ;;
    *)
        echo -e "${RED}未知源: $SOURCE${NC}"
        echo "支持: gitee, github, gitcode"
        exit 1
        ;;
esac

if [ "$SECOND" = "check-ssh" ]; then
    echo "=========================================="
    echo "  SSH 连接检测 ($SOURCE)"
    echo "=========================================="
    echo ""

    HAS_KEY=false
    KEY_FILE=""
    for key in ~/.ssh/id_ed25519 ~/.ssh/id_rsa ~/.ssh/id_ecdsa; do
        if [ -f "$key" ]; then
            HAS_KEY=true
            KEY_FILE="$key"
            break
        fi
    done

    if [ "$HAS_KEY" = false ]; then
        echo "SSH_STATUS=NO_KEY"
        echo ""
        warn "未找到 SSH Key"
        echo ""
        echo "  推荐配置 SSH（更稳定，无需重复输入密码）"
        echo ""
        echo "  生成 SSH Key:"
        echo "    ssh-keygen -t ed25519 -C \"your_email@example.com\""
        echo ""
        echo "  查看公钥:"
        echo "    cat ~/.ssh/id_ed25519.pub"
        echo ""
        echo "  添加到 $SOURCE:"
        case "$SOURCE" in
            github)  echo "    https://github.com/settings/ssh/new" ;;
            gitee)   echo "    https://gitee.com/profile/sshkeys" ;;
            gitcode) echo "    https://gitcode.com/-/user_settings/ssh_keys" ;;
        esac
        echo ""
        echo "  参考文档:"
        echo "    https://docs.github.com/en/authentication/connecting-to-github-with-ssh/adding-a-new-ssh-key-to-your-github-account"
        exit 0
    fi

    info "检测到 SSH Key: $KEY_FILE"
    info "测试 $SSH_HOST 连接..."

    if ssh -T -o ConnectTimeout=5 -o StrictHostKeyChecking=no "git@${SSH_HOST}" 2>&1 | grep -qi "success\|welcome\|Hi "; then
        echo "SSH_STATUS=OK"
        echo ""
        info "SSH 连接 $SOURCE 成功 ✓"
        echo ""
        echo "  推荐使用 SSH 协议"
    else
        echo "SSH_STATUS=KEY_NOT_ADDED"
        echo ""
        warn "SSH Key 存在但未添加到 $SOURCE"
        echo ""
        echo "  公钥内容:"
        echo "    $(cat "${KEY_FILE}.pub" 2>/dev/null || echo '未找到公钥文件')"
        echo ""
        echo "  添加到 $SOURCE:"
        case "$SOURCE" in
            github)  echo "    https://github.com/settings/ssh/new" ;;
            gitee)   echo "    https://gitee.com/profile/sshkeys" ;;
            gitcode) echo "    https://gitcode.com/-/user_settings/ssh_keys" ;;
        esac
        echo ""
        echo "  参考文档:"
        echo "    https://docs.github.com/en/authentication/connecting-to-github-with-ssh/adding-a-new-ssh-key-to-your-github-account"
    fi

    echo ""
    echo "=========================================="
    echo "  可选协议: ssh (推荐) | https"
    echo "=========================================="
    exit 0
fi

BRANCH="$SECOND"
case "$BRANCH" in
    dev|trunk) ;;
    *)
        echo -e "${RED}未知分支: $BRANCH${NC}"
        echo "支持: dev, trunk"
        exit 1
        ;;
esac

if [ -z "$PROTOCOL" ]; then
    echo -e "${RED}请指定协议: ssh 或 https${NC}"
    echo "用法: bash init-repo.sh $SOURCE $BRANCH <ssh|https>"
    echo "或先检测: bash init-repo.sh $SOURCE check-ssh"
    exit 1
fi

case "$PROTOCOL" in
    ssh|https) ;;
    *)
        echo -e "${RED}未知协议: $PROTOCOL${NC}"
        echo "支持: ssh, https"
        exit 1
        ;;
esac

if [ -d ".repo" ]; then
    warn "检测到已存在 .repo 目录，将重新初始化"
    rm -rf .repo
fi

echo ""
echo "=========================================="
echo "  openvela 仓库初始化"
echo "=========================================="
echo ""
echo -e "  源:   ${BOLD}${SOURCE}${NC}"
echo -e "  分支: ${BOLD}${BRANCH}${NC}"
echo -e "  协议: ${BOLD}${PROTOCOL}${NC}"
echo ""

case "$SOURCE-$PROTOCOL" in
    gitee-https)
        repo init -u https://gitee.com/open-vela/manifests.git -b "$BRANCH" -m openvela.xml \
            --repo-url=https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/ --git-lfs
        ;;
    gitee-ssh)
        repo init -u ssh://git@gitee.com/open-vela/manifests.git -b "$BRANCH" -m openvela.xml \
            --repo-url=https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/ --git-lfs
        ;;
    github-https)
        repo init -u https://github.com/open-vela/manifests.git -b "$BRANCH" -m openvela.xml --git-lfs
        ;;
    github-ssh)
        repo init -u ssh://git@github.com/open-vela/manifests.git -b "$BRANCH" -m openvela.xml --git-lfs
        ;;
    gitcode-https)
        repo init -u https://gitcode.com/open-vela/manifests.git -b "$BRANCH" -m openvela.xml \
            --repo-url=https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/ --git-lfs
        ;;
    gitcode-ssh)
        repo init -u ssh://git@gitcode.com/open-vela/manifests.git -b "$BRANCH" -m openvela.xml \
            --repo-url=https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/ --git-lfs
        ;;
    *)
        echo -e "${RED}未知组合: $SOURCE-$PROTOCOL${NC}"
        exit 1
        ;;
esac

echo ""
echo "=========================================="
echo -e "${GREEN}仓库初始化完成！${NC}"
echo ""
echo "下一步:"
echo "  repo sync -c -j8"
echo ""
echo "首次同步耗时较长，中断后可重复执行。"
echo "=========================================="
