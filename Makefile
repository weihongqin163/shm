# created by:wei
# copyright (c) 2026 Agora IO. All rights reserved.
# date: 2026-04-15

CC = cc
CFLAGS = -std=c11 -Wall -Wextra -O2 -Isrc

BUILD_DIR := build

UNAME_S := $(shell uname -s)
LIBS := -pthread
ifeq ($(UNAME_S),Linux)
LIBS += -lrt
endif

AGORA_OBJ := $(BUILD_DIR)/agora_shm_ipc.o
AGORA_MANAGER_OBJ := $(BUILD_DIR)/agora_shm_manager.o
AGORA_WRITER := $(BUILD_DIR)/agora_writer_demo
AGORA_READER := $(BUILD_DIR)/agora_reader_demo
AGORA_MANAGER_DEMO := $(BUILD_DIR)/agora_manager_demo
AGORA_LOCALSOCK_OBJ := $(BUILD_DIR)/agora_localsock.o
AGORA_LOCALSOCK_DEMO := $(BUILD_DIR)/agora_localsock_demo

.PHONY: all clean

all: $(AGORA_WRITER) $(AGORA_READER) $(AGORA_MANAGER_OBJ) $(AGORA_MANAGER_DEMO) $(AGORA_LOCALSOCK_DEMO)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(AGORA_OBJ): src/agora_shm_ipc.c src/agora_shm_ipc.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c src/agora_shm_ipc.c -o $@

$(AGORA_MANAGER_OBJ): src/agora_shm_manager.c src/agora_shm_manager.h src/agora_shm_ipc.h src/agora_localsock.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c src/agora_shm_manager.c -o $@

$(AGORA_WRITER): examples/agora_writer_demo.c $(AGORA_OBJ) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ examples/agora_writer_demo.c $(AGORA_OBJ) $(LIBS)

$(AGORA_READER): examples/agora_reader_demo.c $(AGORA_OBJ) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ examples/agora_reader_demo.c $(AGORA_OBJ) $(LIBS)

$(AGORA_MANAGER_DEMO): examples/agora_manager_demo.c $(AGORA_OBJ) $(AGORA_LOCALSOCK_OBJ) $(AGORA_MANAGER_OBJ) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ examples/agora_manager_demo.c $(AGORA_OBJ) $(AGORA_LOCALSOCK_OBJ) $(AGORA_MANAGER_OBJ) $(LIBS)

$(AGORA_LOCALSOCK_OBJ): src/agora_localsock.c src/agora_localsock.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c src/agora_localsock.c -o $@

$(AGORA_LOCALSOCK_DEMO): examples/agora_localsock_demo.c $(AGORA_LOCALSOCK_OBJ) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ examples/agora_localsock_demo.c $(AGORA_LOCALSOCK_OBJ) $(LIBS)

clean:
	rm -rf $(BUILD_DIR)
