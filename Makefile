# author: Wei
# date: 2026-04-04

CC = cc
CFLAGS = -std=c11 -Wall -Wextra -O2 -Isrc

BUILD_DIR := build

UNAME_S := $(shell uname -s)
LIBS :=
ifeq ($(UNAME_S),Linux)
LIBS += -lrt
endif

WRITER := $(BUILD_DIR)/writer_demo
READER := $(BUILD_DIR)/reader_demo
SHM_OBJ := $(BUILD_DIR)/shm_yuv.o

.PHONY: all clean

all: $(WRITER) $(READER)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(SHM_OBJ): src/shm_yuv.c src/shm_yuv.h src/shm_yuv_layout.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c src/shm_yuv.c -o $@

$(WRITER): examples/writer_demo.c $(SHM_OBJ) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ examples/writer_demo.c $(SHM_OBJ) $(LIBS)

$(READER): examples/reader_demo.c $(SHM_OBJ) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ examples/reader_demo.c $(SHM_OBJ) $(LIBS)

clean:
	rm -rf $(BUILD_DIR)
