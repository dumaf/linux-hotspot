CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c99 -D_GNU_SOURCE
LDFLAGS = -lpthread

SRC_DIR = src
BUILD_DIR = build
TEST_DIR = tests

SRCS = $(SRC_DIR)/main.c $(SRC_DIR)/packet_engine.c $(SRC_DIR)/qr.c
OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

TARGET = packet_engine

TEST_SRCS = $(TEST_DIR)/test_packet_engine.c $(SRC_DIR)/packet_engine.c $(SRC_DIR)/qr.c
TEST_TARGET = $(BUILD_DIR)/test_packet_engine

.PHONY: all clean test

all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

$(TEST_TARGET): $(TEST_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_SRCS) -o $@ $(LDFLAGS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

