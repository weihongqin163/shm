<!--
  created by:wei
  copyright (c) 2026 Agora IO. All rights reserved.
  date: 2026-04-15
-->

# agora_shm 用法说明

本文说明仓库内 **POSIX 共享内存 IPC**（`agora_shm_ipc`）、**本机 UDP 信令**（`agora_localsock`）与 **组合层 manager**（`agora_shm_manager`）的编译、典型用法与示例参数。**写读双进程示例**仅依赖 **`agora_shm_ipc`**（读端短间隔轮询 `agora_shm_ipc_read`）；**manager** 路径用 **localsocket** 做信令与唤醒。

## 依赖与平台

- **系统**：Linux、macOS（POSIX `shm_open` / `mmap`）。
- **语言**：C11（`stdatomic.h`）。
- **链接**：Linux 需 `-lrt`；macOS 一般不需要。本仓库 `Makefile` 已按 `uname` 区分。

## 编译

在项目根目录执行：

```bash
make clean && make all
```

主要产物：

| 产物 | 说明 |
|------|------|
| `build/agora_shm_ipc.o` | SHM + seqlock 核心对象 |
| `build/agora_localsock.o` | 127.0.0.1 UDP localsocket |
| `build/agora_shm_manager.o` | manager 组合层（链 `agora_shm_ipc.o` + `agora_localsock.o` + `-pthread`） |
| `build/agora_writer_demo` / `build/agora_reader_demo` | 仅 **SHM** 的写/读示例（读端轮询，无独立 notify 模块） |
| `build/agora_manager_demo` | **localsocket + manager**：端口、`server_mode`、localsock 参数、写 SHM 名（见下文） |
| `build/agora_localsock_demo` | localsocket 最小 server/client 示例 |

将 `src/*.c` 或对应 `build/*.o` 与你的源码一起编译，并保证 `-Isrc` 能包含头文件。

## agora_shm_manager（localsocket + SHM）

- **职责**：`agora_shm_manager_start` 在 **127.0.0.1 UDP** 上以 **server**（绑定端口）或 **client**（`connect` 对端端口）运行 localsocket，并起 **一条** worker 线程：`server_poll` / `client_poll` 收到 **APP**（整帧 `AgoraShmIpcHeader`）或 **WRITECMD**（`AgoraShmIpcFrameMeta`）后，在读表中查找或 **自动附着读 SHM**，再 **`agora_shm_ipc_read`**（**`*buf == NULL`** 的零拷贝路径，见 `agora_shm_ipc.h`），释锁后调用 **`on_frame`**（**`hdr`** 与 **`payload`** 均与读接口快照 / mmap 一致；详见下文生命周期）。
- **`max_read_cap`**：表示 worker 可接受的 **最大 SHM 帧 payload 长度**（用于附着与派发上限，`0` 为内置默认），**不再**表示内部分配的读 scratch 堆缓冲。
- **表**：读表、写表各最多 **64** 槽。**`agora_shm_manager_add(shm_name, max_payload_size)`**：写端创建并登记。**`agora_shm_manager_remove`**：先写表后读表。**`agora_shm_manager_write`**：写表上 `agora_shm_ipc_write`，再发 **WRITECMD** 信令。写表已占用的 `shm_name` 不会同时为该名自动开读。
- **`agora_shm_manager_close`**：停止 worker；`close` 所有读/写 IPC；写槽若为 **`agora_shm_ipc_open(..., is_creator=1)`** 创建（`ipc.creator` 非零），在 close 后 **`agora_shm_ipc_unlink`**；再销毁 localsocket。
- **设计细节**：[`PLAN_shm_ipc_manager.md`](PLAN_shm_ipc_manager.md)；localsocket 协议：[`PLAN_localsocket.md`](PLAN_localsocket.md)。

## agora_shm_ipc（核心）

- **单槽**：一块连续区 = 固定 **header** + 你指定的 **payload**；总大小为 `sizeof(AgoraShmIpcHeader) + payload_size`。
- **一致性**：payload 经 **seqlock**（`header.seq` 奇偶）保护；读端可能 **`errno == EAGAIN`**，应重试。
- **`agora_shm_ipc_read(ctx, buf, cap, out_len, out_hdr)`**：`buf` 为 `void **`（指向调用方 `void *` 槽位）。调用前令 `void *p` 指向用户缓冲并传 `&p` 做 **拷贝读**，或 `p = NULL` 再传 `&p` 做 **零拷贝**（成功时 `p` 变为 `ctx->payload`，映射内视图，须尽快使用或自行拷贝）。**`cap`** 为允许的 **`data_len` 上限**（不足则 **`ENOBUFS`**）。

### writer/reader demo（轮询读）

- 写端只调用 **`agora_shm_ipc_write`**；读端在短 **`usleep`** 间隔下重试 **`agora_shm_ipc_read`**（例如 **`void *read_dst = buf; agora_shm_ipc_read(&ctx, &read_dst, ...)`** 拷贝到已分配缓冲），可选 **`out_hdr`** 取头快照，并用 **`hdr.seq`** 去重，避免同一稳定帧重复打印。生产环境可用 **localsocket APP 整头**（与 manager 一致）替代忙等。

## 概念与约束（manager 场景）

- **APP UDP**：`msg_type == 2`，payload 为 **`sizeof(AgoraShmIpcHeader)`** 的裸头（用于携带头快照以便对端附着/读 SHM）。
- **WRITECMD**：`msg_type == 3`，payload 为 **`AgoraShmIpcFrameMeta`**；对端若无读表项，会 **probe** SHM 头得到 **`payload_size`** 再附着读。
- **生命周期**：`on_frame` 里的 **`const AgoraShmIpcHeader *hdr`** 与 **`const void *payload`** 仅在回调返回前有效；**`payload` 指向 SHM 映射区**，跨异步长期持有须先在回调内 **memcpy** 到自有缓冲。

## 写端流程（API，仅 SHM）

1. `agora_shm_ipc_open(shm_name, payload_size, /*is_creator=*/1, &ctx)`；若 `EEXIST` 可改为 `is_creator=0` 附着。
2. `agora_shm_ipc_writer_session_begin(&ctx)`。
3. 循环：`agora_shm_ipc_write`；`len <= payload_size`。
4. 退出：`agora_shm_ipc_close`；必要时 **`agora_shm_ipc_unlink(shm_name)`**。

## 读端流程（API，仅 SHM）

1. `agora_shm_ipc_open(shm_name, payload_size, 0, &ctx)`（`payload_size` 须与对象一致）。
2. `agora_shm_ipc_read`：传入 **`void *p = your_buf;`** 与 **`&p`** 做拷贝读，或 **`p = NULL` / `&p`** 做零拷贝视图；**`EAGAIN`** 时重试（可与 **`usleep` / `poll`** 或 **localsocket** 结合）。
3. 退出：`agora_shm_ipc_close`。

## 错误与 `errno`（`agora_shm_ipc` 摘要）

| 场景 | 典型 `errno` |
|------|----------------|
| 无稳定帧 / seqlock 不稳定 | `EAGAIN` |
| 用户缓冲区小于当前 `data_len` | `ENOBUFS` |
| `len` 大于 `payload_size` | `EMSGSIZE` |

具体以 [`src/agora_shm_ipc.h`](src/agora_shm_ipc.h) 为准。

## 示例程序命令行

### 读端 `agora_reader_demo`

```text
./build/agora_reader_demo [shm_name [payload_size]]
```

默认：`shm_name=/agsh1`，`payload_size=4096`。

### 写端 `agora_writer_demo`

```text
./build/agora_writer_demo [shm_name [payload_size]]
```

默认：`shm_name=/agsh1`，`payload_size=4096`。

两端 **`shm_name` 与 `payload_size` 必须一致**。

### manager 示例 `agora_manager_demo`（localsocket）

```text
./build/agora_manager_demo <port> <server_mode 0|1> <max_clients> <keepalive_ms> <write_shm_name>
```

| 参数 | 含义 |
|------|------|
| `port` | 127.0.0.1 UDP 端口（须 > 0） |
| `server_mode` | `1`：本进程 manager 为 UDP **server** 并绑定 `port`；`0`：为 **client** 连到 `port`（demo 无 writer 循环，仅等待信令） |
| `max_clients` / `keepalive_ms` | 仅 **server** 模式传给 `agora_localsock_server_create`（client 模式仍须填合法正数） |
| `write_shm_name` | **server_mode=1** 时：本进程创建/附着写 SHM，并用 localsock **client** 向本机 `port` 发 **APP**（整头）唤醒 manager |

**server_mode=1** 时：进程内同时有 **manager（UDP server）** 与 **localsock client** 发信令，无需第二进程即可联调。`payload_size` 在 demo 内固定为 **4096**；退出 **Ctrl+C**。异常退出后可 **`shm_unlink`** 各自 `write_shm_name`。

### localsocket 最小示例 `agora_localsock_demo`

```text
./build/agora_localsock_demo server <port> <keepalive_ms> <max_clients>
./build/agora_localsock_demo client <port> <keepalive_ms> [seconds]
```

### 最小联调（reader / writer）

终端 A：

```bash
./build/agora_reader_demo
```

终端 B：

```bash
./build/agora_writer_demo
```

## 清理与调试

- **POSIX 共享内存**：调试可 **`agora_shm_ipc_unlink("/agsh1")`**（名称与运行时一致）。
- **macOS**：`shm_open` 名称尽量短；示例 `/agsh1` 较短。

## 进一步设计说明

- SHM / seqlock：[PLAN_shm_ipc.md](PLAN_shm_ipc.md)
- Manager + localsocket：[PLAN_shm_ipc_manager.md](PLAN_shm_ipc_manager.md)
- UDP 协议：[PLAN_localsocket.md](PLAN_localsocket.md)
