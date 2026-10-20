#!/usr/bin/env bash
# Build the board self-test application (linked at 0x08008000, flashed over USB DFU).
set -euo pipefail
cd "$(dirname "$0")"
LIB=../../lib

GCC_BIN="${GCC_BIN:-/e/ST/STM32CubeCLT_1.18.0/GNU-tools-for-STM32/bin}"
CC="$GCC_BIN/arm-none-eabi-gcc"
OBJCOPY="$GCC_BIN/arm-none-eabi-objcopy"
SIZE="$GCC_BIN/arm-none-eabi-size"

OUT=build
mkdir -p "$OUT/obj"

CPU="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard"
DEFS="-DSTM32F407xx -DUSE_HAL_DRIVER -DUSER_VECT_TAB_ADDRESS -DVECT_TAB_OFFSET=0x8000U"
INCS="-ICore/Inc -I../common -I$LIB/Middlewares/USB_Device/Core/Inc -I$LIB/Middlewares/USB_Device/Class/CDC/Inc \
      -I$LIB/Drivers/STM32F4xx_HAL_Driver/Inc -I$LIB/Drivers/CMSIS/Device/ST/STM32F4xx/Include -I$LIB/Drivers/CMSIS/Include"
CFLAGS="$CPU $DEFS $INCS -Os -g -Wall -ffunction-sections -fdata-sections -std=gnu11"

HAL=$LIB/Drivers/STM32F4xx_HAL_Driver/Src
SRCS=(
  Core/Src/main.c Core/Src/stm32f4xx_it.c ../common/usbd_conf.c ../common/usbd_desc.c ../common/usbd_cdc_if.c
  $LIB/system_stm32f4xx.c
  $LIB/Middlewares/USB_Device/Core/Src/usbd_core.c $LIB/Middlewares/USB_Device/Core/Src/usbd_ctlreq.c
  $LIB/Middlewares/USB_Device/Core/Src/usbd_ioreq.c $LIB/Middlewares/USB_Device/Class/CDC/Src/usbd_cdc.c
  $HAL/stm32f4xx_hal.c $HAL/stm32f4xx_hal_cortex.c $HAL/stm32f4xx_hal_rcc.c $HAL/stm32f4xx_hal_rcc_ex.c
  $HAL/stm32f4xx_hal_gpio.c $HAL/stm32f4xx_hal_pwr.c $HAL/stm32f4xx_hal_pwr_ex.c $HAL/stm32f4xx_hal_flash.c
  $HAL/stm32f4xx_hal_flash_ex.c $HAL/stm32f4xx_hal_pcd.c $HAL/stm32f4xx_hal_pcd_ex.c $HAL/stm32f4xx_ll_usb.c
  $HAL/stm32f4xx_hal_adc.c $HAL/stm32f4xx_hal_adc_ex.c $HAL/stm32f4xx_hal_spi.c $HAL/stm32f4xx_hal_dma.c
  $HAL/stm32f4xx_hal_dma_ex.c
)

OBJS=()
for src in "${SRCS[@]}"; do
  obj="$OUT/obj/$(basename "${src%.c}").o"
  "$CC" $CFLAGS -c "$src" -o "$obj"
  OBJS+=("$obj")
done
"$CC" $CPU -x assembler-with-cpp -c $LIB/startup_stm32f407xx.s -o "$OUT/obj/startup.o"
OBJS+=("$OUT/obj/startup.o")

"$CC" $CPU -T link.ld -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections \
  -Wl,-Map="$OUT/selftest.map" -Wl,--print-memory-usage "${OBJS[@]}" -o "$OUT/selftest.elf"
"$OBJCOPY" -O binary "$OUT/selftest.elf" "$OUT/selftest.bin"
"$OBJCOPY" -O ihex "$OUT/selftest.elf" "$OUT/selftest.hex"
"$SIZE" "$OUT/selftest.elf"
