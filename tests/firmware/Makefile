CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror

ROOT := ../..
BUILD_DIR := build
TARGET := $(BUILD_DIR)/error_manager_tests

INCLUDES := -I./stubs -I$(ROOT)/main
SOURCES := \
  $(ROOT)/main/error_manager.cpp \
  stubs/stubs.cpp \
  test_error_manager.cpp

all: $(TARGET)

$(TARGET): $(SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SOURCES) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

test: run

clean:
	rm -rf $(BUILD_DIR)
