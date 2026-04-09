/**
 * author: Wei
 * date: 2026-04-04
 *
 * Fixed layout for POSIX shared memory YUV frame exchange (two slots).
 */

#ifndef SHM_YUV_LAYOUT_H
#define SHM_YUV_LAYOUT_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define SHM_YUV_USERID_LEN 64u
#define SHM_YUV_WIDTH_DEFAULT 1920u
#define SHM_YUV_HEIGHT_DEFAULT 1080u
#define SHM_YUV_SIZE (1920u * 1080u * 3u / 2u)

/** Slot lifecycle: invalid / ready to read / writer holds the slot. */
enum ShmYuvSlotState {
  SHM_YUV_STATE_INVALID = 0u,
  SHM_YUV_STATE_READY = 1u,
  SHM_YUV_STATE_WRITING = 2u,
};

typedef struct ShmYuvSlot {
  char userid[SHM_YUV_USERID_LEN];
  _Atomic uint_least32_t state;
  uint32_t width;
  uint32_t height;
  uint32_t sequence;
  uint8_t yuv_data[SHM_YUV_SIZE];
} ShmYuvSlot;

typedef struct ShmYuvRegion {
  _Atomic unsigned int active_slot;
  ShmYuvSlot slots[2];
} ShmYuvRegion;

_Static_assert(SHM_YUV_SIZE == 3110400u, "YUV byte count");
_Static_assert(offsetof(ShmYuvSlot, userid) == 0u, "userid offset");
_Static_assert(offsetof(ShmYuvSlot, state) == SHM_YUV_USERID_LEN, "state offset");
_Static_assert(offsetof(ShmYuvSlot, width) ==
                   SHM_YUV_USERID_LEN + sizeof(_Atomic uint_least32_t),
               "width offset");
_Static_assert(offsetof(ShmYuvSlot, height) ==
                   SHM_YUV_USERID_LEN + sizeof(_Atomic uint_least32_t) +
                       sizeof(uint32_t),
               "height offset");
_Static_assert(offsetof(ShmYuvSlot, sequence) ==
                   SHM_YUV_USERID_LEN + sizeof(_Atomic uint_least32_t) +
                       2u * sizeof(uint32_t),
               "sequence offset");
_Static_assert(offsetof(ShmYuvSlot, yuv_data) ==
                   SHM_YUV_USERID_LEN + sizeof(_Atomic uint_least32_t) +
                       3u * sizeof(uint32_t),
               "yuv_data offset");
_Static_assert(sizeof(ShmYuvRegion) ==
                   sizeof(_Atomic unsigned int) + 2u * sizeof(ShmYuvSlot),
               "ShmYuvRegion size");

#endif /* SHM_YUV_LAYOUT_H */
