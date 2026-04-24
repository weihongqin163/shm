<!--
  author: wei
  date: 2026-04-15
-->

# Changelog

本仓库变更记录；版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)（SemVer）。

## [Unreleased]

（尚无条目。）

## [1.0.1] - 2026-04-24

### 破坏性 / API

- **`agora_shm_ipc_read`**：第二参数改为 **`void **buf`**（调用方传入指针变量的地址）。**`*buf != NULL`** 时向该地址 **拷贝** payload（`cap >= data_len`）；**`*buf == NULL`** 时不拷贝，成功返回后 **` *buf = ctx->payload`**（映射区内视图）。调用方需改为例如 `void *p = stack_buf; agora_shm_ipc_read(ctx, &p, ...)` 或零拷贝 `void *p = NULL; ...`。
- **`agora_shm_manager`**：不再内部分配 **read scratch**；worker 对 SHM 读使用 **`*buf == NULL`** 路径，**`on_frame` 的 `payload` 指向 SHM mmap**，与 **`hdr` 一样仅在回调返回前有效**。**`max_read_cap`** 语义为 **允许挂接/派发的最大帧 payload 长度**（`0` 仍为内置默认），不再表示堆上 scratch 大小。

### 变更

- **`agora_shm_manager_worker_server`**：原栈上 **`srv_poll_buf[AGORA_SHM_MANAGER_UDP_CAP]`** 改为线程入口 **`malloc`**、退出前 **`free`**，降低 worker 线程栈占用。

## [1.0.0] - 2026-04-15

首个对外归纳版本，涵盖当前 **POSIX 共享内存 IPC**、**本机 UDP 信令** 与 **组合层 manager** 的整体形态。

### 新增 / 能力概览

- **`agora_shm_ipc`**：单槽 SHM + **seqlock**（`agora_shm_ipc_open` / `write` / `read` / `unlink` 等）；读侧通过 **`agora_shm_ipc_read(..., AgoraShmIpcHeader *out_hdr)`** 可取与 payload 同一稳定快照的**整头**（含 `magic`、`version`、`payload_size`、`data_len`、`seq` 等）。
- **`agora_localsock`**：127.0.0.1 **UDP**；客户端无 `connect`，以 **`sendto`** 固定发往本机端口；服务端收包、keep-alive 与 peer 管理。
- **`agora_shm_manager`**：组合 localsocket 与 SHM；**`agora_shm_manager_start(..., size_t max_read_cap, ...)`** 指定 worker 读 **scratch** 上限（`0` 为内置默认）；**APP**（整头）与 **WRITECMD**（`AgoraShmIpcFrameMeta`）派发、读写分表、自动附着读 SHM；**`on_frame`** 回调参数为 **`const AgoraShmIpcHeader *hdr`**。
- **示例**：`agora_writer_demo` / `agora_reader_demo`（读端轮询）、`agora_manager_demo`、`agora_localsock_demo`；**`Makefile`** 一键构建。

### 变更 / 移除

- **已移除** 独立 Unix 域模块 **`agora_shm_ipc_notify`**；manager 与示例信令走 **localsocket**。
- **manager demo**：本地写 SHM 仍按音视频区分 payload，**`max_read_cap`** 取 **max(视频, 音频)**，避免对端大帧因 `read_cap` 过小被丢弃。

### 文档

- **`README.md`**：编译、manager / IPC 要点与示例命令行。
- **`PLAN_shm_ipc.md`**、**`PLAN_shm_ipc_manager.md`**、**`PLAN_localsocket.md`**：设计与协议说明。

### 平台

- 目标：**Linux / macOS**，C11；Linux 链接需 **`-lrt`**（本仓库 `Makefile` 已按 `uname` 处理）。
