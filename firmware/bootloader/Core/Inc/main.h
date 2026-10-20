#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"

/* Flash layout: bootloader in sectors 0-1, application from sector 2. */
#define BOOT_APP_ADDR          0x08008000U
#define BOOT_FLASH_END         0x08100000U

/* Boot-request word in the top 256 B of SRAM, outside the bootloader's RAM.
 * An application may write BOOT_MAGIC_DFU_REQ here and reset to enter DFU. */
#define BOOT_MAGIC_ADDR        0x2001FFF0U
#define BOOT_MAGIC_DFU_REQ     0xDF11B007U
#define BOOT_MAGIC_DBLTAP      0xB007D8B1U
#define BOOT_DBLTAP_WINDOW_MS  500U

void Error_Handler(void);
void SystemClock_Config(void);

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
