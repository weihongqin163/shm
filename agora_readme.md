<!--
  created by:wei
  copyright (c) 2026 Agora IO. All rights reserved.
  date: 2026-04-13
-->

# agora_shm_ipc 用法说明

本文说明同机双进程下 **POSIX 共享内存 + Unix 域 UDP 通知** 库 `agora_shm_ipc` 的编译、典型调用顺序与示例程序参数。

## 依赖与平台

- **系统**：Linux、macOS（POSIX `shm_open` / `mmap`）。
- **语言**：C11（`stdatomic.h`）。
- **链接**：Linux 需 `-lrt`；macOS 一般不需要。本仓库 `Makefile` 已按 `uname` 区分。

## 编译

在项目根目录执行：

```bash
make clean && make all
```

与 `agora_shm_ipc` 相关的产物：

| 产物 | 说明 |
|------|------|
| `build/agora_shm_ipc.o` | 库对象文件，可链进你的程序 |
| `build/agora_writer_demo` | 写端示例 |
| `build/agora_reader_demo` | 读端示例 |
| `build/agora_manager_demo` | **manager** 示例：单进程监听 notify，按 `shm_name` 动态附着 SHM 并回调（见 [`PLAN_shm_ipc_manager.md`](PLAN_shm_ipc_manager.md)） |
| `build/agora_shm_manager.o` | manager 层对象文件，与 `agora_shm_ipc.o`、`-pthread` 一起链接 |

将 `src/agora_shm_ipc.c` 与你的源码一起编译，并保证 `-Isrc` 能包含 `agora_shm_ipc.h`。使用 manager 时需增加 `src/agora_shm_manager.c` 或 `build/agora_shm_manager.o`。

## 概念与约束

- **单写单读**：协议上只应有一个写进程、一个读进程；不做多读者。
- **单槽**：一块连续区 = 固定 **header** + 你指定的 **payload** 长度；对象总大小为 `sizeof(AgoraShmIpcHeader) + payload_size`。
- **一致性**：payload 通过 **seqlock**（`header.seq` 奇偶）保护；读端可能因并发写得到 `errno == EAGAIN`，应重试。
- **通知**：`AF_UNIX` + `SOCK_DGRAM`。写端 **`bind(writer_bind_path)`**，写完共享内存后对 **`reader_recv_path`** 执行 **`sendto`**；读端 **`bind(reader_recv_path)`**，用 `poll`/`recv` 等待唤醒。

## 推荐启动顺序

1. **读进程**：先 `agora_shm_ipc_notify_reader_init`（绑定读端 socket 路径），再循环或重试 `agora_shm_ipc_open(..., is_creator=0)` 直到共享内存对象存在。
2. **写进程**：`agora_shm_ipc_open`（创建或附着）、`agora_shm_ipc_writer_session_begin`、`agora_shm_ipc_notify_writer_init`，再循环 `agora_shm_ipc_write`。

若写端先于读端完成 `notify_writer_init`，则早期几次 `sendto` 可能失败（实现中为 best-effort，不影响已成功写入的共享内存）；需要可靠唤醒时，请保证读端已 `bind` 接收路径。

## 写端流程（API）

1. `agora_shm_ipc_open(shm_name, payload_size, /*is_creator=*/1, &ctx)`；若返回 `EEXIST`，可改为 `is_creator=0` 附着（与示例一致）。
2. `agora_shm_ipc_writer_session_begin(&ctx)`（每个写进程生命周期内、第一次写之前调用一次）。
3. `agora_shm_ipc_notify_writer_init(&notify, writer_sock, reader_sock)`。
4. 循环：`agora_shm_ipc_write(&ctx, data, len, &notify)`，其中 `len <= payload_size`。
5. 退出：`agora_shm_ipc_notify_fini(&notify)`、`agora_shm_ipc_close(&ctx)`。若需删除内核中的共享内存对象，在确认无进程再使用时调用 `agora_shm_ipc_unlink(shm_name)`。

## 读端流程（API）

1. `agora_shm_ipc_notify_reader_init(&notify, reader_sock)`。
2. 用 `agora_shm_ipc_notify_fd(&notify)` 得到 `fd`，在 `poll` 中等待 `POLLIN`；有数据时 `recv` 丢弃通知字节即可。
3. `agora_shm_ipc_open(shm_name, payload_size, 0, &ctx)`（与写端创建时使用的 `payload_size` 必须一致）。
4. 收到通知后调用 `agora_shm_ipc_read(&ctx, buf, cap, &out_len)`；若 `errno == EAGAIN`，无稳定帧或并发写中，可稍后重试。示例 `agora_reader_demo` 在 **`poll` 超时** 时也会尝试一次 `read`，用于在附着共享内存前错过 `sendto` 时仍能拉到数据。
5. 退出：`agora_shm_ipc_close(&ctx)`、`agora_shm_ipc_notify_fini(&notify)`。

## 错误与 `errno`（摘要）

| 场景 | 典型 `errno` |
|------|----------------|
| 无数据或 seqlock 不稳定 | `EAGAIN` |
| 用户缓冲区小于当前 `data_len` | `ENOBUFS` |
| `len` 大于 `payload_size` | `EMSGSIZE` |
| 附着时 magic/version 不匹配 | `EPROTO`（若平台无该值则见实现） |
| 附着时 `payload_size` 与对象不一致 | `EINVAL` |

具体以 `agora_shm_ipc.h` 注释为准。

## 示例程序命令行

### 读端 `agora_reader_demo`

```text
./build/agora_reader_demo [shm_name [reader_sock [payload_size]]]
```

默认：`shm_name=/agsh1`，`reader_sock=/tmp/agora_reader.sock`，`payload_size=4096`。

### 写端 `agora_writer_demo`

```text
./build/agora_writer_demo [shm_name [writer_sock [reader_sock [payload_size]]]]
```

默认：`shm_name=/agsh1`，`writer_sock=/tmp/agora_writer.sock`，`reader_sock=/tmp/agora_reader.sock`，`payload_size=4096`。

两端 **`shm_name` 与 `payload_size` 必须一致**；`reader_sock` 必须与写端第三个参数相同，写端才能 `sendto` 到读端已绑定的路径。

### 最小联调示例

终端 A（先起读端）：

```bash
./build/agora_reader_demo
```

终端 B：

```bash
./build/agora_writer_demo
```

## 清理与调试

- **POSIX 共享内存**：对象在进程崩溃后仍可能残留，调试可调用 `agora_shm_ipc_unlink("/agsh1")`（名称与运行时一致），或自行写小程序调用 `shm_unlink`。
- **Unix socket 路径**：`notify_fini` 会 `unlink` 本进程 `bind` 过的路径；若进程异常退出，可手动删除 `/tmp/agora_reader.sock`、`/tmp/agora_writer.sock` 后再启动。
- **macOS**：`shm_open` 名称尽量短（历史上含 `/` 的总长度约 14 字节的限制需注意），示例中的 `/agsh1` 较短。

## 进一步设计说明

协议细节、seqlock 步骤与验收建议见 [PLAN_shm_ipc.md](PLAN_shm_ipc.md)。
