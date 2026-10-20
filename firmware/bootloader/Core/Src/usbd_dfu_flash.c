/*
 * DfuSe flash media for STM32F405RG (1 MB, single bank).
 * Sectors 0-1 (bootloader) are reported read-only and rejected here as well.
 */
#include <string.h>
#include "main.h"
#include "usbd_dfu_flash.h"

#define FLASH_DESC_STR      "@Internal Flash   /0x08000000/02*016Ka,02*016Kg,01*064Kg,07*128Kg"
#define FLASH_ERASE_TIME    50U
#define FLASH_PROGRAM_TIME  50U

static uint16_t Flash_If_Init(void);
static uint16_t Flash_If_DeInit(void);
static uint16_t Flash_If_Erase(uint32_t Add);
static uint16_t Flash_If_Write(uint8_t *src, uint8_t *dest, uint32_t Len);
static uint8_t *Flash_If_Read(uint8_t *src, uint8_t *dest, uint32_t Len);
static uint16_t Flash_If_GetStatus(uint32_t Add, uint8_t Cmd, uint8_t *buffer);

USBD_DFU_MediaTypeDef USBD_DFU_Flash_fops =
{
  (uint8_t *)FLASH_DESC_STR,
  Flash_If_Init,
  Flash_If_DeInit,
  Flash_If_Erase,
  Flash_If_Write,
  Flash_If_Read,
  Flash_If_GetStatus,
};

static uint32_t get_sector(uint32_t addr)
{
  if (addr < 0x08010000U)
  {
    return (addr - 0x08000000U) / 0x4000U;          /* sectors 0-3, 16 KB */
  }
  if (addr < 0x08020000U)
  {
    return 4U;                                       /* sector 4, 64 KB */
  }
  return 5U + ((addr - 0x08020000U) / 0x20000U);     /* sectors 5-11, 128 KB */
}

static uint16_t Flash_If_Init(void)
{
  HAL_FLASH_Unlock();
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                         FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
  return USBD_OK;
}

static uint16_t Flash_If_DeInit(void)
{
  HAL_FLASH_Lock();
  return USBD_OK;
}

static uint16_t Flash_If_Erase(uint32_t Add)
{
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t sector_error = 0U;

  if (Add < BOOT_APP_ADDR || Add >= BOOT_FLASH_END)
  {
    return USBD_FAIL;
  }

  erase.TypeErase = FLASH_TYPEERASE_SECTORS;
  erase.Sector = get_sector(Add);
  erase.NbSectors = 1U;
  erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
  if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK)
  {
    return USBD_FAIL;
  }
  return USBD_OK;
}

static uint16_t Flash_If_Write(uint8_t *src, uint8_t *dest, uint32_t Len)
{
  uint32_t addr = (uint32_t)dest;

  if (addr < BOOT_APP_ADDR || (addr + Len) > BOOT_FLASH_END || (addr & 3U) != 0U)
  {
    return USBD_FAIL;
  }

  for (uint32_t i = 0U; i < Len; i += 4U)
  {
    uint32_t word;
    memcpy(&word, src + i, sizeof(word));
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, word) != HAL_OK)
    {
      return USBD_FAIL;
    }
    if (*(volatile uint32_t *)(addr + i) != word)
    {
      return USBD_FAIL;
    }
  }
  return USBD_OK;
}

static uint8_t *Flash_If_Read(uint8_t *src, uint8_t *dest, uint32_t Len)
{
  memcpy(dest, src, Len);
  return dest;
}

static uint16_t Flash_If_GetStatus(uint32_t Add, uint8_t Cmd, uint8_t *buffer)
{
  (void)Add;
  uint16_t t = (Cmd == DFU_MEDIA_PROGRAM) ? FLASH_PROGRAM_TIME : FLASH_ERASE_TIME;

  buffer[1] = (uint8_t)t;
  buffer[2] = (uint8_t)(t >> 8);
  buffer[3] = 0U;
  return USBD_OK;
}
