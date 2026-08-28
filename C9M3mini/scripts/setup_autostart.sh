#!/bin/bash
# PiCameraApp Autostart Setup Script
# 设置变量
APP_NAME="PiCameraApp"
PROJECT_DIR="$HOME/.vs/C9M3mini"
# 自动探测可执行文件路径
POSSIBLE_BIN_PATHS=(
    "$PROJECT_DIR/out/build/linux-debug/$APP_NAME"
    "$PROJECT_DIR/build/$APP_NAME"
    "$PROJECT_DIR/bin/$APP_NAME"
)

EXEC_PATH=""
for path in "${POSSIBLE_BIN_PATHS[@]}"; do
    if [ -f "$path" ]; then
        EXEC_PATH="$path"
        break
    fi
done

if [ -z "$EXEC_PATH" ]; then
    echo "错误: 未找到可执行文件 $APP_NAME。"
    echo "请确认项目已编译，或手动修改本脚本中的 EXEC_PATH。"
    exit 1
fi

WORKING_DIR=$(dirname "$EXEC_PATH")
AUTOSTART_DIR="$HOME/.config/autostart"
DESKTOP_FILE="$AUTOSTART_DIR/picamera.desktop"

echo "正在配置自启动..."
echo "程序位置: $EXEC_PATH"
echo "工作目录: $WORKING_DIR"

# 创建自启动目录
mkdir -p "$AUTOSTART_DIR"

# 创建 .desktop 文件
cat <<EOF > "$DESKTOP_FILE"
[Desktop Entry]
Type=Application
Name=Pi Camera App
Comment=Start Camera System on Boot
Exec=$EXEC_PATH
WorkingDirectory=$WORKING_DIR
Terminal=false
X-GNOME-Autostart-enabled=true
EOF

# 赋予执行权限
chmod +x "$DESKTOP_FILE"

echo "配置完成"
echo "提示: 下次重启树莓派进入桌面后，相机程序将自动全屏启动。"
echo "如果需要取消自启动，只需删除文件: $DESKTOP_FILE"
