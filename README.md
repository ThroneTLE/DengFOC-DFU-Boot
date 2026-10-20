# DengFOC 单路板 FOC 学习套件

在灯哥 DengFOC-ODrive 单路驱动板（STM32F405RG + DRV8303）上从零写的一套固件和工具：USB 烧录引导程序、电压型 FOC、41 种力反馈旋钮手感、2 kHz 实时波形，外加仿真和系统辨识脚本。电机是 2804 云台电机，编码器是 AS5600。

这个仓库也是一个学习记录：怎样学 FOC、怎样把《自动控制原理》用在实物上（先测对象、再按公式定参数、最后用阶跃和扫频验证）。路线见 [docs/学习路线.md](docs/学习路线.md)，每个实验的指标、计算和实测数据放在 [experiments/](experiments/)。

## 功能

- **只用 USB 烧录**：板子上只有 RESET 键，芯片自带的 ROM DFU 在这块板上进不去，所以写了自己的 DFU 引导程序（32 KB）。第一次用 SWD 烧一次引导程序，之后连按两下 RESET，或在命令行发 `dfu`，就能通过 USB 烧录。
- **电压型 FOC**：TIM1 中心对齐 24 kHz PWM（6 路、死区 119 ns），控制中断 2 kHz，AS5600 位操作 I²C（读一次约 120 µs）。有开环、校准（自动识别方向和极对数）、力矩、速度、位置几种模式。
- **41 种力反馈旋钮**：棘轮、限位、开关、回中弹簧、磁吸点、阻尼、摩擦、纹理、单向棘轮；虚拟飞轮做出的 iOS 拨轮惯性手感；仿罗技 MagSpeed 的速度自适应棘轮。手感来源和实测见[实验记录](experiments/2026-10-20_力反馈旋钮/README.md)。
- **实时波形**：14 个通道以 2 kHz 推送 VOFA+ JustFloat 格式，实测丢帧 0。可用 VOFA+、Synex 查看，或用脚本存成 CSV。
- **保护**：上电时所有栅极输入保持低电平；母线电压超出范围、DRV8303 过流、编码器失效、过温都会立即停机。
- **工具**：PC 上的旋钮仿真器（把固件的 `knob.c` 原样编译，接电机和编码器模型），电机摩擦和反电动势辨识脚本，USB 命令行。

## 硬件

| 部件 | 型号 / 说明 |
|---|---|
| 驱动板 | DengFOC-ODrive 单路板：STM32F405RGT6（8 MHz 晶振）、DRV8303、板载 AS5047P（本项目没用） |
| 电机 | 2804 云台电机，7 对极，相电阻 2.55 Ω，额定 0.5 A |
| 编码器 | AS5600 模块（I²C），径向充磁磁铁装在电机轴上 |
| 电源 | 12 V 电池或稳压电源，接 XT30，建议串 3–5 A 保险丝 |
| 首次烧录 | 任意 CMSIS-DAP / DAPLink 调试器（只在烧引导程序时用一次） |

### 接线

| 连接 | 说明 |
|---|---|
| USB Type-C | 逻辑供电、烧录、命令行、波形 |
| AS5600 | VCC → 2×7 排针"3"（3.3 V），GND → "G"，SCL → Pin3（PA2），SDA → Pin4（PA3）。接 Pin1–Pin7 任意两根也行，上电会自动扫描 |
| 电机三相 | MR30 电机口，相序任意，校准时自动识别 |
| SWD（仅首次） | 排针 DIO / CLK / G；调试器的 VCC 不要接 |

引脚和外设的完整对应（含实测结果）见 [docs/硬件开发手册.md](docs/硬件开发手册.md)。

## 快速开始

### 1. 准备工具

- [STM32CubeCLT](https://www.st.com/en/development-tools/stm32cubeclt.html)：提供 `arm-none-eabi-gcc` 和 `STM32_Programmer_CLI`。`build.sh` 默认在 `E:/ST/STM32CubeCLT_1.18.0` 下找编译器，可以用环境变量 `GCC_BIN` 改。
- Python 3 + `pyserial`。仿真器还需要 `numpy`、`matplotlib` 和 PC 上的 `gcc`（如 MinGW-w64）。
- 首次烧录用 [xPack OpenOCD](https://xpack-dev-tools.github.io/openocd-xpack/)。
- Windows 下 USB DFU 用 STM32CubeProgrammer 自带的 DFU 驱动。

### 2. 首次烧录引导程序（SWD，只做一次）

**会擦掉板子上原厂的 ODrive 固件。** 烧录时断开 XT30 主电源。

```
cd firmware/bootloader && bash build.sh
openocd -f interface/cmsis-dap.cfg -f target/stm32f4x.cfg \
        -c "program build/dfu_boot.bin 0x08000000 verify reset exit"
```

之后电脑上能看到 `DengFOC DFU Bootloader`（0483:DF11）。

### 3. 编译、烧录 FOC 程序（USB）

```
cd firmware/apps/foc_demo && bash build.sh
# 进入 USB 烧录模式：0.5 秒内连按两下 RESET，或在程序运行时发 `dfu`
STM32_Programmer_CLI -c port=USB1 -w build/foc_demo.bin 0x08008000 -v -s 0x08008000
```

程序起来后是一个 USB 虚拟串口（0483:5740）。

### 4. 让电机转起来

接好编码器、电机和 12 V 电源后：

```
python tools/dengfoc_cli.py enc            # 编码器在哪两根针、读一次多久、磁铁状态
python tools/dengfoc_cli.py cal            # 校准：方向、极对数、电角度零位（每次上电做一次）
python tools/dengfoc_cli.py "vel 5" "mon 3" stop
python tools/dengfoc_cli.py "pos 90" "mon 2" stop
python tools/dengfoc_cli.py "knob 9"       # 惯性棘轮；`knob` 不带参数列出全部 41 种
```

全部命令见 [foc_demo README](firmware/apps/foc_demo/README.md#命令)。

## 力反馈旋钮

`knob <n>` 选一种手感，`kstr <x>` 调整体力度，`knob 0` 停止。每种手感都是同一个参数化引擎（[knob.c](firmware/apps/foc_demo/Core/Src/knob.c)）的一组参数：

| 类别 | 预设 |
|---|---|
| 档位·菜单 | 细棘轮、粗棘轮、8 档吸附、每圈 16 档、四向菜单、菜单、缺口档、咔嗒、Surface Dial、色环 |
| 数值·限位 | 限位 0–10、多圈限位、精调 0–255、粗调强档 / 弱档、调光 0–100、空调 0–14、窗帘 0–20、磁吸点、90° 窗口 |
| 开关·弹簧 | 开关、三档开关、回中、回中带档、分段弹簧、阻尼弹簧、单向棘轮、阻尼限位、恒力回位 |
| 阻尼·惯性 | 自由、阻尼、摩擦、飞轮、惯性棘轮（iOS 拨轮）、惯性列表、速度自适应 |
| 纹理·特效 | 纹理、方波纹理、棘爪纹理、震动、自转 |

几个设计要点（详见[实验记录](experiments/2026-10-20_力反馈旋钮/README.md)）：

- **虚拟飞轮**：固件里模拟一个有惯量和摩擦的轮子，档位作用在轮子上，轴通过虚拟弹簧阻尼带着它。拨一下就会"得得得"一格一格转下去，越转越慢，最后落进一格。
- **对象补偿**：电压模式下 Uq = 0 等于把电机短路，反电动势会刹车，所以"自由"和惯性类要补偿。补偿量来自实测（`tools/ident_friction.py`）。反电动势部分补 80%，摩擦只补实测最小值的一半，因为摩擦每次上电可能差 40%。模型有误差也不会变成正反馈。
- **静音**：AS5600 最低位来回跳，乘以弹簧刚度会变成力矩噪声，电机会发出"沙沙"声。用 2 码回差、静止回正和 400 Hz 低通去掉后，静止时力矩台阶从每秒 220–840 次降到 0–6 次。
- **先仿真再上板**：`python tools/knob_sim.py all` 对每个预设跑静止、慢拨、拨一下松手、损耗再低 15%，检查会不会嗡嗡响或自己转起来。

## 实时波形

```
python tools/scope_capture.py --seconds 4 --cmd "vel 5" --cmd "@2 vel -5" --cmd "@3.5 stop" --out run.csv
```

- 通道：目标、位置、速度、速度参考、Uq、电角度、母线电压、中断耗时、模式、故障、MOS 温度、已校准、旋钮档位、旋钮模式。通道表见 [foc_demo README](firmware/apps/foc_demo/README.md#实时波形)。
- **VOFA+**：协议选 JustFloat，14 个通道，发 `scope 1` 开始推送。
- **Synex**：[tools/synex](tools/synex/README.md) 能生成"电机"和"力反馈旋钮"两个按钮页。

## 目录

```
├── docs/
│   ├── 硬件开发手册.md           引脚对应、外设分配、换算公式、安全规范
│   ├── 电机与编码器参数.md       2804 + AS5600 参数、一阶模型
│   ├── 学习路线.md               分阶段的学习与实验计划
│   └── reference/               厂家资料（不在仓库里，见其中的 README）
├── firmware/
│   ├── bootloader/              USB DFU 引导程序（0x08000000，32 KB）
│   ├── apps/
│   │   ├── common/              各应用共用的 USB 虚拟串口
│   │   ├── selftest/            板级自检
│   │   └── foc_demo/            FOC + 力反馈旋钮（0x08008000）
│   └── lib/                     CMSIS、STM32F4 HAL、ST USB Device 库、启动文件
├── experiments/                 实验记录：指标 → 对象 → 设计 → 验证，含原始数据
└── tools/
    ├── dengfoc_cli.py           USB 命令行
    ├── scope_capture.py         波形流存 CSV，可按时间发命令
    ├── ident_friction.py        速度扫描，测摩擦和反电动势
    ├── knob_sim.py              旋钮仿真（PC 上编译 knob.c）
    ├── synex/                   Synex 上位机页面生成
    └── debug/                   OpenOCD 调试脚本
```

## 安全

- 程序一启动就把 EN_GATE、刹车管和六路栅极输入全部拉低，只有运动命令才启用功率级。
- 母线电压不在 9–16.8 V 时不启动；DRV8303 设为过流锁存关断；编码器失效、MOS 过温（80 °C）都会停机。
- `ulim` 默认 2 V，最大 5 V（约 2 A，即这个电机的最大电流）。长时间顶着端点或弹簧时不要用大电压上限，电机会发热。
- 改了功率级相关代码后，第一次上电先调低 `ulim`，手不要碰电机。
- 引导程序只能用 SWD 更新，日常不要动它。

## 新建一个应用

复制 `firmware/apps/foc_demo`，改 `build.sh` 里的输出名。链接地址保持 0x08008000，向量表偏移 0x8000；可以复用 `apps/common` 的 USB 命令行；`main()` 里第一件事调用 `power_stage_safe()`。

## 致谢与第三方代码

- 驱动板：灯哥 DengFOC。板子按 ODrive v3.5 的 M0 通道设计，引脚对应参考了 ODrive 固件 v0.5.1。
- 力反馈手感参考了以下项目的思路，代码都是重写的，没有复制：[SmartKnob](https://github.com/scottbez1/smartknob)、[稚晖君 HelloWord 瀚文键盘](https://github.com/peng-zhihui/HelloWord-Keyboard)、[SeedLabs smartknob-firmware](https://github.com/SeedLabs-it/smartknob-firmware)、[X-Knob](https://github.com/SmallPond/X-Knob)、[Super Dial](https://gitee.com/coll45/super-dial-motor-knob-screen)、[Wang-Nainai/SmartKnob](https://github.com/Wang-Nainai/SmartKnob)、[DengFOC 力反馈旋钮课程](https://github.com/ToanTech/DengFOC_FFBKnob)、[simonhtet10/Haptic-Knob](https://github.com/simonhtet10/Haptic-Knob)、Jesse Schoch 在 [SimpleFOC 社区](https://community.simplefoc.com/t/haptic-textures/301) 的触感纹理。逐项说明见[实验记录](experiments/2026-10-20_力反馈旋钮/README.md#0-参考的开源项目调研结论)。
- `firmware/lib` 里的第三方代码沿用各自的许可：CMSIS（Apache-2.0）、STM32F4xx HAL（BSD-3-Clause）、STM32 USB Device Library（ST SLA0044），许可文件放在各自目录下。

## 许可证

本仓库自有代码的许可证待定。
