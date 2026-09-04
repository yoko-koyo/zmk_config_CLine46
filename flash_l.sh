#!/bin/bash

# --- 設定 ---
TARGET="cline46_l"
UF2_PATH="./build/${TARGET}/zephyr/zmk.uf2"
# XIAO BLE をダブルリセットした時にマウントされるドライブ名
DRIVE_NAME="${DRIVE_NAME:-XIAO-SENSE}"

echo "========================================"
echo "👈 左手側 (${TARGET}) のビルドを開始します..."
echo "========================================"

# Kconfig の警告でビルドを止めない
export CMAKE_ARGS="-DCONFIG_ZMK_KCONFIG_WARNINGS_AS_ERRORS=n"

INCREMENTAL=true ./build_local.sh build "$TARGET"

if [ $? -ne 0 ]; then
    echo "❌ ビルド失敗"
    exit 1
fi

echo "✅ ビルド成功！リセットボタンをダブルクリックしてください..."

while [ ! -d "/Volumes/$DRIVE_NAME" ]; do
    sleep 1
done

echo "⚡️ 書き込み中..."
# -X は macOS の拡張属性（._ ファイル）をコピーしないためのフラグ
cp -X "$UF2_PATH" "/Volumes/$DRIVE_NAME/" 2>/dev/null || true

sleep 2
echo "🎉 完了しました！"
