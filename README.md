# MiniVectorDB

2026 OceanBase 数据库大赛 · 赛题一（加赛）：实现一个支持 SQL、持久化存储与事务的轻量向量数据库。

本仓库基于官方 `2026-competition-round2` 分支，在该分支上实现 MiniVectorDB 本体。
`mysqltest/` 是官方随分支发布的公开基础用例，未做改动。

## 当前状态

**协议骨架阶段。** 已完成并可在本地验证：

| 部分 | 状态 |
|---|---|
| stdin/stdout 协议层（分帧、结果集、错误、就绪、mode 切换） | ✅ 可运行 |
| 本地用例驱动 `tools/run_case.py`（复刻公开用例断言语义，含 `--restart` / `--mode ann` / `error` 断言） | ✅ 可运行 |
| 官方公开题包 `minivectordb-public-v4`（20 用例 / 95 条 SQL） | ✅ 已同步，未改动内容 |
| 公开用例通过率 | **12 / 20**（8 个失败全部源于无持久化与无事务） |
| 多阶段 `Dockerfile`（ubuntu:24.04，从源码构建） | ✅ 已写好（本机无 docker，未实机构建） |
| 存储 / 事务 / WAL / 崩溃恢复 / 向量索引 | ⛔ 未实现（当前为内存 stub） |

失败集中在 `storage_*`（重启后表丢失）、`transaction_*`（`ROLLBACK` 未实现、关闭不回滚）、
`vector_index_maintenance`（重启后表丢失）。它们不是协议层问题，而是引擎本体尚未实现。

`src/engine.cpp` 现在是**刻意保留的 stub**：它只在内存里维持表与行，用于验证协议链路，
不落盘、无事务、无恢复、无索引。`src/engine.h` 是协议层与引擎之间的接口，真正的存储与
执行层将替换 `engine.cpp` 的实现而不改动协议层。

## 目录

```
CMakeLists.txt            构建定义（C++20，静态链接 libstdc++/libgcc）
Dockerfile                多阶段构建，ENTRYPOINT 前台运行 --data-dir /data
src/main.cpp              入口：stdin 循环、信号、flush
src/protocol.h/.cpp       语句分帧 + 响应格式化（所有可配置的协议假设都在这里）
src/engine.h              引擎接口（Value / Result / Engine）
src/engine.cpp            STUB 实现，待替换
tools/run_case.py         本地用例驱动
docs/PROTOCOL.md          协议推断与风险清单
mysqltest/                官方公开用例（未改动）
```

## 构建与自测

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
python3 tools/run_case.py --binary build/minivectordb --all
```

预期输出（当前 stub 引擎）：

```
PASS sql_crud (7 statements)
...
RESULT: 12 passed, 8 failed (of 20)
failed: storage_restart, transaction_rollback, storage_mutations, ...
```

协议层回归测试独立于引擎：

```bash
bash tests/protocol_test.sh     # 24 passed, 0 failed
```

## 运行方式

```bash
./build/minivectordb --data-dir /data
```

从 stdin 逐条读取 SQL，向 stdout 输出结果，日志只走 stderr。每条响应后立即 flush。

```
$ printf 'CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(8));\nINSERT INTO t VALUES (2,\x27b\x27),(1,\x27a\x27);\nSELECT id FROM t ORDER BY id;\n' | ./build/minivectordb --data-dir /tmp/x
OK
END
OK 2
END
id
1
2
END
```

## 协议说明（重要）

官方题包与 `run_case.py` 驱动**未随分支发布**，`mysqltest/README.md` 明确说明它只定义
SQL 输入与结果语义、不定义进程通信封装。因此语句边界、响应定界、错误码文本、就绪标识、
exact/ann 切换方式目前都是**推断值**，集中定义在 `src/protocol.h` 的 `Config` 中，并可用
命令行参数覆盖：

| 参数 | 默认 | 作用 |
|---|---|---|
| `--statement-mode` | `semicolon` | 语句分帧：`semicolon` / `line` |
| `--output-format` | `tsv` | 响应格式：`tsv` / `jsonl` |
| `--ready-marker` | 空 | 每条响应后追加的标记行 |
| `--vector-mode` | `exact` | 默认检索模式 |
| `--numeric-errors` | 关 | 错误码渲染为数字 |
| `--verbose` | 关 | 把语句与响应回显到 stderr |

详见 [`docs/PROTOCOL.md`](docs/PROTOCOL.md)，其中逐条列出证据强度与拿到题包后需要对齐的清单。

## 未完成事项

- 持久化存储（catalog + 分页堆文件 + 主键索引）
- WAL 与崩溃恢复（含恢复途中再次终止、未提交回滚）
- 事务（BEGIN/COMMIT/ROLLBACK，语句失败不留部分修改）
- 近似向量索引及其增删改维护
- 性能与内存峰值优化
- `conversation.json`（Agent prompt 材料）
