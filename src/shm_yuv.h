/**
 * author: Wei
 * date: 2026-04-04
 */

#ifndef SHM_YUV_H
#define SHM_YUV_H

#include "shm_yuv_layout.h"

#include <stddef.h>

typedef struct ShmYuvMap {
  ShmYuvRegion *region;
  size_t mapped_size;
  int fd;
  int creator;
} ShmYuvMap;

/**
 * Opens POSIX shared memory: first opener uses O_CREAT|O_EXCL, ftruncates, and
 * zero-initializes the region; others attach and must not memset the region.
 * Returns 0 on success.
 */
int shm_yuv_open(const char *name, ShmYuvMap *out);

void shm_yuv_close(ShmYuvMap *m);

/** Unlinks the shared memory object (name). */
int shm_yuv_unlink(const char *name);

/**
 * Writer-side recovery: invalidate both slots and reset published index.
 * Call once per writer process start before publishing frames.
 */
/**
 * @brief 写端会话初始化：清理所有槽为无效，重置 active_slot。
 *
 * 写进程每次启动（不论是首次还是重启、是否创建者），在第一次写入前应调用本函数，
 * 以避免上次崩溃遗留的脏状态，保证读端不会遇到“写入中”卡死点。
 * 
 * - 对所有 slots[i].state 以 memory_order_release 置为 SHM_YUV_STATE_INVALID（0）。
 * - active_slot 以 memory_order_release 置为 0（任一均可，约定为 0）。
 * 
 * @param m 通过 shm_yuv_open 得到的 ShmYuvMap*
 * @return 0 成功，负数表示错误
 */
int shm_yuv_writer_session_begin(ShmYuvMap *m);

/**
 * 向共享内存中的非发布槽写入一帧数据，并发布。
 * 
 * @param m         shm_yuv_open 得到的 ShmYuvMap*
 * @param userid    用于标识写入者的字符串，不足 64 字节补 '\0'
 * @param seq       帧序号，单调递增
 * @param yuv_data  指向 YUV 数据的缓冲区（YUV420 格式，必须不少于 width*height*3/2 字节）
 * @param width     帧宽度（如 1920）
 * @param height    帧高度（如 1080）
 * @return 0 on success, -10001 on error
 * 
 * 实现双槽协议：
 * - 只在“非当前发布槽”上写；
 * - 写入流程：state=WRITING，填充所有字段（含 yuv 数据），state=READY，原子发布 active_slot；
 * - 注意状态与槽索引的原子发布顺序，保证读端可见性。
 * 
 * 注意：本函数不阻塞，可能覆盖未被及时读取的帧。
 */

 int shm_yuv_write_one_frame(ShmYuvMap *m, const char *userid, uint32_t seq, uint8_t *yuv_data, int width, int height);

/**
 * @brief 读取一帧 YUV 数据并输出到参数中，若无可用帧或缓冲区不足则返回错误码。
 * 
 * @param m           shm_yuv_open 得到的 ShmYuvMap*
 * @param out_yuv_data 输出缓冲区，需足够大以容纳一帧 (width*height*3/2 字节)
 * @param out_width    [输入/输出] 传入缓冲区宽度，返回帧实际宽度
 * @param out_height   [输入/输出] 传入缓冲区高度，返回帧实际高度
 * @param out_userid   用于接收帧的 userid，需至少 SHM_YUV_USERID_LEN 字节
 * @param out_seq      返回帧的序号（可选，若不关心可传 NULL）
 * @return 1 表示读取到有效完整帧，0 表示无可用，负数为错误码
 * 
 * 注意事项：
 * - 本函数不会阻塞。
 * - 需预先分配好合适的 out_yuv_data/out_userid 缓冲区。
 */
 int shm_yuv_try_read_frame(ShmYuvMap *m, uint8_t *out_yuv_data, int *out_width, int *out_height, char *out_userid, uint32_t *out_seq);

#endif /* SHM_YUV_H */
