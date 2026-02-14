# Makefile for M1 GBA Emulator

CXX := clang++
CXXFLAGS := -std=c++20 -O3 -Wall -Wextra -fobjc-arc -arch arm64 -Isrc -Isrc/core -Isrc/platform
LDFLAGS := -framework Cocoa -framework Metal -framework MetalKit -framework AudioToolbox -arch arm64

SRC_DIR := src
BUILD_DIR := build
BIN_DIR := bin

# Find all source files
SRCS_CPP := $(shell find $(SRC_DIR) -name "*.cpp")
SRCS_MM := $(shell find $(SRC_DIR) -name "*.mm")

# Generate object file paths
OBJS := $(SRCS_CPP:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o) \
        $(SRCS_MM:$(SRC_DIR)/%.mm=$(BUILD_DIR)/%.o)

TARGET := $(BIN_DIR)/GBAEmu

.PHONY: all clean run directories

all: directories $(TARGET)

directories:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BIN_DIR)

$(TARGET): $(OBJS)
	@echo "Linking $@"
	@$(CXX) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.mm
	@mkdir -p $(dir $@)
	@echo "Compiling $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD_DIR) $(BIN_DIR)

run: all
	@cd $(BIN_DIR) && ./$(notdir $(TARGET))
