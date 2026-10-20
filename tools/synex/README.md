# Synex 上位机配置（DengFOC 电机页、力反馈旋钮页）

Synex v4（`E:\User\Desk\Synex_v3.0.5`）里已经配好两个页面："DengFOC 电机"和"力反馈旋钮"。日常使用不需要运行这里的脚本；它们用来在 Synex 重装、配置被重置时重新生成页面。

## 使用

1. 打开 Synex，连接板子的 COM 口（VID:PID 0483:5740），协议 JustFloat。
2. 点"遥测数据流"开关开始/停止推送波形。
3. 用按钮和滑条控制电机。每次上电先点"校准"。

## "DengFOC 电机"页

| 区域 | 控件 | 发送 / 显示 |
|---|---|---|
| 开关 | 遥测数据流 | `scope 1` / `scope 0` |
| 按钮条 | 停止、校准、清故障、归零 | `act 0` … `act 3`（重复点击也会发送） |
| 状态文字 | 运行模式、故障、校准 | 通道 8、9、11，显示成"速度""编码器""已校准"这类文字 |
| 滑条 | 速度目标、位置目标、力矩、开环转速、电压上限 | `vel`、`pos`、`tq`、`open`、`ulim` |
| 滑条 | 速度环 P / I、位置环 P、速度上限 | `vp`、`vi`、`ap`、`vlim` |
| 仪表 | 转速、母线电压、MOS 温度 | 通道 2、6、10 |
| 数值 | 位置、Uq、目标、中断耗时 | 通道 1、4、0、7 |
| 波形 | 位置（目标 / 实际）、速度（实际 / 参考）、Uq、电角度 | 通道 0+1、2+3、4、5 |

## "力反馈旋钮"页

| 区域 | 控件 | 发送 / 显示 |
|---|---|---|
| 顶部 | 遥测数据流、停止/校准/清故障/归零、当前模式、档位、故障 | 同上；通道 13（模式名）、12、9 |
| 五排按钮 | 档位·菜单、数值·限位、开关·弹簧、阻尼·惯性、纹理·特效，共 41 个 | `knob <n>`，编号和名称见 `build_dengfoc_page.py` 的 `KNOB` 表，手感说明见[实验记录](../../experiments/2026-10-20_力反馈旋钮/README.md#32-41-个预设) |
| 滑条 | 手感力度、电压上限 | `kstr`、`ulim` |
| 数值 | 转速、Uq、飞轮转速 | 通道 2、4、3 |
| 波形 | 位置（格中心 / 实际）、速度（实际 / 虚拟飞轮）、Uq | 通道 0+1、2+3、4 |

命令面板最上面另有一组"DengFOC 电机"：校准、停止、清除故障、位置归零、遥测开启、遥测停止、进入烧录模式。

物理通道名（config.ini 的 `[ChannelNames]`）：0 目标、1 位置_deg、2 速度_rad_s、3 速度参考、4 Uq_V、5 电角度_rad、6 母线_V、7 中断_us、8 模式、9 故障、10 MOS温度_C、11 已校准、12 旋钮档位、13 旋钮模式。各通道含义见 [foc_demo README](../../firmware/apps/foc_demo/README.md#实时波形)。

## 脚本

| 文件 | 作用 | 前提 |
|---|---|---|
| `build_dengfoc_page.py` | 清空并重建两个页面和命令组（缺的页面会新建），最后逐个读回核对 | Synex 开着，并打开"AI 外部助手" |
| `synex_offline_config.py dengfoc` | 写 14 个中文通道名、删除多余旧通道名、清派生通道、采样间隔 0.5 ms、缓冲 40000 点，并把波形历史文件移开 | Synex 已关闭 |
| `synex_plus_mcp.py` | 给 Claude Code 用的 MCP（已在 `.mcp.json` 注册为 `synex-plus`）：转发 Synex 自带的全部工具，再加上离线改设置、读取控件默认配置、批量换波形通道等 | — |
| `synex_mcp.py` | 通用命令行 MCP 客户端，调试用 | — |

```
python -X utf8 build_dengfoc_page.py --dry-run   # 只预览
python -X utf8 build_dengfoc_page.py
python -X utf8 synex_offline_config.py dengfoc
```

## Synex 的坑（4.0.1）

- **没有按钮控件。** 一排按钮用"模式选择"控件做，并打开"同状态重发"；单个命令放在命令面板。
- **通过 MCP 新增控件时，位置和配置都会被忽略。** 要先 `addTile`，再对返回的 id 发 `updateTileGeometry` 和 `updateTileConfig`（字段名是 `patch`，不是 `config`）。
- **通道名会被历史数据覆盖。** 启动时 Synex 会导入 `plot_data.csv`（3.x 是 `plot_data.splot`），通道数和名字都取自里面；退出时又把这些名字写回 config.ini，并重新导出历史文件。所以改通道名必须先关掉 Synex，再把历史文件移开。
- **退出时会重写 config.ini。** 离线修改只能在 Synex 关闭时做，否则会被覆盖。控件的配置存在二进制的 `@Variant` 里，只能通过 MCP 改；位置（`PageN\TileM\col/row/colSpan/rowSpan`）是普通键，可以离线改。
- "AI 外部助手"每次启动 Synex 都要手动重新打开。
