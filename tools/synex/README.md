# Synex 上位机配置（DengFOC 电机页、力反馈旋钮页）

本工程自带一份 Synex v4，放在仓库根目录的 `Synex/`。里面的 `config.ini` 已经配好两个页面："DengFOC 电机"和"力反馈旋钮"。

## 每个工程一份 Synex

Synex 是绿色软件：所有设置都存在 `Synex.exe` 旁边的 `config.ini`（另有波形历史 `plot_data.*` 和日志 `SynexLog.txt`），自带 Qt 和 VC++ 运行库，不写注册表，拷到别的 Windows 10/11 电脑上能直接运行。所以每个工程放一份自己的 Synex，配置互不干扰。

- Git 里只有 `Synex/config.ini`；程序本体约 95 MB，是第三方软件，不进仓库（见 `.gitignore`）。
- 新电脑或新工程，用脚本从任意一份 Synex 拷一份干净的过来（不带更新缓存、备份、波形历史、日志），已有的 `config.ini` 会保留：

  ```
  python -X utf8 -I tools/synex/install_synex.py --from "E:/User/Desk/Synex_v3.0.5"
  ```

- 本目录的脚本默认操作仓库里的 `Synex/`，可以用环境变量 `SYNEX_DIR` 指到别处。
- 几份 Synex 同时开着时，"AI 外部助手"的连接信息只有一份（`%TEMP%\Synex\ai_bridge.json`），后打开的那份会覆盖前面的。`build_dengfoc_page.py` 会核对连上的是不是本工程的 Synex，不是就停下，不会改到别的工程。

## 使用

1. 打开 `Synex/Synex.exe`，连接板子的 COM 口（VID:PID 0483:5740），协议 JustFloat。
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
| `build_dengfoc_page.py` | 清空并重建两个页面和命令组（缺的页面会新建），最后逐个读回核对 | 本工程的 Synex 开着，并打开"AI 外部助手" |
| `install_synex.py --from <Synex 文件夹>` | 把一份干净的 Synex 拷进本工程的 `Synex/`，保留已有配置 | 本工程的 Synex 已关闭 |
| `synex_offline_config.py dengfoc` | 写 14 个中文通道名、删除多余旧通道名、清派生通道、采样间隔 0.5 ms、缓冲 40000 点、曲线窗口 0–20 s，并把波形历史文件移开 | 本工程的 Synex 已关闭 |
| `synex_offline_config.py plotwindow` | 只把所有曲线窗口改成 0–20 s（曲线不出图时用） | 本工程的 Synex 已关闭 |
| `synex_plus_mcp.py` | 给 Claude Code 用的 MCP（已在 `.mcp.json` 注册为 `synex-plus`）：转发 Synex 自带的全部工具，再加上离线改设置、读取控件默认配置、批量换波形通道等 | — |
| `synex_mcp.py` | 通用命令行 MCP 客户端，调试用 | — |

```
python -X utf8 build_dengfoc_page.py --dry-run   # 只预览
python -X utf8 build_dengfoc_page.py
python -X utf8 synex_offline_config.py dengfoc
```

## Synex 的坑（4.0.1）

- **没有按钮控件。** 一排按钮用"模式选择"控件做，并打开"同状态重发"；单个命令放在命令面板。
- **实时曲线的横轴是缓存里的位置，不是真实时间。** 范围固定是 0 到 缓存点数 × 采样间隔（40000 × 0.5 ms = 20 s），新数据在这个框里滚动。通过 MCP 新建的曲线会继承一个旧窗口（比如 487–490 s），落在框外，表现为"有数据、数值在变，曲线却是空的，Pts: 0w"。而且通过 MCP 改的曲线窗口退出时不一定被保存，`[PlotSettings]` 里还有一个所有曲线共用的 `globalXRange`，启动时会把曲线拉回旧窗口。所以要在 Synex 关闭时运行 `synex_offline_config.py plotwindow`（`dengfoc` 也会做）：把全局窗口和每个曲线的窗口都改成 0 到缓存时长。它先对配置里所有 `@Variant` 值做解码再编码的往返校验，逐字一致才写入。`build_dengfoc_page.py` 建页面时也会设同样的窗口（`X_SPAN_S`）；改了缓存点数或采样间隔，这两处都要跟着改。4.0 的界面里没有缓存设置窗口，缓存点数只能离线改 config.ini 的 `SerialbufferSize`。
- **通过 MCP 新增控件时，位置和配置都会被忽略。** 要先 `addTile`，再对返回的 id 发 `updateTileGeometry` 和 `updateTileConfig`（字段名是 `patch`，不是 `config`）。
- **通道名会被历史数据覆盖。** 启动时 Synex 会导入 `plot_data.csv`（3.x 是 `plot_data.splot`），通道数和名字都取自里面；退出时又把这些名字写回 config.ini，并重新导出历史文件。所以改通道名必须先关掉 Synex，再把历史文件移开。
- **退出时会重写 config.ini。** 离线修改只能在 Synex 关闭时做，否则会被覆盖。控件的配置存在二进制的 `@Variant` 里，只能通过 MCP 改；位置（`PageN\TileM\col/row/colSpan/rowSpan`）是普通键，可以离线改。
- "AI 外部助手"每次启动 Synex 都要手动重新打开。
