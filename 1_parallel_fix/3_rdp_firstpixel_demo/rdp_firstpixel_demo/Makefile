# rdp_firstpixel_demo (libdragon)
#
# Requires the libdragon toolchain and libdragon (preview branch) installed in
# $N64_INST. See https://github.com/DragonMinded/libdragon
#
#   make        Build rdp_firstpixel_demo.z64
#   make clean

BUILD_DIR = build
include $(N64_INST)/include/n64.mk

all: rdp_firstpixel_demo.z64

$(BUILD_DIR)/rdp_firstpixel_demo.elf: $(BUILD_DIR)/main.o

rdp_firstpixel_demo.z64: N64_ROM_TITLE = "RDP FIRSTPIXEL"

clean:
	rm -rf $(BUILD_DIR) rdp_firstpixel_demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
