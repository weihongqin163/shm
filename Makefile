# created by:wei
# copyright (c) 2026 Agora IO. All rights reserved.
# date: 2026-04-04

CC = cc
CFLAGS = -std=c11 -Wall -Wextra -O2 -Isrc

BUILD_DIR := build

UNAME_S := $(shell uname -s)
LIBS :=
ifeq ($(UNAME_S),Linux)
LIBS += -lrt
endif

AGORA_OBJ := $(BUILD_DIR)/agora_shm_ipc.o
AGORA_WRITER := $(BUILD_DIR)/agora_writer_demo
AGORA_READER := $(BUILD_DIR)/agora_reader_demo

.PHONY: all clean

all: $(AGORA_WRITER) $(AGORA_READER)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(AGORA_OBJ): src/agora_shm_ipc.c src/agora_shm_ipc.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c src/agora_shm_ipc.c -o $@

$(AGORA_WRITER): examples/agora_writer_demo.c $(AGORA_OBJ) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ examples/agora_writer_demo.c $(AGORA_OBJ) $(LIBS)

$(AGORA_READER): examples/agora_reader_demo.c $(AGORA_OBJ) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ examples/agora_reader_demo.c $(AGORA_OBJ) $(LIBS)

clean:
	rm -rf $(BUILD_DIR)
