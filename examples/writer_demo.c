/**
 * author: Wei
 * date: 2026-04-04
 */

#include "shm_yuv.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void sleep_frame_interval(void) {
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 1000000000L / 30L;
  (void)nanosleep(&ts, NULL);
}



int main(int argc, char **argv) {
  /* POSIX shared memory names are short on some platforms (e.g. macOS). */
  const char *name = "/shm_yuv1";
  const char *userid = "user-demo-0001";
  if (argc >= 2) {
    name = argv[1];
  }
  if (argc >= 3) {
    userid = argv[2];
  }

  // cal first sequence number
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  
  uint32_t seq = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);


 

  ShmYuvMap m;
  if (shm_yuv_open(name, &m) != 0) {
    perror("shm_yuv_open");
    return 1;
  }
  printf("writer: shm=%s creator=%d\n", name, m.creator);
  if (shm_yuv_writer_session_begin(&m) != 0) {
    fprintf(stderr, "shm_yuv_writer_session_begin failed\n");
    shm_yuv_close(&m);
    return 1;
  }

  uint8_t yuv_data[SHM_YUV_SIZE];
  int width = 1920;
  int height = 1080;
  for (;;) {
   
    if (shm_yuv_write_one_frame(&m, userid, seq, yuv_data, width, height) != 0) {
      fprintf(stderr, "shm_yuv_write_one_frame failed\n");
      shm_yuv_close(&m);
      return 1;
    }
    printf("writer: published seq=%u active=%u\n", seq,
           atomic_load_explicit(&m.region->active_slot, memory_order_relaxed));
    seq++;
    sleep_frame_interval();
  } 
}
