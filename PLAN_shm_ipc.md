# agora_shm_ipc 共享内存 IPC 设计（Linux / macOS）

## 目标

- 同机双进程通过 **POSIX 共享内存**（`shm_open` + `mmap`）交换任意二进制 **payload**。
- **单写单读**、**单槽**（无 ping-pong 多 buffer slot）。
- 创建/打开时可指定 **payload 字节数**；对象总长度 = **固定头 + payload**。
- **读不阻塞写**：写端从不等待读端；读慢时可能读到「不稳定」并重试（seqlock）。
- **写入中 / 写完**：由 **seqlock 序列号奇偶** 表达；可选扩展独立 `state` 位（当前实现以 seq 为准）。
- **崩溃恢复**：写进程每次启动在首次写前调用 `agora_shm_ipc_writer_session_begin`；读进程重启仅重新 `open` + 通知 socket，写进程可不重启。
- **写完通知**：**`AF_UNIX` + `SOCK_DGRAM`**；**写端 bind** 到 `writer_bind_path`，**读端 bind** 到 `reader_recv_path`，写端提交共享内存后对 `reader_recv_path` **`sendto`** 短报文唤醒读端。

## 与 shm_yuv 双槽方案的关系

| 项目 | shm_yuv | agora_shm_ipc |
|------|---------|---------------|
| 数据 | 固定 YUV 布局 + 双槽 | 通用 payload + **单槽 seqlock** |
| 唤醒 | 无（示例轮询） | **Unix 域 UDP** |

## 内存布局

映射区连续：`[AgoraShmIpcHeader][payload × payload_size]`。

头字段（见 `src/agora_shm_ipc.h`）：

- `magic` / `version`：格式校验（当前 `version == 2` 含下列元数据布局）。
- `payload_size`：创建时写入；附着方与入参比对。
- `data_len`：当前逻辑长度（≤ `payload_size`），仅在 seqlock 写临界区内更新。
- `user_id`：固定 64 字节（`AGORA_SHM_IPC_USER_ID_BYTES`），与 payload 同一 seqlock 提交。
- `media_type` / `stream_type`：`uint32_t`，语义为枚举 `AgoraShmMediaType`（0 视频 / 1 音频）、`AgoraShmStreamType`（0 main / 1 slides）。
- `width` / `height`：`int32_t`，视频帧尺寸（音频帧可置 0）。
- `sample_rate` / `channels` / `bits`：`int32_t`，音频属性（视频帧可置 0）。
- `seq`（`atomic_uint`）：**偶数** = 稳定态可读；**奇数** = 写入中。初始 `0`。

写接口 `agora_shm_ipc_write(..., meta, notify)` 在 seqlock 内写入上述元数据与 payload；`meta == NULL` 时将该帧元数据清零。读接口 `agora_shm_ipc_read(..., out_meta)` 可在 `out_meta != NULL` 时取与稳定 payload 同一快照的元数据。

## seqlock 协议

**写端**（`agora_shm_ipc_write`）：

1. `seq` 原子 `+1`（变奇，`memory_order_acq_rel`）。
2. `memcpy` 到 payload 区；写入 `data_len`。
3. `seq` 再 `+1`（变偶，`memory_order_release`）。

**读端**（`agora_shm_ipc_read`）：

1. `s1 = load(seq)`（`acquire`）；若为奇或 `data_len==0`，`errno=EAGAIN`。
2. 校验 `data_len`、用户 `cap`，`memcpy`。
3. `s2 = load(seq)`；若 `s1!=s2` 或 `s2` 为奇，`errno=EAGAIN`（并发写或写中）。

**`agora_shm_ipc_writer_session_begin`**：将 `seq` 置 `0`、`data_len` 置 `0`（`release`），清除上进程残留「卡在奇数」或脏长度。

## Unix 域 UDP 通知拓扑

1. **读进程**：`agora_shm_ipc_notify_reader_init` → `socket` + **`bind(reader_recv_path)`**（必要时先 `unlink` 路径文件）。
2. **写进程**：`agora_shm_ipc_notify_writer_init` → **`bind(writer_bind_path)`** + 记录对端 `reader_recv_path`。
3. 每次 `agora_shm_ipc_write(..., meta, notify)` 成功提交共享内存后，向 **`reader_recv_path`** `sendto` 1 字节。

两端使用 **文件系统路径**（macOS 无 Linux abstract namespace）。路径长度受 `sockaddr_un.sun_path` 限制。

## 公开 API 摘要

- `agora_shm_ipc_open` / `agora_shm_ipc_close` / `agora_shm_ipc_unlink`
- `agora_shm_ipc_writer_session_begin`
- `agora_shm_ipc_write` / `agora_shm_ipc_read`
- `agora_shm_ipc_notify_writer_init` / `agora_shm_ipc_notify_reader_init` / `agora_shm_ipc_notify_fd` / `agora_shm_ipc_notify_fini`

错误：以 **`-1` + `errno`** 为主；无稳定帧可读时 **`errno=EAGAIN`**。

## 平台与构建

- **Linux**：链接 `-lrt`。
- **macOS**：`shm_open` 名称尽量短（历史上总长度约 14 字节含 `/` 的限制需注意）。

## 验收建议

- 双终端：`agora_writer_demo` / `agora_reader_demo`；先读后写、先写后读各一轮。
- 运行中 `kill` 一侧后仅重启该侧，对侧不重启，通信应恢复。
- `len > payload_size` 时写失败且不破坏协议。

## 源码位置

- 头文件 / 实现：[src/agora_shm_ipc.h](src/agora_shm_ipc.h)、[src/agora_shm_ipc.c](src/agora_shm_ipc.c)
- 示例：[examples/agora_writer_demo.c](examples/agora_writer_demo.c)、[examples/agora_reader_demo.c](examples/agora_reader_demo.c)
