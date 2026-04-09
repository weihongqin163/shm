/**
 * author: Wei
 * date: 2026-04-04
 */

#include "shm_yuv.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t s_last_reported_seq;
uint8_t yuv_data[SHM_YUV_SIZE];
int width = 1920;
int height = 1080;
char userid[SHM_YUV_USERID_LEN];
uint32_t seq;
/** Returns 1 if a consistent frame was read, 0 if none / torn, -1 on OOM. */


int main(int argc, char **argv) {
  const char *name = "/shm_yuv1";
  if (argc >= 2) {
    name = argv[1];
  }

  ShmYuvMap m;
  if (shm_yuv_open(name, &m) != 0) {
    perror("shm_yuv_open");
    return 1;
  }
  printf("reader: shm=%s creator=%d (poll ~1ms)\n", name, m.creator);

  for (;;) {
    int r = shm_yuv_try_read_frame(&m, yuv_data, &width, &height, userid, &seq);
    if (r < 0) {
      fprintf(stderr, "shm_yuv_try_read_frame error %d\n", r);
      continue;
    }
    if (r == 1 && seq != s_last_reported_seq) {
      s_last_reported_seq = seq;
      printf("reader: seq=%u %dx%d userid=\"%.64s\"\n", seq, width, height, userid);  
    }
    else if (r == 0) {
      fprintf(stderr, "shm_yuv_try_read_frame no frame\n");
      continue;
    }
    else if (r == -1) {
      fprintf(stderr, "shm_yuv_try_read_frame error %d\n", r);
      continue;
    }

    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000L;
    (void)nanosleep(&ts, NULL);
  }
}
