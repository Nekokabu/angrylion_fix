# rdp_carry_probe_v5 (libdragon)
#
# Requires the libdragon toolchain and libdragon (preview branch) installed in
# $N64_INST. See https://github.com/DragonMinded/libdragon
#
#   make        Build rdp_carry_probe_v5.z64
#   make clean

BUILD_DIR = build
include $(N64_INST)/include/n64.mk

all: rdp_carry_probe_v5.z64

$(BUILD_DIR)/rdp_carry_probe_v5.elf: $(BUILD_DIR)/main.o

rdp_carry_probe_v5.z64: N64_ROM_TITLE = "RDP CARRY PROBE5"

clean:
	rm -rf $(BUILD_DIR) rdp_carry_probe_v5.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
