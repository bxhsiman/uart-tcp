#!/bin/bash
# ESP32 Debug Build Script

echo "🔍 Debug Build Check"
echo "===================="

# Check if in correct directory
if [[ ! -f "main/main.c" ]]; then
    echo "❌ Not in ESP32 project directory"
    exit 1
fi

echo "📁 Project structure:"
echo "├─ main/lidar_packet.c: $(wc -l main/lidar_packet.c | cut -d' ' -f1) lines"
echo "├─ main/lidar_packet.h: $(wc -l main/lidar_packet.h | cut -d' ' -f1) lines"
echo "├─ main/config.h: $(wc -l main/config.h | cut -d' ' -f1) lines"
echo "└─ main/main.c: $(wc -l main/main.c | cut -d' ' -f1) lines"

echo ""
echo "🔧 Key configuration values:"
grep -n "LIDAR_PACKET_SIZE" main/config.h
grep -n "LIDAR_BATCH_SIZE" main/config.h
grep -n "TCP_SEND_QUEUE_SIZE" main/lidar_packet.c

echo ""
echo "🚀 Recent changes in lidar_packet.c:"
echo "├─ lidar_cache_packet() function added"
echo "├─ lidar_check_and_send_batch() function added" 
echo "├─ tcp_sender_task() updated for batch processing"
echo "└─ Safety checks added for queue initialization"

echo ""
echo "⚠️ Known issue fixed:"
echo "├─ Added NULL check for tcp_send_queue in tcp_sender_task"
echo "├─ Added initialization wait loop in tcp_sender_task"
echo "└─ Added safety check in lidar_start_tcp_sender_task"

echo ""
echo "📝 To test this build:"
echo "1. idf.py build"
echo "2. idf.py flash monitor"
echo "3. python3 get_data_batch_receiver.py 3334"