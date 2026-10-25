# rdp_load_hazard_demo, bare-metal build (no libdragon runtime)
#
# Needs only the libdragon toolchain (mips64-elf-gcc) in $N64_INST and
# python3. The ROM boots with libdragon's compatibility IPL3
# (ipl3_compat.z64, public domain, signed for CIC 6102).
#
#   make        Build rdp_load_hazard_demo_bare.z64
#   make clean

N64_INST ?= /opt/libdragon
CC      = $(N64_INST)/bin/mips64-elf-gcc
OBJCOPY = $(N64_INST)/bin/mips64-elf-objcopy
CFLAGS  = -march=vr4300 -mtune=vr4300 -mabi=32 -mno-abicalls -fno-pic -G0 \
          -ffreestanding -fno-builtin -nostdlib -O2 -Wall -Wextra
TARGET  = rdp_load_hazard_demo_bare

all: $(TARGET).z64

$(TARGET).elf: crt0.S main.c font8x8.h link.ld
	$(CC) $(CFLAGS) -T link.ld -o $@ crt0.S main.c -lgcc

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(TARGET).z64: $(TARGET).bin ipl3_compat.z64 mkrom.py
	python3 mkrom.py ipl3_compat.z64 $< $@ "RDP LOADHAZARD DEMO"

clean:
	rm -f $(TARGET).elf $(TARGET).bin $(TARGET).z64

.PHONY: all clean
