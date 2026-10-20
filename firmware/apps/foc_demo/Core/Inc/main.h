#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"

/* Shared with the DFU bootloader (see ../../Core/Inc/main.h). */
#define BOOT_MAGIC_ADDR     0x2001FFF0U
#define BOOT_MAGIC_DFU_REQ  0xDF11B007U

void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
