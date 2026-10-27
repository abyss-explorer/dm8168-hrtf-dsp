#!/bin/sh
# ====================================================================
# DM8168 HRTF空间音效处理系统启动脚本
# 基于MIT KEMAR HRTF，使用SOFA文件进行空间音效卷积
# ====================================================================

# 1. 文件路径配置
INPUT_FILE="input_audio.pcm"       # 输入: 单声道16bit PCM
OUTPUT_FILE="output_processed.pcm"  # 输出: 双声道16bit PCM
HRIR_BIN="hrir_table.bin"           # 预处理后的HRIR表

# 2. 优雅停机处理函数
cleanup() {
    echo -e "\n[INFO] 捕捉到退出信号，开始清理系统资源..."

    # 杀死主控程序
    [ -n "$APP_PID" ] && kill -9 $APP_PID 2>/dev/null

    # 向DSP发送卸载命令
    echo "[INFO] 正在关闭DSP固件服务..."
    ./slaveloader shutdown DSP 2>/dev/null

    echo "[INFO] 系统安全退出."
    exit 0
}

trap cleanup INT TERM EXIT

# ====================================================================
# 系统启动流水线
# ====================================================================

# 3. 检查输入文件
if [ ! -f "$INPUT_FILE" ]; then
    echo "[ERROR] 输入音频文件不存在: $INPUT_FILE"
    echo "  请将PCM音频文件(单声道, 16bit, 48kHz)放到 $INPUT_FILE"
    exit 1
fi

if [ ! -f "$HRIR_BIN" ]; then
    echo "[ERROR] HRIR表文件不存在: $HRIR_BIN"
    echo "  请先运行: python3 tools/sofa_to_bin.py mit_kemar_normal_pinna.sofa tools/"
    echo "  然后将生成的 hrir_table.bin 复制到 $HRIR_BIN"
    exit 1
fi

# 4. 启动DSP固件服务端
echo "[INFO] 1/3 正在加载DSP固件 (server_dsp.xe674)..."
./slaveloader startup DSP server_dsp.xe674 || { echo "[ERROR] DSP加载失败"; exit 1; }

# 5. 启动ARM主控程序 (后台运行)
echo "[INFO] 2/3 启动App_host (HRTF空间音效处理)..."
./app_host DSP &
APP_PID=$!

# 6. 等待处理完成
echo "[INFO] 3/3 正在处理音频..."
echo "========================================================"
echo " 输入文件: $INPUT_FILE"
echo " 输出文件: $OUTPUT_FILE"
echo " HRIR表:   $HRIR_BIN"
echo " 处理模式: 固定文件输入 -> HRTF卷积 -> 文件输出"
echo " 动态轨迹: 圆周运动 (2圈旋转)"
echo "========================================================"

# 等待app_host完成
wait $APP_PID
APP_EXIT=$?

echo "[INFO] 处理完成，退出码: $APP_EXIT"
echo "[INFO] 输出文件: $OUTPUT_FILE"
echo "  可使用以下命令播放:"
echo "    aplay -f S16_LE -r 48000 -c 2 $OUTPUT_FILE"
