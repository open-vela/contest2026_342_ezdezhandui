#!/bin/bash
# openvela 环境检测脚本
# Usage: bash detect-env.sh

set +e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass() { echo -e "${GREEN}✓${NC} $1"; }
fail() { echo -e "${RED}✗${NC} $1"; }
warn() { echo -e "${YELLOW}!${NC} $1"; }

ERRORS=0

echo "=========================================="
echo "  openvela 开发环境检测"
echo "=========================================="
echo ""

echo "【1/7】操作系统..."
if [ -f /etc/os-release ]; then
    . /etc/os-release
    if [[ "$NAME" == *"Ubuntu"* ]] && [[ "$VERSION_ID" == "22.04" ]]; then
        pass "Ubuntu 22.04"
    elif [[ "$NAME" == *"Ubuntu"* ]]; then
        warn "Ubuntu $VERSION_ID (推荐 22.04)"
    else
        fail "$NAME $VERSION_ID (需要 Ubuntu 22.04)"
        ERRORS=$((ERRORS + 1))
    fi
else
    fail "无法检测操作系统"
    ERRORS=$((ERRORS + 1))
fi

echo ""
echo "【2/7】运行环境..."
if grep -qi microsoft /proc/version 2>/dev/null; then
    fail "WSL 环境 (不支持编译)"
    ERRORS=$((ERRORS + 1))
elif [ -f /.dockerenv ]; then
    fail "Docker 环境 (不支持编译)"
    ERRORS=$((ERRORS + 1))
else
    pass "原生 Linux"
fi

echo ""
echo "【3/7】内存..."
MEM_MB=$(free -m | awk 'NR==2{print $2}')
MEM_GB=$(( (MEM_MB + 512) / 1024 ))
if [ "$MEM_MB" -ge 15360 ]; then
    pass "内存: ${MEM_GB}GB (${MEM_MB}MB)"
elif [ "$MEM_MB" -ge 7680 ]; then
    warn "内存: ${MEM_GB}GB (${MEM_MB}MB, 推荐 16GB)"
else
    fail "内存: ${MEM_GB}GB (${MEM_MB}MB, 需要至少 8GB)"
    ERRORS=$((ERRORS + 1))
fi

echo ""
echo "【4/7】磁盘空间..."
DISK_AVAIL=$(df -BG . | awk 'NR==2{print $4}' | tr -d 'G')
if [ "$DISK_AVAIL" -ge 40 ]; then
    pass "可用空间: ${DISK_AVAIL}GB"
elif [ "$DISK_AVAIL" -ge 20 ]; then
    warn "可用空间: ${DISK_AVAIL}GB (推荐 40GB)"
else
    fail "可用空间: ${DISK_AVAIL}GB (需要至少 40GB)"
    ERRORS=$((ERRORS + 1))
fi

echo ""
echo "【5/7】基础工具..."
for tool in git cmake python3 make gcc; do
    if command -v $tool &> /dev/null; then
        pass "$tool"
    else
        fail "$tool: 未安装"
        ERRORS=$((ERRORS + 1))
    fi
done

echo ""
echo "【6/7】Git LFS..."
if command -v git-lfs &> /dev/null; then
    pass "git-lfs: $(git-lfs --version | head -1)"
    if git lfs env 2>/dev/null | grep -q "git-lfs"; then
        pass "git-lfs 已初始化"
    else
        warn "git-lfs 未初始化，请运行: git lfs install"
    fi
else
    fail "git-lfs: 未安装 (必须安装!)"
    ERRORS=$((ERRORS + 1))
fi

echo ""
echo "【7/7】Repo 工具..."
if command -v repo &> /dev/null; then
    REPO_VER=$(repo --version 2>&1 | head -1)
    pass "repo: $REPO_VER"
else
    fail "repo: 未安装"
    ERRORS=$((ERRORS + 1))
fi

echo ""
echo "【附加】代码源检测..."

HAS_SSH_KEY=false
SSH_KEY_FILE=""
for key in ~/.ssh/id_ed25519 ~/.ssh/id_rsa ~/.ssh/id_ecdsa; do
    if [ -f "$key" ]; then
        HAS_SSH_KEY=true
        SSH_KEY_FILE="$key"
        break
    fi
done

GITEE_TIME=$(curl -o /dev/null -s -w '%{time_total}' --connect-timeout 5 https://gitee.com 2>/dev/null || echo "999")
GITHUB_TIME=$(curl -o /dev/null -s -w '%{time_total}' --connect-timeout 5 https://github.com 2>/dev/null || echo "999")
GITCODE_TIME=$(curl -o /dev/null -s -w '%{time_total}' --connect-timeout 5 https://gitcode.com 2>/dev/null || echo "999")

check_ssh() {
    local host="$1"
    if [ "$HAS_SSH_KEY" = false ]; then
        echo "NO_KEY"
        return
    fi
    if ssh -T -o ConnectTimeout=5 -o StrictHostKeyChecking=no "git@${host}" 2>&1 | grep -qi "success\|welcome\|Hi "; then
        echo "OK"
    else
        echo "FAIL"
    fi
}

GITEE_SSH=$(check_ssh "gitee.com")
GITHUB_SSH=$(check_ssh "github.com")
GITCODE_SSH=$(check_ssh "gitcode.com")

RECOMMEND=""

echo ""
echo "  源              网络      SSH        响应时间"
echo "  ──────────────────────────────────────────────"

declare -A TIMES SSHS AVAIL
for entry in "gitee:$GITEE_TIME:$GITEE_SSH:Gitee" "github:$GITHUB_TIME:$GITHUB_SSH:GitHub" "gitcode:$GITCODE_TIME:$GITCODE_SSH:GitCode"; do
    IFS=':' read -r key time ssh_status label <<< "$entry"

    TIMES[$key]="$time"
    SSHS[$key]="$ssh_status"

    if (( $(echo "$time < 5" | bc -l) )); then
        NET="✓"
        AVAIL[$key]=1
    else
        NET="✗"
        AVAIL[$key]=0
    fi

    case "$ssh_status" in
        OK)     SSH_DISPLAY="✓ 已验证" ;;
        FAIL)   SSH_DISPLAY="✗ 未添加" ;;
        NO_KEY) SSH_DISPLAY="- 无Key" ;;
    esac

    printf "  %-16s %s         %-10s %s\n" "$label" "$NET" "$SSH_DISPLAY" "${time}s"
done

FASTEST_TIME="999"
for key in gitee github gitcode; do
    if [ "${AVAIL[$key]}" = "1" ] && (( $(echo "${TIMES[$key]} < $FASTEST_TIME" | bc -l) )); then
        FASTEST_TIME="${TIMES[$key]}"
    fi
done

THRESHOLD=1.0
RECOMMEND=""
RECOMMEND_REASON=""

HAS_SSH_SOURCES=""
for key in github gitee gitcode; do
    if [ "${AVAIL[$key]}" = "1" ] && [ "${SSHS[$key]}" = "OK" ]; then
        DIFF=$(echo "${TIMES[$key]} - $FASTEST_TIME" | bc -l)
        if (( $(echo "$DIFF < $THRESHOLD" | bc -l) )); then
            HAS_SSH_SOURCES="$HAS_SSH_SOURCES $key"
        fi
    fi
done

if [ -n "$HAS_SSH_SOURCES" ]; then
    for prefer in github gitee gitcode; do
        if echo "$HAS_SSH_SOURCES" | grep -qw "$prefer"; then
            RECOMMEND="$prefer"
            RECOMMEND_REASON="SSH 已验证"
            break
        fi
    done
else
    for key in github gitee gitcode; do
        if [ "${AVAIL[$key]}" = "1" ] && (( $(echo "${TIMES[$key]} - $FASTEST_TIME < $THRESHOLD" | bc -l) )); then
            if [ -z "$RECOMMEND" ]; then
                RECOMMEND="$key"
                RECOMMEND_REASON="响应最快"
            fi
        fi
    done
    if [ -z "$RECOMMEND" ]; then
        for key in gitee github gitcode; do
            if [ "${AVAIL[$key]}" = "1" ]; then
                RECOMMEND="$key"
                RECOMMEND_REASON="可用"
                break
            fi
        done
    fi
fi

echo ""

if [ "$HAS_SSH_KEY" = false ]; then
    warn "未检测到 SSH Key，建议配置（更稳定，无需重复输入密码）"
    echo ""
    echo "  生成 SSH Key:"
    echo "    ssh-keygen -t ed25519 -C \"your_email@example.com\""
    echo ""
    echo "  查看公钥:"
    echo "    cat ~/.ssh/id_ed25519.pub"
    echo ""
    echo "  添加到平台:"
    echo "    Gitee:   https://gitee.com/profile/sshkeys"
    echo "    GitHub:  https://github.com/settings/ssh/new"
    echo "    GitCode: https://gitcode.com/-/user_settings/ssh_keys"
    echo ""
    echo "  参考文档:"
    echo "    https://docs.github.com/en/authentication/connecting-to-github-with-ssh/adding-a-new-ssh-key-to-your-github-account"
fi

if [ -n "$RECOMMEND" ]; then
    echo ""
    echo -e "  ${GREEN}推荐源: $RECOMMEND ($RECOMMEND_REASON)${NC}"

    RECOMMEND_SSH="${SSHS[$RECOMMEND]}"
    if [ "$RECOMMEND_SSH" = "OK" ]; then
        echo -e "  ${GREEN}推荐协议: ssh (已验证通过)${NC}"
    else
        echo -e "  ${YELLOW}推荐协议: ssh (需先配置) 或 https (开箱即用)${NC}"
    fi
fi

echo ""
echo "=========================================="
if [ $ERRORS -eq 0 ]; then
    echo -e "${GREEN}环境检测通过！${NC}"
    if [ -n "$RECOMMEND" ]; then
        echo ""
        echo "下一步:"
        echo "  bash scripts/install-deps.sh"
    fi
else
    echo -e "${RED}发现 $ERRORS 个问题需要解决${NC}"
fi
echo "=========================================="

exit $ERRORS
