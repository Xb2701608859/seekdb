# MiniVectorDB — 运行协议推断与可配置项

> 状态：**协议未冻结**。官方题包与 `run_case.py` 驱动未随 `2026-competition-round2`
> 分支发布（见 `mysqltest/README.md` 末段），因此下面每一条都是**从公开材料反推**的
> 假设，而不是官方规定。本文件的作用是：把每个假设集中列出，标明证据强度，并指出
> 代码里对应的开关，便于在拿到真实题包后一次性对齐。

## 1. 官方明确写了什么（证据最强）

来源：赛题一正文 3.1 / 3.2 节 + `mysqltest/README.md`。

| # | 事实 | 出处 |
|---|---|---|
| F1 | 通过 **stdin 收 SQL、stdout 返结果**，不用 TCP、不用 JSON 请求/响应封装 | 3.1 |
| F2 | 日志/调试信息**只能进 stderr**，不得混入 stdout | 3.1 |
| F3 | 程序**常驻**，同一进程内连续执行多条语句与事务 | 3.1 |
| F4 | 必须**及时 flush stdout**，否则驱动等待会超时 | 3.1 |
| F5 | 数据目录固定 `/data`，重启复用；容器 `ENTRYPOINT` 前台运行 | 3.1 / 4.2.1 |
| F6 | 语句边界、结果集格式、错误码、就绪标识、exact/ann 切换、正常退出方式 **以题包为准** | 3.1 |
| F7 | 断言字段语义：`{}`=应成功、`affected_rows`=精确整数、`columns`=含别名且有序、`rows`=默认有序、`unordered:true`=多重集合、浮点 abs+rel 1e-5 | README |
| F8 | LIMIT 0 返回空结果；LIMIT 超行数返回已有记录；普通排序并列按主键升序 | README |

## 1.5 题包 v4（`minivectordb-public-v4`）新增的明确事实

官方分支在 2026-11-09T10:40:21Z 前进到 `9f6df2ba5acf871376e22b64a3d0724e4b575044`
（"upload more cases"），把公开样例从 5 个扩到 **20 个 / 95 条 SQL**，并明确了下面几点：

| # | 事实 | 对协议层的影响 |
|---|---|---|
| F9 | 驱动指令 `--restart`：正常关闭并等待退出，再用**同一数据目录**重启 | 关闭路径必须回滚未提交事务并落盘；EOF 退出要真的 exit(0) |
| F10 | 驱动指令 `--mode ann`：后续向量请求走 ann，每个用例最初 exact | 切换是**每条用例中途**发生的，不能只靠启动参数 |
| F11 | 断言新增 `error` 字段，值是**错误语义名**（`duplicate_key` / `unsupported_sql` / `dimension_mismatch`）；"当前未冻结错误码映射的配置至少要求非空错误码" | 错误行必须非空且稳定；具体文本仍可协商，但语义名必须能被驱动映射到 |
| F12 | 本包**没有强杀指令**；强杀/恢复仍是正式要求，只是不在公开包里 | 恢复能力必须实现，但无法用公开包验证 |
| F13 | 每个用例从**独立空库/数据目录**开始 | 用例之间不能共享状态；也意味着建库开销会计入每个用例 |
| F14 | 正式用例共 **29 个**，组织方另保留 **9 个** | 公开 20 个全过也只是下限 |

`--restart` 与 `--mode ann` 都**不是 SQL**、不占 `.result` 的断言位置，所以它们不会出现在
语句计数里；但它们决定了进程生命周期与检索模式，属于驱动协议的一部分。

## 2. 必须推断的部分（证据弱，风险高）

| # | 未知项 | 我们的默认选择 | 代码开关 | 风险 |
|---|---|---|---|---|
| U1 | 语句如何分帧 | 累积到引号/注释外的 `;`，可跨行；EOF 时冲刷残余 | `--statement-mode=semicolon\|line` | 中 |
| U2 | 一条响应在哪里结束 | 每条响应后输出独立终止行 `END` | `--ready-marker`、`Config::terminator` | **高** |
| U3 | 结果集是否带表头 | 带表头（列名一行，`\t` 分隔），因为 `columns` 断言必须被驱动看到 | `--output-format=tsv\|jsonl`、`Config::header` | **高** |
| U4 | 错误行长什么样 | `ERROR <CODE>: <message>`，CODE 为稳定符号名 | `--numeric-errors` | 中 |
| U5 | 就绪标识 | 无单独标识，用 U2 的终止行兼任 | `--ready-marker=TOKEN` | 中 |
| U6 | exact/ann 如何切换 | 启动参数 `--vector-mode=` + 会话内 `SET vector_mode = ann\|exact;` | 同上 | 中 |
| U7 | 正常退出方式 | stdin EOF 或 SIGTERM，两者都走"回滚未提交事务 + 落盘"路径 | — | 低 |
| U8 | 向量字面量写法 | `'[0, 1]'`（引号）与 `[0,1]`（裸）都接受 | — | 低 |

### U2/U3 为什么风险最高

公开用例的 `.result` 是**逐条对应**的 JSON 数组，驱动必须能把 stdout 切成一条条响应。
但 `SELECT id FROM docs WHERE id = 99` 这种查询合法地返回 **0 行**，此时：

- 若用"空行分隔"，零行结果与"没有输出"无法区分；
- 若用"行数前缀"，`columns` 又不好表达。

所以真实驱动**一定**有某种显式定界（终止符行、长度前缀、或 JSON 封装——虽然 F1 排除了
JSON 请求，但没排除 JSON 响应）。我们把定界做成一个字段而不是散落在输出代码里，就是为了
拿到题包后改一行即可。`--output-format=jsonl` 已经能直接产出与 `.result` 同构的对象，
可作为对照实验。

## 3. 已实现的协议层行为

```
$ printf 'CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(8));\nINSERT INTO t VALUES (2,\x27b\x27),(1,\x27a\x27);\nSELECT id FROM t ORDER BY id;\n' | minivectordb --data-dir /tmp/x
OK
END
OK 2
END
id
1
2
END
```

- 每条语句 → 一段响应 → `END` 行 → 立即 `fflush(stdout)`。
- DDL / 事务语句 → `OK`；DML → `OK <受影响行数>`；查询 → 表头 + 数据行。
- 错误 → `ERROR <CODE>: <message>`，进程**不退出**（题面要求不支持的语句返回约定错误而非崩溃）。
- stderr 只放日志，`--verbose` 时回显语句与响应，便于对照。

## 4. 本地验证

`tools/run_case.py` 复刻了 F7–F14 的断言语义与两条驱动指令，是**我们自己的**驱动
（不是官方那个）。当前 stub 引擎在 20 个公开用例上是 **12 passed / 8 failed**，
失败项全部是缺持久化与缺事务，不是协议层问题。

> 踩过的坑：`SET vector_mode = ann` 若不带结尾分号，在 `semicolon` 分帧下会被当成
> 未完成语句无限缓存，表现为驱动 `readline()` 永久阻塞。驱动发指令时**必须**带分号。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
python3 tools/run_case.py --binary build/minivectordb --all
```

## 5. 拿到题包后要对齐的清单

1. 用真实 `run_case.py` 跑一次 `sql_crud`，抓 stdout 原始字节，比对 U2/U3。
2. 确认错误码表（U4）：官方若给数字码，加映射表即可，不必改引擎。
3. 确认 ann 切换方式（U6）：若是启动参数，改 `ENTRYPOINT`；若是 SQL，已有 `SET`。
4. 确认正常退出方式（U7）：若题包用特定命令而非 EOF，在 `main.cpp` 的循环里加一条分支。
