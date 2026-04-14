# 共享内存 IPC 说明

原先基于 **shm_yuv** 的双槽 YUV 示例与源码已从本仓库移除。

当前实现为通用 **`agora_shm_ipc`**（单槽 seqlock + 可选 Unix 域 UDP 通知），用法与构建见：

- [agora_readme.md](agora_readme.md)
- [PLAN_shm_ipc.md](PLAN_shm_ipc.md)

历史设计笔记（不含现行源码）：[PLAN_shm_yuv_ipc.md](PLAN_shm_yuv_ipc.md)
