# Compile the float fuzz core for a Cortex-M7 with hardware FPU (Daisy Seed:
# STM32H750, Teensy 4.x: i.MX RT1062) and report size + codegen checks.
#   make -C tools/embedded ARM_GCC=/path/to/arm-none-eabi-gcc/bin
ARM_GCC ?=
CXX := $(if $(ARM_GCC),$(ARM_GCC)/,)arm-none-eabi-g++
SIZE := $(if $(ARM_GCC),$(ARM_GCC)/,)arm-none-eabi-size
OBJDUMP := $(if $(ARM_GCC),$(ARM_GCC)/,)arm-none-eabi-objdump
CXXFLAGS := -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard \
	-O2 -std=c++20 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
	-Wall -Wextra -Wdouble-promotion \
	-I../../core
LDFLAGS := --specs=nosys.specs --specs=nano.specs -Wl,--gc-sections

all: fuzz_m7.elf
	$(SIZE) fuzz_m7.elf
	@echo "double-precision FPU ops inside the per-sample path (want 0):"
	@$(OBJDUMP) -d --no-show-raw-insn -C fuzz_m7.elf \
		| awk '/<.*FuzzFaceDKT<float>::process|<cd_fuzz_process/{p=1} p&&/^$$/{p=0} p' \
		| grep -cE 'vmul\.f64|vadd\.f64|vsub\.f64|vdiv\.f64|vfma\.f64|vcvt\.f64|__aeabi_d' || true

fuzz_m7.elf: fuzz_m7.cpp ../../core/circuit/FuzzFaceDK.h ../../core/circuit/FuzzFace.h
	$(CXX) $(CXXFLAGS) -DCD_FUZZ_TEST_MAIN $< $(LDFLAGS) -o $@

clean:
	rm -f fuzz_m7.elf

.PHONY: all clean
