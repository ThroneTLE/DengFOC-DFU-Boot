# DengFOC-ODrive 单路板 USB DFU 引导程序

套件总览见 [../../README.md](../../README.md)，板卡引脚与安全规范见 [../../docs/硬件开发手册.md](../../docs/硬件开发手册.md)。

STM32F405RG，8 MHz 晶振。芯片自带的 ROM DFU 在这块板上进不去（ROM 配好时钟后约 25 ms 自行软件复位），所以用这个引导程序代替。

## Flash 分区

| 区域 | 地址 | 说明 |
|---|---|---|
| 引导程序 | 0x08000000–0x08007FFF（扇区 0–1） | USB 无法擦写，只能用 SWD 更新 |
| 用户程序 | 0x08008000–0x080FFFFF（扇区 2–11） | 通过 USB 烧录 |

## 进入 USB 烧录模式

- 用户程序区为空或无效时，上电自动进入。
- **0.5 秒内连按两下 RESET**。
- 用户程序向 `0x2001FFF0` 写入 `0xDF11B007` 后调用 `NVIC_SystemReset()`。

只按一下 RESET 时，约 0.5 秒后启动用户程序。电脑上显示为 `DengFOC DFU Bootloader`（0483:DF11），使用已安装的 STM32 DFU 驱动。

## 烧录用户程序

```
STM32_Programmer_CLI -c port=USB1 -w app.bin 0x08008000 -v -s 0x08008000
```

.hex / .elf 自带地址，可以省略 `0x08008000`。

## 用户程序需要改的地方

- 链接脚本：`FLASH ORIGIN = 0x08008000, LENGTH = 992K`。
- 向量表：引导程序跳转前已设置 `SCB->VTOR = 0x08008000`。如果 `system_stm32f4xx.c` 定义了 `USER_VECT_TAB_ADDRESS`，要把 `VECT_TAB_OFFSET` 设为 `0x8000`。

## 编译与首次烧录（SWD）

```
bash build.sh        # 输出 build/dfu_boot.bin，需要 STM32CubeCLT 的 GCC
```

首次烧录或更新引导程序用 DAPLink + OpenOCD（cmsis-dap.cfg + stm32f4x.cfg），写入 0x08000000。

引导程序只初始化时钟和 PA11/PA12（USB），不碰电机相关引脚。烧录时主电源（XT60）保持断开。
