# DM8168 HRTF DSP — 空间音效处理系统

基于 TI DM8168 DSP 的 HRTF（头部相关传递函数）空间音效处理系统。通过 FFT 快速卷积将单声道 PCM 音频实时转换为带有 HRTF 空间效果的双声道音频。

## 功能

- 读取单声道 16-bit 48kHz PCM 音频（`input_audio.pcm`）
- 使用 SOFA 格式的 HRTF 数据（SADIE KEMAR），预计算 HRIR 二进制表（`hrir_table.bin`）
- DSP 端执行 Overlap-Save 分块频域卷积（FFT_N=1024, HRIR_LEN=256）
- 根据方位角轨迹动态切换 HRIR，模拟声源在水平面移动
- 输出双声道 16-bit 立体声音频

## 架构

```
Host (ARM Linux)                    DSP (C674x @ 1GHz)
┌──────────────┐    SysLink       ┌──────────────────┐
│ App.c        │ ──────────────▶  │ Server.c         │
│ - 读 PCM 文件 │   SharedRegion   │ - FFT 卷积引擎    │
│ - 写输出文件  │ ◀──────────────  │ - HRIR 频域查表   │
│ sofa_loader  │                  │ fft_conv.c        │
└──────────────┘                  └──────────────────┘
```

### 关键参数

| 参数 | 值 | 说明 |
|---|---|---|
| `FFT_N` | 1024 | FFT 点数 |
| `HRIR_LEN` | 256 | HRIR 滤波器长度 |
| `CONV_INPUT_LEN` | 512 | 每帧新输入样本数 |
| `CONV_HISTORY_LEN` | 512 | Overlap-Save 历史样本数 |
| 采样率 | 48000 Hz | — |

## 目录结构

```
├── dsp/               # DSP 端代码
│   ├── fft_conv.c     # FFT 引擎 + Overlap-Save 卷积
│   ├── fft_conv.h     # FFT 卷积接口定义
│   ├── Server.c       # DSP 服务端（消息处理 + 音频处理）
│   └── main_dsp.c     # DSP 主程序入口
├── host/              # Host 端代码
│   ├── App.c          # 主应用（文件 I/O + DSP 通信 + 轨迹）
│   ├── sofa_loader.c  # SOFA 文件加载器
│   └── sofa_loader.h  # 加载器接口
├── shared/            # 共享头文件
│   ├── AppCommon.h    # 公共定义（消息命令、共享缓冲区布局）
│   └── SystemCfg.h    # 系统配置
├── tools/             # 工具
│   ├── fft_test.c     # FFT 回归测试程序（PC 端 GCC 编译）
│   └── sofa_to_bin.py # SOFA → HRIR 二进制表转换脚本
├── azimuth_map.h      # 方位角映射表
├── hrir_table.bin     # 预计算的 HRIR 二进制表
├── input_audio.pcm    # 测试输入音频
├── makefile           # 顶层构建文件
└── run.sh             # 运行脚本
```

## 构建

### Host 端

```bash
make
```

### DSP 端

需要在 TI Code Generation Tools 和 SysLink SDK 环境下编译。

### FFT 回归测试（PC 端）

```bash
cd tools && gcc -o fft_test fft_test.c -lm && ./fft_test
```

测试验证项：
1. FFT/IFFT 往返一致性
2. 单位冲激 HRIR 卷积（恒等滤波器）
3. 真实 HRIR DC 增益校验

## 技术细节

### FFT 快速卷积

使用 Radix-2 DIT FFT 实现 1024 点复数 FFT，通过 Overlap-Save 方法实现分块卷积：

1. 构建 FFT 块：`[history(512) | new_input(512)]` = 1024 样本
2. 前向 FFT → 频域
3. 与预变换的 HRIR 频域数据逐点相乘（左耳、右耳各一次）
4. IFFT → 时域
5. 提取有效输出（后 512 个样本）
6. 更新历史缓冲区

### HRIR 预变换

HRIR 表在 DSP 初始化时一次性做 FFT 变换并存入 `g_hrir_freq_table`，运行时无需重复计算。

## Bug 修复历史

本项目经历了以下关键 bug 修复：

1. **FFT 蝶形运算索引错误**：蝶形组内第 k 个蝶形应操作 `x[i+k]` 而非 `x[i]`，导致 k>0 的蝶形读写错误位置，FFT 输出完全错乱
2. **逆 FFT twiddle factor 共轭计算错误**：共轭应只翻转虚部，原代码同时翻转实部得到 `-W` 而非 `conj(W)`
3. **Overlap-Save 历史长度错误**：`CONV_HISTORY_LEN` 应为 `FFT_N - CONV_INPUT_LEN = 512`，原值为 511 导致帧间不连续

## 运行环境

- 硬件：TI DM8168 EVM（ARM Cortex-A8 + C674x DSP）
- SDK：TI ezsSDK 5.05.01.04 + SysLink 2.20.00.14
- OS：ARM 端 Linux，DSP 端 SYS/BIOS
