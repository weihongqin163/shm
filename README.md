<!--
  created by:wei
  copyright (c) 2026 Agora IO. All rights reserved.
  date: 2026-04-15
-->
# agora_shm的最佳实践
一：头文件
只需要关注： agora_shm_manager.h 中的5个api

 二、SHM 集成有几点可能注意下哈：
1、udp 端口使用9877
2、同一进程或同一机器可启动多个 manager；每个 manager 使用独立的加锁状态目录
agora_shm_manager_start： 你用client 模式；agora用server模式
read_cap = *1920*1080*3)/2

agora_shm_manager_add：
如果是视频，max_pay_size = (1920*1080*3)/2
如果是音频，max_pay_size = (48000*1*2)/8
3、shm 只支持 127，所以 docker 要使用 host 网络

三、api 调用逻辑
1、进程启动，agora_shm_manager_start
注意用“二”中的参数

2、当你收到AVCDialInReply的时候，取from XX里面的media ID作为shm_name参数，调用
agora_shm_manager_add

// write-read
3、你有数据要通知agora的时候：agora_shm_manager_write
4、当agora有数据达到，会自动callback 给你：agora_shm_manager_on_frame_fn
注意：不要修改on_frame里面的内存值、不要释放、不要在callback中做耗时操作


5、当Hang协议的时候，调用agora_shm_manager_remove
注意：只移除你在三.2中加入的shm_name
6、进程退出的时候：agora_shm_manager_close

四、写入/读取数据大小的约定
1、音频数据：需要是10ms的整数倍，不局限多少倍

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
| `build/agora_shm_manager_state.o` | manager 写 SHM 清单、文件锁与异常退出恢复 |
| `build/agora_shm_manager.o` | manager 组合层（链 `agora_shm_ipc.o` + `agora_localsock.o` + `agora_shm_manager_state.o` + `-pthread`） |
| `build/agora_writer_demo` / `build/agora_reader_demo` | 仅 **SHM** 的写/读示例（读端轮询，无独立 notify 模块） |
| `build/agora_manager_demo` | **localsocket + manager**：端口、`server_mode`、localsock 参数、写 SHM 名（见下文） |
| `build/agora_localsock_demo` | localsocket 最小 server/client 示例 |

将 `src/*.c` 或对应 `build/*.o` 与你的源码一起编译，并保证 `-Isrc` 能包含头文件。

## agora_shm_manager（localsocket + SHM）

- **职责**：`agora_shm_manager_start` 在 **127.0.0.1 UDP** 上以 **server**（绑定端口）或 **client**（`connect` 对端端口）运行 localsocket，并起 **一条** worker 线程。收到 **APP**（整帧 `AgoraShmIpcHeader`）或 **WRITECMD**（`AgoraShmIpcFrameMeta`）后，在读表中查找或 **自动附着读 SHM**，再读取稳定帧；收到 **REMOVECMD** 后只关闭并移除同名读槽。
- **接收模式**：`on_frame != NULL` 保持原来的零拷贝回调模式；`on_frame == NULL` 启用 `agora_shm_manager_poll` 模式。两种模式互斥，回调模式调用 poll 返回 `ENOTSUP`。
- **`max_read_cap`**：worker 可接受的 **最大 SHM 帧 payload 长度**（`0` 为内置默认）。poll 模式还用它作为视频缓存和稳定读取 scratch 的容量；callback 模式不分配这些缓存。
- **表与所有权**：读表、写表各最多 **256** 槽。**`agora_shm_manager_add(shm_name, max_payload_size)`** 创建写 SHM 并把名称持久化。**`agora_shm_manager_remove`** 命中写槽时发送 **REMOVECMD**、关闭并 unlink SHM、删除清单记录；命中读槽时只清理本地读槽。写表已占用的 `shm_name` 不会同时为该名自动开读。
- **`agora_shm_manager_close`**：停止 worker；对写槽发送 best-effort **REMOVECMD**，关闭全部读/写 IPC 并 unlink 创建方写 SHM；销毁 localsocket，成功清理后删除本 manager 的磁盘状态目录。
- **设计细节**：[`PLAN_shm_ipc_manager.md`](PLAN_shm_ipc_manager.md)；localsocket 协议：[`PLAN_localsocket.md`](PLAN_localsocket.md)。

### manager 音频 burst 写入

`agora_shm_manager_write` 对音频先拷贝 payload 和 metadata 到每个 `shm_name`
独立的 pre-buffer。返回 `0` 表示已入队，调用方可立即复用原缓冲区；实际写入和
通知由已有 worker 异步完成。音频须为有效 PCM：正数采样率、声道数、可被 8
整除的正数位深，且非空 payload 包含完整的交错采样点。

- 首帧进入空队列后等待 **10 ms** 合并窗口；worker 使用单调时间调度，每个
  截止时间最多合并 **4 个**连续且 metadata 一致的输入帧，一次写 SHM、通知一次。
  未合并的帧保留至下一周期；worker 延迟后不连续补写。
- 合并总长度不超过 `add(max_payload_size)` 已分配的 SHM payload。希望合并
  4 帧时，应预留至少 4 帧容量，并确保接收端 `max_read_cap` 足够；容量较小则
  每次合并更少帧，无需修改 SHM 协议。采样率/声道数/位深变化时清除旧格式队列。
- 为限制实时语音积压，发送队列最多保存 **200 ms PCM / 64 个输入帧**，超限
  丢弃最旧完整帧。单次输入超过 200 ms 但能放入 SHM 时，仍可单独保留。
  `remove/close` 丢弃并释放尚未发送的音频。
- 视频继续立即覆盖写入。音频异步通知失败会记录日志，不会通过原 `write`
  返回，也不重放已提交批次。定时合并缓解 burst；单槽 SHM 和 UDP 通知仍不保证
  每批必达。

### manager poll 模式

启动时将 `on_frame` 传为 `NULL`，再用调用方分配的 `header` 和 `out` 接收数据：

```c
AgoraShmManager *manager = NULL;
if (agora_shm_manager_start(NULL, port, true, max_clients, keepalive_ms, NULL,
                            max_read_cap, &manager) != 0) {
  /* handle errno */
}

AgoraShmIpcHeader header;
char *out = malloc(max_read_cap);
ssize_t n = agora_shm_manager_poll(manager, shm_name, &header, out,
                                   max_read_cap);
if (n > 0) {
  /* out[0..n) is valid and header.data_len == (uint32_t)n */
} else if (n < 0) {
  /* handle errno */
}
```

- 音频按到达顺序追加，最多保留最新 **200 ms**（容量按帧元数据中的采样率、声道数和位深计算）；poll 可以分段取走数据，并将剩余数据前移。格式变化时清空旧缓存并重算容量。
- 视频只保留最新完整帧；`out` 容量不足时返回 `-1/ENOBUFS`，该帧保留供扩大缓冲后重试。
- 找到读条目但暂无缓存数据时返回 `0`，不修改 `header` 和 `out`；找不到 `shm_name` 时返回 `-1/ENOENT`。
- 音频聚合时输出 header 来自最新接收帧；所有成功 poll 都将 `header.data_len` 改为本次返回字节数。

## agora_shm_ipc（核心）

- **单槽**：一块连续区 = 固定 **header** + 你指定的 **payload**；总大小为 `sizeof(AgoraShmIpcHeader) + payload_size`。
- **一致性**：payload 经 **seqlock**（`header.seq` 奇偶）保护；读端可能 **`errno == EAGAIN`**，应重试。
- **`agora_shm_ipc_read(ctx, buf, cap, out_len, out_hdr)`**：`buf` 为 `void **`（指向调用方 `void *` 槽位）。调用前令 `void *p` 指向用户缓冲并传 `&p` 做 **拷贝读**，或 `p = NULL` 再传 `&p` 做 **零拷贝**（成功时 `p` 变为 `ctx->payload`，映射内视图，须尽快使用或自行拷贝）。**`cap`** 为允许的 **`data_len` 上限**（不足则 **`ENOBUFS`**）。

### writer/reader demo（轮询读）

- 写端只调用 **`agora_shm_ipc_write`**；读端在短 **`usleep`** 间隔下重试 **`agora_shm_ipc_read`**（例如 **`void *read_dst = buf; agora_shm_ipc_read(&ctx, &read_dst, ...)`** 拷贝到已分配缓冲），可选 **`out_hdr`** 取头快照，并用 **`hdr.seq`** 去重，避免同一稳定帧重复打印。生产环境可用 **localsocket APP 整头**（与 manager 一致）替代忙等。

## 概念与约束（manager 场景）

- **APP UDP**：`msg_type == 2`，payload 为 **`sizeof(AgoraShmIpcHeader)`** 的裸头（用于携带头快照以便对端附着/读 SHM）。
- **WRITECMD**：`msg_type == 3`，payload 为 **`AgoraShmIpcFrameMeta`**；对端若无读表项，会 **probe** SHM 头得到 **`payload_size`** 再附着读。
- **REMOVECMD**：`msg_type == 4`，payload 为固定 **64 字节 SHM 名称**；只有写槽所有者发送，对端只移除同名读槽且不执行 unlink。
- **生命周期**：`on_frame` 里的 **`const AgoraShmIpcHeader *hdr`** 与 **`const void *payload`** 仅在回调返回前有效；**`payload` 指向 SHM 映射区**，跨异步长期持有须先在回调内 **memcpy** 到自有缓冲。

### manager 异常退出恢复与 Pod 部署

- 状态根目录默认为 **`/var/lib/agora/shm-manager`**。启动时逐级创建缺失的父目录和根目录，新建目录权限为 `0700`；已有目录不修改权限或属主。每个成功初始化状态的 manager 使用独立子目录、`flock` 独占锁和 `shm.list`；`add` 尝试登记名称，`remove` 成功 unlink 后尝试删除名称。
- **状态持久化尽力完成，不阻断媒体业务**：`state_open/open_at` 初始化或回收失败只记录日志，manager 继续启动并跳过后续状态更新；`state_add` 登记失败只记录日志，保留已创建的 SHM 并完成内存写表登记，`add` 仍成功。清单更新失败也不改变 `remove` 的业务结果；实际 SHM 创建、映射、初始化、通知和 unlink 错误仍按各接口规则处理。
- `agora_shm_manager_start` 尝试扫描状态目录：仍被锁定的目录属于活跃 manager，必须跳过；可获得锁的目录按清单幂等执行 `shm_unlink` 后删除。
- **降级限制**：状态目录不可用或运行期间被删除时，正常 `remove/close` 仍根据内存写表清理自己创建的 SHM；异常退出后，缺少有效清单的 SHM 可能残留并占用容量，后续创建同名对象可能返回 `EEXIST`。当前不会自动重建状态目录或在同名冲突时删除旧对象。
- 宿主机进程与同一节点上的 Pod 进程协作时，通过 `hostPath` 把宿主机目录挂载到容器中的相同路径，并确保两个进程的有效 UID 与目录属主匹配。下面的 `DirectoryOrCreate` 由 Kubernetes 创建目录，但不会自动把目录授权给业务用户，部署时仍需设置合适的属主和权限：

```yaml
volumeMounts:
  - name: shm-manager-state
    mountPath: /var/lib/agora/shm-manager
volumes:
  - name: shm-manager-state
    hostPath:
      path: /var/lib/agora/shm-manager
      type: DirectoryOrCreate
```

仅在同一个 Pod 内的容器间共享时，也可在该路径挂载同一个 `emptyDir`；它跨容器崩溃和重启保留清单，但 Pod 被删除时清单随之删除。`emptyDir` 不会自动与宿主机进程共享该路径。

非 Pod 部署通常不需要额外挂载。若 `/var/lib/agora` 尚不存在，进程必须有权限在 `/var/lib` 下创建它；普通非 root 用户通常需要部署预先创建目录并设置属主。权限不足或文件系统只读时仅记录状态初始化失败，媒体业务继续运行，异常退出回收能力随之降级。

只读根文件系统需要为 `/var/lib/agora/shm-manager` 提供可写挂载才能保存清单。容器被删除、重新创建后仍需回收旧 SHM 时，应使用与其 SHM 可见范围一致的宿主机存储，并确保协作 manager 看到同一个实际目录。

本次路径调整不会自动迁移旧 `/tmp/agora_shm_manager` 中的清单；升级部署需另行处理旧实例及其资源。

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

Manager API 失败时返回 `-errno`，同时保留正数 `errno`；调用方应使用
`ret < 0` 判断失败，或直接比较 `ret == -ENOENT` 等具体错误码，不再用
`ret == -1` 判断所有错误。`start/add/remove/write` 成功返回 `0`，`poll`
成功返回读取字节数或 `0`。音频 `write` 成功表示已拷贝入队，发送错误异步记录；
视频 `write` 通知失败时，SHM 数据可能已经写入。

Manager 启动成功后，server/client worker 仅在主动调用 `close()` 时退出。
收包或 keepalive 出错后在锁外等待 **20ms**：明确失效的 socket 进入重建；
其他 socket 错误连续 **3 次**后重建，正常轮询后清零，`EINTR` 与正常无数据
不计数。重建失败仍每隔 20ms 重试，保留 manager、读写表、缓存、SHM 映射
及所有权记录。客户端重建后立即发送 keepalive，服务端等待对端重新登记。
错误和恢复信息直接输出到 `stderr`，不做限流。

恢复期间，`poll()` 优先返回已有缓存；没有可读数据时返回 `-EAGAIN`
（包括尚未挂接的名称）。音频 `write()` 仍可入队，worker 继续定时写 SHM，
通知错误异步记录；视频 `write()` 仍先写 SHM，通知暂不可发时返回
`-EAGAIN`，不回滚。恢复后的新通知可继续触发挂接和读帧；不补发通知、不做
登记同步、不补历史帧，只有一次且已丢失的通知无需自动补偿。

业务层须先停止并等待所有 `write/poll/add/remove` 调用结束，再调用
`close()`；关闭会唤醒 worker 的等待并回收线程。不要从帧回调中调用
`close()`。内部正常空闲检查间隔为 10ms，失败等待使用单调时间，避免系统
时钟调整拉长重试间隔。

```text
./build/agora_manager_demo <port> <server_mode 0|1> <max_clients> <keepalive_ms> <write_shm_name>
```

| 参数 | 含义 |
|------|------|
| `port` | 127.0.0.1 UDP 端口（须 > 0） |
| `server_mode` | `1`：本进程 manager 为 UDP **server** 并绑定 `port`；`0`：为 **client** 连到 `port`（demo 无 writer 循环，仅等待信令） |
| `max_clients` / `keepalive_ms` | 仅 **server** 模式传给 `agora_localsock_server_create`（client 模式仍须填合法正数） |
| `write_shm_name` | **server_mode=1** 时：本进程创建/附着写 SHM，并用 localsock **client** 向本机 `port` 发 **APP**（整头）唤醒 manager |

**server_mode=1** 时：进程内同时有 **manager（UDP server）** 与 **localsock client** 发信令，无需第二进程即可联调。`payload_size` 在 demo 内固定为 **4096**；退出 **Ctrl+C**。manager 正常退出会 unlink 自己创建的写 SHM；异常退出遗留对象由下一次 manager start 根据磁盘清单恢复。

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
