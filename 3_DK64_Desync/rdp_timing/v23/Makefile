# rdp_timing v9 (libdragon)
#
# Requires the libdragon toolchain and libdragon (preview branch) installed in
# $N64_INST. See https://github.com/DragonMinded/libdragon
#
#   make        Build rdp_timing_v23.z64 (the captured display lists in filesystem/ are appended as a DragonFS image)
#   make clean

BUILD_DIR = build
include $(N64_INST)/include/n64.mk

all: rdp_timing_v23.z64

$(BUILD_DIR)/rdp_timing_v23.dfs: $(wildcard filesystem/*)
$(BUILD_DIR)/rdp_timing_v23.elf: $(BUILD_DIR)/main.o

rdp_timing_v23.z64: N64_ROM_TITLE = "RDP TIMING V23"
rdp_timing_v23.z64: $(BUILD_DIR)/rdp_timing_v23.dfs

clean:
	rm -rf $(BUILD_DIR) rdp_timing_v23.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
