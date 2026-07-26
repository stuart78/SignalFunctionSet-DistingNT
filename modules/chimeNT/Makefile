
NT_API_PATH := ../../distingNT_API
INCLUDE_PATH := $(NT_API_PATH)/include

# Prefer the official ARM GNU Toolchain (which includes newlib).
# Falls back to whatever's first on PATH.
ARM_TOOLCHAIN_BIN := /Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin
ifneq ($(wildcard $(ARM_TOOLCHAIN_BIN)/arm-none-eabi-c++),)
  CXX := $(ARM_TOOLCHAIN_BIN)/arm-none-eabi-c++
else
  CXX := arm-none-eabi-c++
endif

inputs := $(wildcard *.cpp)
outputs := $(patsubst %.cpp,plugins/%.o,$(inputs))

all: $(outputs)

clean:
	rm -f $(outputs)

plugins/%.o: %.cpp
	mkdir -p $(@D)
	$(CXX) -std=c++11 -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -fno-rtti -fno-exceptions -Os -fPIC -Wall -I$(INCLUDE_PATH) -c -o $@ $^
