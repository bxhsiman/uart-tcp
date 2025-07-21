#!/bin/bash
# 测试Python服务器脚本

echo "=== TCP服务器测试脚本 ==="

# 测试标准服务器
echo "📡 测试标准TCP服务器..."
timeout 3s python3 get_data_server.py --port 3333 --outfile test_dump.hex &
SERVER1_PID=$!
sleep 1

if kill -0 $SERVER1_PID 2>/dev/null; then
    echo "✅ 标准服务器启动成功 (PID: $SERVER1_PID)"
    kill $SERVER1_PID 2>/dev/null
else
    echo "❌ 标准服务器启动失败"
fi

# 测试带长度前缀的服务器
echo "📡 测试带长度前缀的TCP服务器..."
timeout 3s python3 get_data_server_framed.py --port 3334 --outfile test_dump_framed.hex &
SERVER2_PID=$!
sleep 1

if kill -0 $SERVER2_PID 2>/dev/null; then
    echo "✅ 长度前缀服务器启动成功 (PID: $SERVER2_PID)"
    kill $SERVER2_PID 2>/dev/null
else
    echo "❌ 长度前缀服务器启动失败"
fi

# 清理测试文件
rm -f test_dump.hex test_dump_framed.hex

echo ""
echo "🎉 服务器测试完成！"
echo ""
echo "使用方法："
echo "1. 标准服务器（原协议）:"
echo "   python3 get_data_server.py --port 3333 --outfile lidar_data.hex"
echo ""
echo "2. 长度前缀服务器（推荐，用于重构后的ESP32固件）:"
echo "   python3 get_data_server_framed.py --port 3334 --outfile lidar_framed.hex"