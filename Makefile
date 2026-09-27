# Dummy DJ Set build. `make` builds ./build/dummydj, `make run` starts it.
CXX      ?= clang++
CC       ?= clang
BUILD    := build
TARGET   := $(BUILD)/dummydj
INCLUDES := -Isrc -Ithird_party/sokol -Ithird_party/sokol/util -Ithird_party/imgui -Ithird_party/dr_libs
OPT      ?= -O2
CXXFLAGS := -std=c++17 $(OPT) -g -Wall -Wextra $(INCLUDES) -MMD -MP
CFLAGS   := $(OPT) -g $(INCLUDES) -MMD -MP
# Third-party code is built without our warning flags.
TP_CXXFLAGS := -std=c++17 $(OPT) -g $(INCLUDES) -MMD -MP

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  PLATFORM_SRC := src/platform/sokol_impl.mm
  LDLIBS := -framework Cocoa -framework QuartzCore -framework Metal -framework MetalKit -framework AudioToolbox
  TP_OBJCFLAGS := -fobjc-arc
else
  PLATFORM_SRC := src/platform/sokol_impl.cpp
  LDLIBS := -lX11 -lXi -lXcursor -lGL -lasound -ldl -lm -lpthread
  TP_OBJCFLAGS :=
endif

APP_SRCS := $(wildcard src/*.cpp)
IMGUI_SRCS := third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp \
              third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp \
              third_party/imgui/imgui_demo.cpp

APP_OBJS   := $(patsubst src/%.cpp,$(BUILD)/app/%.o,$(APP_SRCS))
IMGUI_OBJS := $(patsubst third_party/imgui/%.cpp,$(BUILD)/imgui/%.o,$(IMGUI_SRCS))
PLAT_OBJS  := $(BUILD)/platform/sokol_impl.o $(BUILD)/platform/dr_impl.o
OBJS := $(APP_OBJS) $(IMGUI_OBJS) $(PLAT_OBJS)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) -o $@ $^ $(LDLIBS) -lpthread

$(BUILD)/app/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/imgui/%.o: third_party/imgui/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(TP_CXXFLAGS) -c $< -o $@

$(BUILD)/platform/sokol_impl.o: $(PLATFORM_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(TP_CXXFLAGS) $(TP_OBJCFLAGS) -c $< -o $@

$(BUILD)/platform/dr_impl.o: src/platform/dr_impl.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
-include $(OBJS:.o=.d)
