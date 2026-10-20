# 首次运转：电压型 FOC 全流程

日期：2026-10-20
固件：`firmware/apps/foc_demo`（2026-10-20 版）
硬件：DengFOC 单路板 + 2804 + AS5600，12 V 电池（实测 11.78 V）
负载：空载，电压上限 `ulim` = 2 V

## 1. 目的

确认板卡、编码器、功率级和电压型 FOC 能完整跑通。

## 2. 预测

- 一个电周期对应机械 360° / 7 = 51.4°；
- 空载维持 5 rad/s 所需 Uq ≈ 5 / 40.8 = 0.12 V（`docs/电机与编码器参数.md` 的一阶模型，不含摩擦）。

## 3. 步骤

```
python tools/dengfoc_cli.py enc s
python tools/dengfoc_cli.py "open 3" "mon 3" stop
python tools/dengfoc_cli.py cal
python tools/dengfoc_cli.py "vel 5" "mon 3" "vel -5" "mon 3" stop
python tools/dengfoc_cli.py "pos 0" "mon 1.5" "pos 90" "mon 1.5" "pos -90" "mon 1.5" "pos 0" "mon 1.5" stop
```

## 4. 结果

| 项目 | 结果 |
|---|---|
| 编码器 | SCL = Pin3（PA2）、SDA = Pin4（PA3）；读一次 119 µs；控制环 2 kHz；0 次读取错误 |
| 开环 3 rad/s | 测得 2.86–2.98 rad/s |
| 校准 | 一个电周期转 51.1°（预测 51.4°），极对数 7.05，往返误差 0.4° |
| 速度 ±5 rad/s | 4.65–5.21 rad/s，所需 Uq ≈ 0.2 V（预测 0.12 V） |
| 位置 0 / 90 / −90 / 0° | 误差 ≤ 0.1°，180° 跳转 < 0.5 s |
| 温度 | MOS 35.0 → 35.6 °C |

## 5. 结论与下一步

- 极对数和电周期角度与模型吻合。
- 维持速度所需电压比模型大约 0.08 V，推测是轴承摩擦和齿槽力矩。下一步做"不同转速下的稳态 Uq"实验，把摩擦模型辨识出来。
- 现在只能通过状态行粗看曲线。要做严肃的阶跃响应和扫频，先完成学习路线阶段 2 的高速数据流。
