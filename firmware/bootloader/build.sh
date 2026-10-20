#!/usr/bin/env bash
# Build the DengFOC USB DFU bootloader with the STM32CubeCLT GCC toolchain.
set -euo pipefail
cd "$(dirname "$0")"

GCC_BIN="${GCC_BIN:-/e/ST/STM32CubeCLT_1.18.0/GNU-tools-for-STM32/bin}"
CC="$GCC_BIN/arm-none-eabi-gcc"
OBJCOPY="$GCC_BIN/arm-none-eabi-objcopy"
SIZE="$GCC_BIN/arm-none-eabi-size"

OUT=build
mkdir -p "$OUT/obj"

CPU="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard"
DEFS="-DSTM32F407xx -DUSE_HAL_DRIVER"
INCS="-ICore/Inc -IUSB -I../lib/Middlewares/USB_Device/Core/Inc -I../lib/Middlewares/USB_Device/Class/DFU/Inc \
      -I../lib/Drivers/STM32F4xx_HAL_Driver/Inc -I../lib/Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../lib/Drivers/CMSIS/Include"
CFLAGS="$CPU $DEFS $INCS -Os -g -Wall -ffunction-sections -fdata-sections -std=gnu11"

SRCS=(
  Core/Src/main.c Core/Src/stm32f4xx_it.c ../lib/system_stm32f4xx.c Core/Src/usbd_dfu_flash.c
  USB/usbd_conf.c USB/usbd_desc.c
  ../lib/Middlewares/USB_Device/Core/Src/usbd_core.c ../lib/Middlewares/USB_Device/Core/Src/usbd_ctlreq.c
  ../lib/Middlewares/USB_Device/Core/Src/usbd_ioreq.c ../lib/Middlewares/USB_Device/Class/DFU/Src/usbd_dfu.c
)
for f in ../lib/Drivers/STM32F4xx_HAL_Driver/Src/*.c; do SRCS+=("$f"); done

OBJS=()
for src in "${SRCS[@]}"; do
  obj="$OUT/obj/$(basename "${src%.c}").o"
  "$CC" $CFLAGS -c "$src" -o "$obj"
  OBJS+=("$obj")
done
"$CC" $CPU -x assembler-with-cpp -c ../lib/startup_stm32f407xx.s -o "$OUT/obj/startup.o"
OBJS+=("$OUT/obj/startup.o")

"$CC" $CPU -T link.ld -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections \
  -Wl,-Map="$OUT/dfu_boot.map" -Wl,--print-memory-usage "${OBJS[@]}" -o "$OUT/dfu_boot.elf"
"$OBJCOPY" -O binary "$OUT/dfu_boot.elf" "$OUT/dfu_boot.bin"
"$OBJCOPY" -O ihex "$OUT/dfu_boot.elf" "$OUT/dfu_boot.hex"
"$SIZE" "$OUT/dfu_boot.elf"
