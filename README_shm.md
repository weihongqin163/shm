# 双进程 YUV 共享内存（纯 C）

同机两个进程通过 **POSIX 共享内存**（`shm_open` + `mmap`）交换一帧 **1920×1080 YUV420 平面布局** 数据（`1920 * 1080 * 3 / 2 = 3,110,400` 字节），约 **30 fps** 量级；读、写各自 **单线程**；写端 **不阻塞**（双槽 ping-pong，读慢会跳帧）；读端仅在 `state == 1` 时读，并对大帧做 **拷贝前后双检**。

## 需求摘要

1. **纯 C**（C11，`stdatomic.h`）。
2. 同一机器上 **两个进程** 传数据。
3. 载荷为 **YUV 帧**，单帧大小如上；示例写端按 **30 Hz** 休眠模拟。
4. 每槽字段：**userid**（`char[64]`，不足填 `'\0'`）、**state**（`0` 无效，`1` 写完可读，`2` 写入中）、**width / height / sequence**、**yuv_data**。
5. 读、写路径各 **单线程**。
6. 写端 **不因读端而阻塞**；双槽交替写并原子发布 `active_slot`。
7. 读端仅当 **state 为 1** 时读取；拷贝 YUV 后再次核对 **state 与 sequence（及宽高）** 未变，否则丢弃。

## 二进制布局

- 头文件：[shm_yuv_layout.h](shm_yuv_layout.h) 使用 `offsetof` / `_Static_assert` 固定字段顺序与大小。
- `ShmYuvRegion`：`active_slot`（`0` 或 `1`）+ `slots[2]`。
- 写端在 **非** `active_slot` 的槽上：`state=2` → 填元数据与 YUV → `state=1` → `release` 更新 `active_slot`。
- 读端：`acquire` 读 `active_slot` 与对应槽的 `state`，仅 `1` 时拷贝并双检。

## 状态与内存序

- 槽内 `state` 为 `_Atomic uint_least32_t`，发布索引为 `_Atomic unsigned int`。
- 写端对 `state`、`active_slot` 使用 `memory_order_release`；读端使用 `memory_order_acquire`，避免重排序导致读到未完成数据。

## 启动次序与崩溃恢复

- **谁先启动都可以**：双方均调用 `shm_yuv_open()`。内部先尝试 `O_CREAT | O_EXCL`；**创建者**执行 `ftruncate` 并对 **已映射的前缀** `memset(0)`；附着方 **不得** 对整块共享区 `memset`。
- **POSIX 对象在进程崩溃后仍存在**（直到 `shm_unlink` 或系统回收）。存活进程保持 `mmap` 即可；重启进程再次 `shm_open` 附着。
- **写进程每次启动**（含崩溃重启）在发帧前须调用 `shm_yuv_writer_session_begin()`：两槽 `state` 置 `0`，`active_slot` 置 `0`，清除「卡在 `state==2`」的残留。
- **读进程崩溃** 无需写共享区；重启后继续按 `state==1` 读即可。
- **大帧与写端重启**：读端在 `memcpy` 前后核对 `state` 与 `sequence`（及宽高），避免写端同时失效槽时出现脏读。

## 平台说明

- **macOS**：`shm_open` 名称需以 `/` 开头且 **总长度受限**（历史上常见约 14 字节含 `/`），示例默认使用 **`/shm_yuv1`**。命令行可传入更短自定义名。
- **对象长度**：创建时 `ftruncate(sizeof(ShmYuvRegion))`；部分系统会把 `fstat` 看到的 `st_size` **向上取整到页大小**。实现要求 `st_size >= sizeof(ShmYuvRegion)`，只 `mmap` 使用的 `sizeof(ShmYuvRegion)` 前缀。
- **Linux**：链接时可能需要 **`-lrt`**（本仓库 `Makefile` 已按 `uname` 处理）。

## 构建

```bash
make
```

生成 `writer_demo`、`reader_demo`。

## 读端、写端用法

### 1. 命令行演示程序

两端必须使用 **相同的** POSIX 共享内存名称（默认均为 `/shm_yuv1`）。可先起写端或读端，与实现细节见上文「启动次序与崩溃恢复」。

| 程序 | 命令格式 | 说明 |
|------|----------|------|
| **写端** `writer_demo` | `./writer_demo [shm_name [userid]]` | 无参数时用默认名与默认 userid；第二参数为共享内存名（须以 `/` 开头，且尽量短，尤其 macOS）；第三参数为 `userid` 字符串，最长 64 字节，更长的部分会被截断，不足 64 的字节在槽内填 `'\0'`。启动后先调用库里的写端恢复，再按约 **30 帧/秒** 循环写帧；标准输出行含 `published seq` 与当前 `active_slot`。 |
| **读端** `reader_demo` | `./reader_demo [shm_name]` | 可选第一参数为共享内存名。内部约 **每 1 ms** 轮询一次；仅当槽 `state==1` 时拷贝 YUV，并做拷贝前后一致性检查。同一 `sequence` 只打印一行，避免刷屏。行内为 `seq`、宽高、`userid`、`pattern_ok`（与写端填充规则是否一致）。 |

**典型操作（两终端）：**

```bash
# 终端 A — 写
./writer_demo

# 终端 B — 读（名称与 A 一致时可省略参数）
./reader_demo
```

**指定名称与 userid：**

```bash
./writer_demo /shm_yuv1 my_user_id
./reader_demo /shm_yuv1
```

### 2. 业务进程接入（API）

头文件：`shm_yuv.h`、`shm_yuv_layout.h`；实现：`shm_yuv.c`（与业务目标文件一同编译；Linux 下链接加 `-lrt`，见 `Makefile`）。

**写端推荐步骤：**

1. 准备与读端约定的共享内存名 `name`（建议短名、以 `/` 开头）。
2. `ShmYuvMap map;`，调用 `shm_yuv_open(name, &map)`；失败则根据 `errno` 处理（若对端尚未创建，需重试或保证先有一侧创建成功）。
3. 每个写进程生命周期内 **首次发帧前** 调用一次 `shm_yuv_writer_session_begin(map.region)`。
4. 每帧在 **非当前发布槽** 上写入（与 `writer_demo.c` 中逻辑一致）：
   - `atomic_store(state, 2)`（`SHM_YUV_STATE_WRITING`）；
   - 写入 `userid`（64 字节内有效字符 + `'\0'` 填充）、`width`、`height`、`sequence`、`yuv_data`；
   - `atomic_store(state, 1)`（`SHM_YUV_STATE_READY`）；
   - `atomic_store(active_slot, 本槽索引)`，均使用 **release** 语义（见 demo）。
5. 进程退出前 `shm_yuv_close(&map)`。若需删除内核对象供下次干净创建，在合适时机（通常所有进程都不再使用时）对同一 `name` 调用 `shm_yuv_unlink(name)`。

**读端推荐步骤：**

1. 使用与写端相同的 `name`，`shm_yuv_open(name, &map)`。
2. 在单线程循环中：
   - `acquire` 读取 `active_slot`，再 `acquire` 读该槽 `state`，**仅当为 `1`（`SHM_YUV_STATE_READY`）** 才继续；
   - 记录 `sequence`、宽高、`userid` 等，再 `memcpy` 拷贝 `yuv_data`（或按需只读元数据）；
   - 拷贝后再次读取 `state` 与 `sequence`（及宽高），**仍为 `1` 且与拷贝前一致** 才采纳本帧，否则丢弃本轮、下次再读（与 `reader_demo.c` 一致）。
3. 退出时 `shm_yuv_close(&map)`。

**注意：** 槽内 `state` 与 `active_slot` 在布局中为原子类型；元数据与 `yuv_data` 的可见性由 `state` 的 release/acquire 与上述双检共同保证，尤其在写端重启清空槽时避免脏读。

## 删除共享内存对象

对象会持久存在，调试时可自行调用 `shm_unlink(3)`（需写一小段 C 或使用其他语言调用同名 API）。名称须与运行时传入一致。

## 自检建议

- 双终端正常收发：读端打印 `sequence`、`userid`、简单图案校验 `pattern_ok=1`。
- **启动顺序**：先写后读、先读后写各测一轮。
- **崩溃恢复**：运行中 `kill` 写端或读端，仅重启被 kill 一侧，另一侧不重启，应能恢复；写端重启后会短暂无 `state==1`，属预期。

## 文件列表

| 文件 | 说明 |
|------|------|
| [shm_yuv_layout.h](shm_yuv_layout.h) | 布局与常量 |
| [shm_yuv.h](shm_yuv.h) / [shm_yuv.c](shm_yuv.c) | 打开、关闭、`shm_unlink` 封装，`shm_yuv_writer_session_begin` |
| [writer_demo.c](writer_demo.c) | 写端示例（约 30 fps） |
| [reader_demo.c](reader_demo.c) | 读端示例（轮询 + 双检） |
| [Makefile](Makefile) | 构建 |

## 局限与后续

- 当前为 **单写单读**；多读者需另行设计（如 seqlock、代次号）。
- **零丢帧** 非目标；需要背压或环形多槽时需扩展协议。
