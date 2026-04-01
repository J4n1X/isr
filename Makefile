CC = gcc
CFLAGS = -Wall -Wextra -O2
INCLUDES = -Ilib

BUILD_DIR = build
TARGET = $(BUILD_DIR)/isr

# Explicitly defining sources based on your tree
SRCS = main.c lib/isr.c lib/lz4.c

# Map source files to object files inside the build directory
OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))

.PHONY: all clean

all: $(TARGET)

# Link
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Compile
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)