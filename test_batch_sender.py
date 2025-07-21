#!/usr/bin/env python3
"""
雷达数据批次发送测试器
模拟ESP32发送批次数据以测试接收器
"""

import socket
import struct
import time
import random
import sys

# 配置常量
LIDAR_PACKET_SIZE = 352
LIDAR_BATCH_SIZE = 8
BATCH_SIZE = LIDAR_BATCH_SIZE * LIDAR_PACKET_SIZE
LIDAR_HEADER = [0x0A, 0x00]

def create_test_packet(sequence: int) -> bytes:
    """创建测试数据包"""
    packet = bytearray(LIDAR_PACKET_SIZE)
    
    # 设置包头
    packet[0] = LIDAR_HEADER[0]  # 0x0A
    packet[1] = LIDAR_HEADER[1]  # 0x00
    packet[2] = sequence & 0x0F  # 序列号
    packet[3] = 0x00             # 空字节
    
    # 填充模拟的LiDAR数据
    for i in range(4, LIDAR_PACKET_SIZE):
        packet[i] = (sequence * 100 + i) & 0xFF
    
    return bytes(packet)

def create_test_batch() -> bytes:
    """创建完整的测试批次 (8个包)"""
    batch_data = bytearray()
    
    for seq in range(LIDAR_BATCH_SIZE):
        packet = create_test_packet(seq)
        batch_data.extend(packet)
    
    return bytes(batch_data)

def create_incomplete_batch() -> bytes:
    """创建不完整的测试批次 (缺少某些序列号)"""
    batch_data = bytearray()
    missing_sequences = random.sample(range(8), random.randint(1, 3))
    
    for seq in range(LIDAR_BATCH_SIZE):
        if seq not in missing_sequences:
            packet = create_test_packet(seq)
            batch_data.extend(packet)
        else:
            # 用错误的序列号填充
            packet = create_test_packet((seq + 10) % 16)
            batch_data.extend(packet)
    
    print(f"🧪 发送不完整批次，缺失序列: {missing_sequences}")
    return bytes(batch_data)

def main():
    if len(sys.argv) >= 3:
        host = sys.argv[1]
        port = int(sys.argv[2])
    else:
        host = "192.168.114.117"  # 默认服务器地址
        port = 3334
    
    print(f"🧪 雷达批次测试发送器")
    print(f"🎯 目标: {host}:{port}")
    print(f"📦 批次大小: {BATCH_SIZE} bytes")
    
    try:
        # 连接到服务器
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
        print(f"✅ 已连接到服务器")
        
        batch_count = 0
        
        while True:
            batch_count += 1
            
            # 90%的概率发送完整批次，10%发送不完整批次
            if random.random() < 0.9:
                batch_data = create_test_batch()
                print(f"📤 发送完整批次 #{batch_count}")
            else:
                batch_data = create_incomplete_batch()
                print(f"📤 发送不完整批次 #{batch_count}")
            
            # 发送批次数据
            sock.sendall(batch_data)
            
            # 每50个批次打印统计
            if batch_count % 50 == 0:
                print(f"📊 已发送 {batch_count} 个批次")
            
            # 控制发送速率
            time.sleep(0.01)  # 100批次/秒
            
    except KeyboardInterrupt:
        print(f"\n⏹️ 用户中断，已发送 {batch_count} 个批次")
    except Exception as e:
        print(f"❌ 发送错误: {e}")
    finally:
        sock.close()

if __name__ == "__main__":
    main()