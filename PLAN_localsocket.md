# 本机 UDP 会话设计（127.0.0.1）

## 目标

- 在 **IPv4 回环 `127.0.0.1`** 上使用 **UDP** 做进程间通信（纯 C，POSIX / BSD socket）。
- **服务端**：绑定调用方指定的 **端口 `port`**。
- **客户端**：不显式 `bind` 本地端口（由内核分配 ephemeral port）。
- **服务端**：维护已知的 **客户端地址表**（`struct sockaddr_in` + 元数据）。
- **客户端**：按 **`keepalive_interval`** 周期发送 **keep-alive** 报文。
- **服务端**：对每个客户端维护 **最后活跃时间**；若超过 **`5 * keepalive_interval`** 未收到任何来自该地址的报文（含 keep-alive），则从表中 **删除** 该客户端。
- **实现语言**：纯 C（不依赖 C++ 运行时）。
- **平台假定**：**小端主机**（如 x86/x86-64、**aarch64 LE**）；链路上整数字段与公开类型 **`agora_localsock_header`**（packed）内存布局一致，**不做**跨字节序转换。大端或混合端环境不在支持范围。
- **构建策略**：**不**对非小端目标使用 `#error` 等编译拦截，仅在本文档与头文件注释中声明约定（定稿 **1-A**）。

## 非目标（首版可不做）

- NAT 或跨机通信。
- IPv6（若需要可在后续章节扩展 `::1` 双栈）。
- 可靠传输、有序、重传（仍为 UDP 语义）。
- 大端主机上的互操作与通用 `#ifdef` 端序适配。

## 协议与报文格式（建议）

为区分 **keep-alive** 与 **业务数据**，定义 **12 字节 packed 头**（`agora_localsock.h` 中 `agora_localsock_header`；在小端上与 `uint32_t`/`uint16_t` 内存序一致，**不**单独规定网络序或 `hton*` 转换）：

| 字段 | 类型 | 说明 |
|------|------|------|
| `magic` | `uint32_t` | 魔数，如 `0x4C4F4341`（`'LOCA'`） |
| `ver` | `uint16_t` | 协议版本 `1` |
| `msg_type` | `uint16_t` | `1` = keep-alive，`2` = APP 业务，`3` = WRITECMD（manager 写后信令，负载为 `AgoraShmIpcFrameMeta`） |
| `payload_len` | `uint32_t` | 紧跟其后的负载长度；keep-alive 可为 `0` |

- **keep-alive**：`msg_type == 1`，`payload_len == 0`（总长度 = 固定头）。
- **APP（2）**：业务负载由上层定义（如 manager 侧整头 `AgoraShmIpcHeader`）。
- **WRITECMD（3）**：manager 写路径信令；负载为 **`AgoraShmIpcFrameMeta`**（见 `PLAN_shm_ipc_manager.md` 3.7）。
- **未知 `msg_type`（已定稿）**：若整包通过下文「包头完全合法」校验，对 **已在客户端表中** 的源地址 **仍刷新** `last_seen_mono`（视为对端仍存活）；**不**凭未知类型 **新建** 表项（新源须先发送已登记语义报文，如 keep-alive `msg_type==1`）。

**待定**：是否要求客户端在 keep-alive 中带 **client_id**（UUID/自增）以便服务端在 ephemeral port 变化后合并逻辑会话——首版可仅用 **五元组中的源地址** 作为键（端口不变则稳定；端口重建则视为新客户端）。

## 服务端设计

### 生命周期

1. `socket(AF_INET, SOCK_DGRAM, 0)`。
2. `setsockopt`：`SO_REUSEADDR`（Linux/macOS 调试重启）；可选 `SO_REUSEPORT`（若多进程共端口，首版可不启用）。
3. `bind` 到 `127.0.0.1:port`（或 `INADDR_ANY` 仅监听回环——首版建议显式绑定 `127.0.0.1` 以满足「本机」约束）。
4. 主循环：`poll` / `select` 在 socket 上等待可读。
5. `recvfrom` 读取报文；先做 **包头完全合法** 校验（见「错误与边界」），再解析 `msg_type`。
6. 对 **可刷新活跃时间** 的报文：若源已在表中则更新 **`last_seen_mono`**；若源不在表中且报文属于 **允许登记** 的类型（首版为 keep-alive 等已知 `msg_type`）且表未满，则 **插入**（见「客户端表」）。

### 客户端表

- **容量（已定稿）**：由 `local_udp_server_create(..., max_clients, ...)` 传入上限（`size_t` 或 `uint32_t`，实现时二选一并文档化）；表内 **线性查找** + 紧凑数组或空闲槽位策略由实现决定。
- **表项字段建议**：
  - `struct sockaddr_in peer`；
  - `uint64_t last_seen_mono`（单调时钟，见下）；
  - 可选：`uint32_t client_generation`（调试）。
- **插入与刷新（已定稿）**：仅当报文 **包头完全合法**（见下节）且 `msg_type` 属于 **允许登记** 的集合（首版至少含 `msg_type==1` keep-alive）时：若源 **已在表中**，更新 `last_seen_mono`；若 **不在表中** 且表 **未满**，追加新表项。若表 **已满** 且该地址 **尚未登记**：**拒绝新条目**——不插入、不刷新任何表项（实现可统计丢弃次数）。**未知 `msg_type`**：对已登记源仍 **刷新** `last_seen_mono`；对未登记源 **不插入**。

### 超时剔除

- 使用 **单调时钟** 避免系统时间跳变：`clock_gettime(CLOCK_MONOTONIC, &ts)`（Linux；macOS 同样可用）。
- 每次 `recvfrom` 后，仅当通过 **包头完全合法** 与 **刷新规则** 判定后，才更新对应源地址的 `last_seen_mono`。
- 每次循环（或在定时器间隔）扫描全表：若 `now_mono - last_seen_mono > 5 * keepalive_interval_ms`，则 **删除** 该项（`memmove` 压缩或标记空闲）。

**注意（已定稿）**：`keepalive_interval` 在对外 API 中统一为 **`uint32_t` 毫秒**；服务端超时阈值为 **`5 * keepalive_interval_ms`**（同一单位）。实现内部可换算为纳秒单调时间差。

### 服务端 API（草拟，纯 C）

```c
typedef struct local_udp_server local_udp_server;

int local_udp_server_create(uint16_t port, uint32_t keepalive_interval_ms,
                            size_t max_clients,
                            local_udp_server **out);
void local_udp_server_destroy(local_udp_server *s);

/* 阻塞一步：处理可读、刷新表、剔除超时；返回 0 或错误码 */
int local_udp_server_poll(local_udp_server *s, int timeout_ms);
```

业务回调（可选后续）：`typedef void (*on_datagram_fn)(const void *payload, size_t len, const struct sockaddr_in *from, void *user);`

## 客户端设计

1. `socket(AF_INET, SOCK_DGRAM, 0)`。
2. **不** `bind`（或 `bind` 端口 `0` 等价语义，首版直接不 bind）。
3. `connect(s, 127.0.0.1:port)` **可选**：使用 `connect` 后可用 `send`/`write` 简化发送；若不用 `connect`，则每次 `sendto` 到 `127.0.0.1:port`。
4. **keep-alive（实现修订）**：**不由**库内线程发送；由 **业务层** 按与服务端一致的周期调用 `agora_localsock_client_send_keepalive()`（周期应与服务端 `keepalive_interval_ms` 对齐，以便在 `5×` 超时前刷新）。
5. 可选：同槽发送业务数据与 keep-alive 共用已 `connect` 的 socket；多线程发送时由业务自行 **串行化**。

```c
typedef struct local_udp_client local_udp_client;

int local_udp_client_create(uint16_t server_port, local_udp_client **out);
void local_udp_client_destroy(local_udp_client *c);

int local_udp_client_send_keepalive(local_udp_client *c);
```

**已定稿（修订）**：keep-alive **默认由应用驱动**；库仅提供 `send_keepalive` 与连接生命周期。**例外**：当 client 由 **`agora_shm_manager`** 内部持有时，由 **manager 工作线程每 500ms** 调用一次 `agora_localsock_client_send_keepalive`（见 [`PLAN_shm_ipc_manager.md`](PLAN_shm_ipc_manager.md)），与「业务自行定时」不冲突。

### `agora_localsock_client_poll`（定稿，供 manager 等组合层）

```c
/**
 * Waits up to timeout_ms for a datagram (after connect, peer is fixed).
 * On success fills buf with the full UDP payload (localsock header + payload),
 * sets *out_len, returns 0. timeout_ms < 0 means wait indefinitely.
 * Returns -1 on error; EAGAIN if no datagram within timeout (if timeout >= 0).
 */
int agora_localsock_client_poll(agora_localsock_client *c, int timeout_ms,
                                void *buf, size_t cap, size_t *out_len);
```

- 实现要点：`poll`/`recv`（或 `recvmsg`），**非阻塞**或带超时；与 **`send_keepalive` / `send_data` 并发**时须 **互斥**（与现有 `send` 路径一致，在 `.c` 内对 `fd` 加锁或文档要求外部串行）。

## 时间与单位（须写进头文件注释）

- 约定：`keepalive_interval` 与超时 **`5 * keepalive_interval`** 均使用 **毫秒**（`uint32_t keepalive_interval_ms`）。
- 服务端剔除条件：`now_mono - last_seen > (uint64_t)5 * interval_ms * 1000000ULL`（若内部用纳秒）或统一用毫秒整数比较。

## 错误与边界

**包头完全合法（已定稿，与问卷 1-B 一致）**：同时满足——(1) UDP 负载长度 `>= sizeof(header)`；(2) `magic`、`ver` 与实现约定一致；(3) `payload_len` 与 **实际负载总长** 一致，即 `sizeof(header) + payload_len == recvfrom 返回长度`（防半包头/截断）；(4) 对 keep-alive：`msg_type==1` 且 `payload_len==0`。仅当满足上述校验后，才执行 **刷新 `last_seen_mono`** 或 **按规则插入**。**短包、`magic`/`ver` 错误、长度与 `payload_len` 不一致**：丢弃，**不刷新**任何客户端时间。

- `magic`/`ver` 不匹配：丢弃，不刷新。
- **未知 `msg_type`**：若头部长度校验通过，对已登记源 **刷新** `last_seen_mono`（问卷 5-A）；对未登记源 **不插入**（见上）。
- 客户端快速重启：新 ephemeral 端口 → 新表项；旧表项在超时后删除。

## 文件与构建（后续实现时）

| 路径 | 说明 |
|------|------|
| `src/agora_localsock.h` / `src/agora_localsock.c` | 当前实现；含 **`agora_localsock_client_poll`**（定稿） |
| `examples/agora_localsock_demo.c` | 可选 demo |
| `Makefile` | 已增加 `agora_localsock` 目标 |

## 已定稿决策

| 项目 | 结论 |
|------|------|
| `keepalive_interval` 单位 | **毫秒**（`uint32_t`） |
| 客户端表已满、且报文来自 **未登记** 的新源地址 | **拒绝新条目**（不插入、不刷新表；不踢 LRU） |
| 短包 / 非法头 / 头与 `payload_len` 不一致 | **不刷新**任何 `last_seen`；**仅**在 **包头完全合法** 后刷新或插入（问卷 **1-B**） |
| 客户端表容量 | **`local_udp_server_create` 传入 `max_clients`**（问卷 **2-B**） |
| 链路与整数字段 | **小端主机**（含 aarch64 LE）；packed struct + `memcpy`；无通用端序适配（问卷 **3-A** 在目标平台上成立） |
| 非小端编译防护 | **无** `#error`；仅靠文档与头注释（定稿 **1-A**） |
| 支持 CPU 范围 | **凡小端即可**（含 aarch64，不限于 x86）（定稿 **2-B**） |
| 包头类型公开 | **`agora_localsock_header` 在 `agora_localsock.h` 公开**（定稿 **3-B**） |
| 客户端 keep-alive 驱动 | **默认**：业务层定时调用 `send_keepalive`；**嵌入 manager 的 client**：由 manager 线程 **每 500ms** 调用（见 manager PLAN） |
| `magic`/`ver` 合法但 `msg_type` 未知 | 对已登记源 **仍刷新** `last_seen_mono`；对未登记源 **不新建**（问卷 **5-A**） |

---

**文档版本**：已纳入上述问卷结论、**`client_poll`**、manager 嵌入 client 的 **500ms keepalive** 例外；与实现代码可迭代同步。
