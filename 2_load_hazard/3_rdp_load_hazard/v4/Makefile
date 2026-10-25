# rdp_load_hazard_v4 (libdragon)
#
# Requires the libdragon toolchain and libdragon (preview branch) installed in
# $N64_INST. See https://github.com/DragonMinded/libdragon
#
#   make        Build rdp_load_hazard_v4.z64
#   make clean

BUILD_DIR = build
include $(N64_INST)/include/n64.mk

all: rdp_load_hazard_v4.z64

$(BUILD_DIR)/rdp_load_hazard_v4.elf: $(BUILD_DIR)/main.o

rdp_load_hazard_v4.z64: N64_ROM_TITLE = "RDP LOAD HAZARD4"

clean:
	rm -rf $(BUILD_DIR) rdp_load_hazard_v4.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
