#!/bin/bash
# openvela 依赖安装脚本
# Usage: bash install-deps.sh

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info() { echo -e "${GREEN}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; }

echo "=========================================="
echo "  openvela 依赖安装"
echo "=========================================="
echo ""

info "更新软件包列表..."
sudo apt update

info "安装基础开发工具..."
sudo apt install -y git cmake python3 build-essential curl

info "安装 Git LFS..."
if ! command -v git-lfs &> /dev/null; then
    curl -s https://packagecloud.io/install/repositories/github/git-lfs/script.deb.sh | sudo bash
    sudo apt-get install -y git-lfs
fi
git lfs install
echo -e "${GREEN}✓${NC} Git LFS: $(git-lfs --version)"

info "安装 Repo 工具..."
if ! command -v repo &> /dev/null; then
    curl -sSL "https://storage.googleapis.com/git-repo-downloads/repo" > /tmp/repo
    chmod +x /tmp/repo
    sudo mv /tmp/repo /usr/local/bin/repo
fi
echo -e "${GREEN}✓${NC} Repo: $(repo --version 2>&1 | head -1)"

echo ""
echo "=========================================="
echo -e "${GREEN}依赖安装完成！${NC}"
echo ""
echo "下一步:"
echo "  mkdir openvela && cd openvela"
echo "  bash scripts/init-repo.sh"
echo "=========================================="
