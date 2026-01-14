CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror

ROOT := ../..
BUILD_DIR := build
ERROR_TARGET := $(BUILD_DIR)/error_manager_tests
UTILS_TARGET := $(BUILD_DIR)/utils_tests
SD_TARGET := $(BUILD_DIR)/sd_cleanup_tests

INCLUDES := -I./stubs -I$(ROOT)/main
COMMON_SOURCES := \
  $(ROOT)/main/app_utils.cpp \
  stubs/stubs.cpp

ERROR_SOURCES := \
  $(ROOT)/main/error_manager.cpp \
  test_error_manager.cpp

UTILS_SOURCES := \
  test_utils.cpp

SD_SOURCES := \
  $(ROOT)/main/sd_maintenance.cpp \
  test_sd_cleanup.cpp

all: $(ERROR_TARGET) $(UTILS_TARGET) $(SD_TARGET)

$(ERROR_TARGET): $(COMMON_SOURCES) $(ERROR_SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(COMMON_SOURCES) $(ERROR_SOURCES) -o $(ERROR_TARGET)

$(UTILS_TARGET): $(COMMON_SOURCES) $(UTILS_SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(COMMON_SOURCES) $(UTILS_SOURCES) -o $(UTILS_TARGET)

$(SD_TARGET): stubs/stubs.cpp $(SD_SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) stubs/stubs.cpp $(SD_SOURCES) -o $(SD_TARGET)

run: $(ERROR_TARGET) $(UTILS_TARGET) $(SD_TARGET)
	./$(ERROR_TARGET)
	./$(UTILS_TARGET)
	./$(SD_TARGET)

test: run

clean:
	rm -rf $(BUILD_DIR)
