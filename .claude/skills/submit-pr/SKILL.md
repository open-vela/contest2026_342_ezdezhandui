---
name: submit-pr
description: "向 openvela 社区提交 Pull Request。通过 Fork 模式提交（先 fork 上游仓库，push 到 fork，再从 fork 向上游提 PR）。支持单仓库或多仓库批量提交、指定文件/分支、自动检测 GitHub/Gitee 平台、分支模糊匹配、MCP 配置引导。Trigger: 提交 PR、提交代码到社区、push 到 openvela、创建 pull request、批量提交、多仓库提交。"
---

# Submit PR to openvela Community (Fork Mode)

通过 Fork 模式向 openvela 开源社区（GitHub/Gitee）提交 Pull Request。

**核心流程**: Fork 上游仓库 → 本地提交 → Push 到 Fork → 从 Fork 向上游创建 PR

## Prerequisites

需要 GitHub MCP server 已配置。如未配置，按 Step 0 引导用户完成。

## Workflow

### Step 0: MCP 配置检查与引导

执行前先检查 MCP 是否可用：

**检测方法**: 尝试调用 `mcp_github_search_repositories` 或类似工具。如果报错说工具不存在，说明 MCP 未配置。

**GitHub MCP 配置引导**:

配置文件位置：
- Kiro: `.kiro/settings/mcp.json`（工作区级）或 `~/.kiro/settings/mcp.json`（用户级）
- Claude Code: `~/.claude/settings/mcp.json`

配置内容：
```json
{
  "mcpServers": {
    "github": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-github"],
      "env": {
        "GITHUB_PERSONAL_ACCESS_TOKEN": "<用户的 GitHub PAT>"
      }
    }
  }
}
```

需要用户提供：
1. GitHub Personal Access Token（需要 `repo` + `workflow` 权限）
   - 获取地址：https://github.com/settings/tokens/new
   - 勾选 `repo` (Full control of private repositories)

**Gitee 配置引导**: 见 [references/mcp-setup.md](references/mcp-setup.md)

配置完成后提示用户**重启 IDE 或重新连接 MCP** 使配置生效。

### Step 1: 检测平台与用户信息

**检测平台**（GitHub / Gitee）：

```bash
bash scripts/detect-repos.sh <repo_root>
```

或手动检测：
```bash
git -C .repo/manifests remote get-url origin
# ssh://git@github.com/open-vela/manifests.git → GitHub
# ssh://git@gitee.com/open-vela/manifests.git → Gitee
```

**获取当前用户 GitHub 用户名**（用于 fork）：

通过 MCP 获取：查看 token 对应的用户名，或询问用户。

### Step 2: 确定目标分支（模糊匹配 + 用户确认）

**分支确定规则**（按优先级）：

1. **用户显式指定**: 用户直接说"提交到 dev 分支"→ 使用 `dev`
2. **模糊匹配**: 检测当前本地分支名，与上游远程分支做模糊匹配
3. **询问用户**: 匹配不到或有多个候选时，列出选项让用户选择

**模糊匹配逻辑**：

```bash
# 获取当前本地分支名
LOCAL_BRANCH=$(git -C <repo_path> branch --show-current)
# 例如: dev-ai-contest-2026, fix/something, feature/xxx

# 获取上游所有远程分支
REMOTE_BRANCHES=$(git -C <repo_path> ls-remote --heads <upstream_remote> | awk '{print $2}' | sed 's|refs/heads/||')
# 例如: dev, trunk, dev-ai-contest-2026, main

# 模糊匹配规则:
# 1. 精确匹配: LOCAL_BRANCH == REMOTE_BRANCH → 直接使用
# 2. 包含匹配: REMOTE_BRANCH 包含 LOCAL_BRANCH 的关键词 → 候选
# 3. 前缀匹配: LOCAL_BRANCH 以 REMOTE_BRANCH 开头 → 候选
```

**交互确认**：

```
🔍 检测到本地分支: dev-ai-contest-2026
   远程分支匹配结果:
   [1] dev-ai-contest-2026 (精确匹配) ← 推荐
   [2] dev
   [3] trunk

   请选择目标分支 (输入编号或分支名):
```

⚠️ **必须等待用户确认目标分支后才能继续。**

### Step 3: Fork 上游仓库

对每个要提交的仓库，检查用户是否已有 fork：

**GitHub 平台**：

```
# 检查 fork 是否存在
mcp_github_get_file_contents(
  owner="<user_github_name>",
  repo="<repo_name>",
  path="README.md"
)
```

如果 404（fork 不存在），执行 fork：

```
mcp_github_fork_repository(
  owner="open-vela",
  repo="<repo_name>"
)
```

Fork 完成后等待几秒（GitHub fork 是异步的）。

**Gitee 平台**：

```bash
curl -X POST "https://gitee.com/api/v5/repos/open-vela/<repo>/forks" \
  -d "access_token=<token>"
```

### Step 4: 确认提交内容

向用户确认以下信息：

| 信息 | 说明 | 示例 |
|------|------|------|
| 仓库列表 | 要提交的仓库路径（支持多个） | `nuttx`, `apps`, `vendor/xxx` |
| 文件列表 | 每个仓库中要提交的文件 | `drivers/foo.c`, `Kconfig` |
| 目标分支 | PR 的 base 分支（Step 2 确定） | `dev`, `dev-ai-contest-2026` |
| PR 标题 | Pull Request 标题 | `fix: resolve mutex issue` |
| commit message | 提交信息 | `fix: add mutex support` |
| 测试说明 | 如何验证此修改（可选） | `编译通过 + 模拟器运行正常` |

**PR 描述由 AI 按模板自动生成**（Summary/Features/Files/Testing 四段式），用户在 Step 5 确认时可修改。

### Step 5: 用户最终确认（⚠️ 必须等待）

展示完整提交计划：

```
📋 提交计划确认：

平台: GitHub
上游仓库: open-vela/<repo>
Fork 仓库: <user>/<repo>
目标分支: dev-ai-contest-2026
新建分支: feat/submit-pr-skill-20260518

仓库 1: .claude
  文件: skills/submit-pr/SKILL.md, scripts/detect-repos.sh, references/mcp-setup.md
  Commit: "feat: add submit-pr skill"

提交路径: 本地 → <user>/<repo>:feat/xxx → PR → open-vela/<repo>:dev-ai-contest-2026

⚠️ 请确认以上信息是否正确？(y/n)
```

**必须等待用户回复 y 后才能继续执行。**

### Step 6: 本地提交并 Push 到 Fork

对每个仓库执行：

```bash
# 1. 添加 fork 为 remote（如果还没有）
git -C <repo_path> remote get-url fork 2>/dev/null || \
  git -C <repo_path> remote add fork git@github.com:<user>/<repo>.git

# 2. 获取上游目标分支最新代码（⚠️ 必须基于目标分支创建，不是当前分支）
git -C <repo_path> fetch <upstream_remote> <target_branch>

# 3. 基于目标分支创建新分支（确保 PR diff 只包含本次 commit）
git -C <repo_path> checkout -b <new_branch> <upstream_remote>/<target_branch>

# 4. 添加指定文件并提交
git -C <repo_path> add <file1> <file2> ...
git -C <repo_path> commit -m "<commit_message>"

# 5. Push 到 fork 仓库（不是上游！）
git -C <repo_path> push -u fork <new_branch>
```

**关键**: 
- Push 目标是用户的 fork（`fork` remote），不是上游（`openvela` remote）
- 新分支必须基于**目标分支**（`<upstream>/<target_branch>`）创建，这样 PR 的 diff 只包含本次 commit，不会带入其他分支的差异

分支命名规则: `<type>/<description>-<YYYYMMDD>`，如 `feat/submit-pr-skill-20260518`

### Step 7: 从 Fork 向上游创建 Pull Request

**PR 描述模板**（必须按此格式生成）：

```markdown
## Summary

一句话简单描述此次提交的修改内容

## Features

- 总结概括修改了哪些内容
- 每个要点一行

## Files

| 文件 | 说明 |
|------|------|
| path/to/file1.c | 文件功能和修改说明 |
| path/to/file2.h | 文件功能和修改说明 |

## Testing

如何测试和测试影响说明
```

**GitHub 平台** — 使用 MCP 工具：

```
mcp_github_create_pull_request(
  owner="open-vela",           # 上游 owner
  repo="<repo_name>",          # 上游 repo
  title="<pr_title>",
  head="<user>:<new_branch>",  # 格式: fork_owner:branch_name
  base="<target_branch>",      # 上游目标分支
  body="<按上述模板生成的 PR 描述>"
)
```

⚠️ **注意 `head` 参数格式**: 从 fork 提 PR 时，head 必须是 `<fork_owner>:<branch>` 格式。

⚠️ **注意 `body` 参数格式**: body 中的换行必须是真实换行符（多行字符串），不能用 `\n` 转义字符串，否则 GitHub 会把 `\n` 当成字面文本显示。

**Gitee 平台** — 使用 API：

```bash
curl -X POST "https://gitee.com/api/v5/repos/open-vela/<repo>/pulls" \
  -H "Content-Type: application/json" \
  -d '{
    "access_token": "<token>",
    "title": "<pr_title>",
    "head": "<user>:<new_branch>",
    "base": "<target_branch>",
    "body": "<按上述模板生成的 PR 描述>"
  }'
```

### Step 8: 返回结果

```
✅ PR 创建成功！

仓库 1: open-vela/nuttx
  PR: https://github.com/open-vela/nuttx/pull/123
  路径: <user>/nuttx:feat/xxx → open-vela/nuttx:dev-ai-contest-2026

仓库 2: open-vela/vendor_allwinnertech
  PR: https://github.com/open-vela/vendor_allwinnertech/pull/45
  路径: <user>/vendor_allwinnertech:feat/xxx → open-vela/vendor_allwinnertech:dev-ai-contest-2026
```

## 多仓库批量提交

当用户修改涉及多个仓库时：

1. 识别所有修改的仓库（通过 `bash scripts/detect-repos.sh` 或用户指定）
2. 每个仓库独立执行 Fork → Push → PR 流程
3. PR 描述中互相引用关联的 PR（如 "Related: open-vela/nuttx#123"）
4. 所有 PR 使用相同的分支名，方便识别

## 分支模糊匹配详细规则

| 本地分支名 | 远程分支列表 | 匹配结果 | 动作 |
|-----------|-------------|---------|------|
| `dev-ai-contest-2026` | dev, trunk, dev-ai-contest-2026 | 精确匹配 | 推荐此分支，用户确认 |
| `fix/adc-sensor` | dev, trunk | 无匹配 | 列出所有远程分支让用户选 |
| `dev` | dev, dev-ai-contest-2026 | 精确匹配 `dev` | 推荐 `dev`，用户确认 |
| `feature/quickapp` | dev, trunk, quickapp-dev | 包含匹配 `quickapp-dev` | 推荐 `quickapp-dev`，用户确认 |

**原则**: 任何自动匹配结果都必须经过用户确认，不能自动决定。

## Error Handling

| 错误 | 原因 | 解决 |
|------|------|------|
| `Permission denied` | Token 权限不足 | 重新生成 Token，勾选 repo 权限 |
| `fork already exists` | 已有 fork | 直接使用现有 fork |
| `branch already exists` | 分支名冲突 | 加时间戳后缀或递增编号 |
| `rejected (non-fast-forward)` | fork 分支落后上游 | `git fetch upstream && git rebase` |
| `MCP tool not found` | MCP 未配置 | 回到 Step 0 引导配置 |
| `base branch not found` | 目标分支不存在 | 确认分支名，列出可用分支 |
| `head not found` | fork 中没有该分支 | 确认 push 成功，检查 fork remote |

## Notes

- **所有操作前必须用户确认**：目标分支确认（Step 2）+ 提交计划确认（Step 5）
- Push 目标永远是 fork，不是上游仓库
- commit message 遵循 Conventional Commits 格式（feat/fix/docs/chore）
- PR 描述建议包含：修改原因、影响范围、测试方法
- 多仓库提交时，建议在 PR 描述中说明关联关系
- Fork 仓库如果长期未同步，提 PR 前建议先同步上游最新代码
