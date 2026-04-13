/**
 * author: Wei
 * date: 2026-04-04
 */

#include "shm_yuv.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const size_t k_shm_yuv_map_size = sizeof(ShmYuvRegion);

int shm_yuv_open(const char *name, ShmYuvMap *out) {
  if (!name || !out) {
    errno = EINVAL;
    return -1;
  }

  memset(out, 0, sizeof(*out));
  out->fd = -1;

  int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0660);
  int created = 0;
  if (fd >= 0) {
    created = 1;
    if (ftruncate(fd, (off_t)k_shm_yuv_map_size) != 0) {
      int e = errno;
      (void)close(fd);
      (void)shm_unlink(name);
      errno = e;
      return -1;
    }
  } else if (errno == EEXIST) {
    fd = shm_open(name, O_RDWR, 0660);
    if (fd < 0) {
      return -1;
    }
  } else {
    return -1;
  }

  struct stat st;
  if (fstat(fd, &st) != 0) {
    int e = errno;
    (void)close(fd);
    errno = e;
    return -1;
  }
  if ((size_t)st.st_size < k_shm_yuv_map_size) {
    (void)close(fd);
    errno = EINVAL;
    return -1;
  }

  /*
   * Some platforms round shared memory objects up to a page size; fstat may
   * report st_size larger than sizeof(ShmYuvRegion). Map only the prefix we use.
   */
  void *p =
      mmap(NULL, k_shm_yuv_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    int e = errno;
    (void)close(fd);
    errno = e;
    return -1;
  }

  if (created) {
    memset(p, 0, k_shm_yuv_map_size);
  }

  out->region = (ShmYuvRegion *)p;
  out->mapped_size = k_shm_yuv_map_size;
  out->fd = fd;
  out->creator = created;
  return 0;
}

void shm_yuv_close(ShmYuvMap *m) {
  if (!m || !m->region) {
    return;
  }
  (void)munmap(m->region, m->mapped_size);
  m->region = NULL;
  m->mapped_size = 0;
  if (m->fd >= 0) {
    (void)close(m->fd);
  }
  m->fd = -1;
}

int shm_yuv_unlink(const char *name) {
  if (!name) {
    errno = EINVAL;
    return -1;
  }
  return shm_unlink(name);
}
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
 * @return 0 成功，-10001 参数不合法
 */

int shm_yuv_writer_session_begin(ShmYuvMap *m) {
  // check region:
  if (!m) {
    return -10001;
  }
  ShmYuvRegion *r = m->region;
  if (!r) {
    return -10002;  
  }
  // check region size:
  if (m->mapped_size < sizeof(ShmYuvRegion)) {
    return -10003;
  }
  // invalidate both slots
  for (unsigned i = 0; i < 2u; i++) {
    atomic_store_explicit(&r->slots[i].state, SHM_YUV_STATE_INVALID,
                          memory_order_release);
  }
  atomic_store_explicit(&r->active_slot, 0u, memory_order_release);
  return 0;
}

/**
 * 向共享内存中的非发布槽写入一帧数据，并发布。
 * 
 * @param m         shm_yuv_open 得到的 ShmYuvMap*
 * @param userid    用于标识写入者的字符串，不足 64 字节补 '\0'
 * @param seq       帧序号，单调递增
 * @param yuv_data  指向 YUV 数据的缓冲区（YUV420 格式，必须不少于 width*height*3/2 字节）
 * @param width     帧宽度（如 1920）
 * @param height    帧高度（如 1080）
 * 
 * 实现双槽协议：
 * - 只在“非当前发布槽”上写；
 * - 写入流程：state=WRITING，填充所有字段（含 yuv 数据），state=READY，原子发布 active_slot；
 * - 注意状态与槽索引的原子发布顺序，保证读端可见性。
 * 
 * 注意：本函数不阻塞，可能覆盖未被及时读取的帧。
 */

int shm_yuv_write_one_frame(ShmYuvMap *m, const char *userid, uint32_t seq,
                            uint8_t *yuv_data, int width, int height) {
  if (!m || !userid || !yuv_data) {
    return -10001;
  }
  ShmYuvRegion *r = m->region;
  if (!r) {
    return -10002;
  }

  // check buffer size to avoid overflowing yuv_data
  size_t yuv_data_size =
  (size_t)width * (size_t)height * 3u / 2u;
  if (yuv_data_size > SHM_YUV_SIZE) {
    return -10004;
  }


  // find empty slot
  unsigned pub = atomic_load_explicit(&r->active_slot, memory_order_relaxed);
  unsigned widx = (pub == 0u) ? 1u : 0u;
  ShmYuvSlot *s = &r->slots[widx];


  // update state to writing
  atomic_store_explicit(&s->state, SHM_YUV_STATE_WRITING, memory_order_release);

  // write userid
  memset(s->userid, 0, SHM_YUV_USERID_LEN);
  size_t ulen = strlen(userid);
  if (ulen > SHM_YUV_USERID_LEN) {
    ulen = SHM_YUV_USERID_LEN;
  }
  memcpy(s->userid, userid, ulen);


  // update width, height and sequence
  s->width = width;
  s->height = height;
  s->sequence = seq;


  // Copy YUV into slot
  memcpy(s->yuv_data, yuv_data, yuv_data_size);

  // update state to ready
  atomic_store_explicit(&s->state, SHM_YUV_STATE_READY, memory_order_release);
  atomic_store_explicit(&r->active_slot, widx, memory_order_release);
  return 0;
}

int shm_yuv_try_read_frame(ShmYuvMap *m, uint8_t *out_yuv_data, int *out_width,
                           int *out_height, char *out_userid, uint32_t *out_seq) {
  if (!m || !out_yuv_data || !out_width || !out_height || !out_userid) {
    return -10001;
  }
  ShmYuvRegion *r = m->region;
  if (!r) {
    return -10002;
  }

  unsigned idx = atomic_load_explicit(&r->active_slot, memory_order_acquire);
  if (idx >= 2u) {
    return -10005;
  }
  ShmYuvSlot *s = &r->slots[idx];

  uint32_t st0 = atomic_load_explicit(&s->state, memory_order_acquire);
  if (st0 != SHM_YUV_STATE_READY) {
    return -10003;
  }

  uint32_t seq0 = s->sequence;
  uint32_t w0 = s->width;
  uint32_t h0 = s->height;
  
 
  // fill out_yuv_data: 
  int max_buffer_size = *out_width * *out_height * 3 / 2;
  int yuv_data_size = w0 * h0 * 3 / 2;
  // fill current width and height
  *out_width = w0;
  *out_height = h0;
  *out_seq = seq0;

  // buffer is not enough
  if (yuv_data_size > max_buffer_size) {
    return -10004;
  }
  // fill out_yuv_data
  memcpy(out_yuv_data, s->yuv_data, yuv_data_size);
 
  

   // fill out_userid, max length is SHM_YUV_USERID_LEN-1
   memcpy( out_userid, s->userid, SHM_YUV_USERID_LEN);
   out_userid[SHM_YUV_USERID_LEN-1] = '\0';
 
  return 1;
}
