# 工作区与仓库（manifest）

openvela 是 `repo` 多仓工作区：`nuttx`、`apps`、`vendor/espressif`、`packages/ai_agent` 等各是一个 git 仓，
靠 `.repo/manifests/` 下的 xml 组合。本项目的交付是一份**自己的 manifest**，把作品仓的文件映射回工作区位置。

## 本项目的交付映射

- 作品仓 `contest2026_342_ezdezhandui` 作为 self-ref 项目出现在 manifest 里；
- 仓内 4 棵文件树映射回工作区：

| 作品仓内 | 工作区位置 |
|---|---|
| `nuttx/**` | `nuttx/**` |
| `board/esp32p4/**` | `vendor/espressif/boards/esp32p4/**` |
| `app/apps/**` | `apps/**` |
| `app/ai_agent/**` | `packages/ai_agent/**` |

- 映射由 `<copyfile src="..." dest="..."/>` 表达（`src` 相对作品仓、`dest` 相对工作区根）。

## 两个必须知道的限制

1. **`<copyfile>` 不支持目录**：repo 工具对目录会直接报错（"copying from directory not supported"），
   所以"整文件夹替换"要靠逐个文件条目，或改用 `<linkfile>`（symlink，可指向目录）。
2. **manifest 不能表达"删除"**：交付只能新增/覆盖文件；所以共享层里被我们改过的 tracked 文件
   只能留在原地（不能"搬走"，否则交付会缺文件）。

## 只读保护区

另一条对话正在开发模拟器 UI，以下路径**不交付、不同步**：

```
apps/examples/deskmate
nuttx/drivers/net/wifi_sim.c
nuttx/boards/sim/sim/sim/configs/deskmate
vendor/openvela
```

对账与受控同步见 `tools/engineering/check_consistency.py` 与 `sync_and_restore.sh`。
