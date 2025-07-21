#!/usr/bin/env python3
"""
雷达数据批次接收器
接收ESP32发送的批次数据 (8个序号0-7的352字节包 = 2816字节)
验证包的完整性和连续性
"""

import socket
import struct
import time
import sys
from typing import Dict, List, Optional

# 配置常量
LIDAR_PACKET_SIZE = 352
LIDAR_BATCH_SIZE = 8
BATCH_SIZE = LIDAR_BATCH_SIZE * LIDAR_PACKET_SIZE  # 2816 bytes
LIDAR_HEADER = [0x0A, 0x00]

class LidarBatchReceiver:
    def __init__(self, host='0.0.0.0', port=3334):
        self.host = host
        self.port = port
        self.socket = None
        
        # 统计信息
        self.total_batches_received = 0
        self.total_packets_received = 0
        self.total_bytes_received = 0
        self.sequence_errors = 0
        self.header_errors = 0
        self.size_errors = 0
        
        # 批次跟踪
        self.last_batch_sequences = None
        self.start_time = time.time()
        
    def start_server(self):
        """启动TCP服务器"""
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        
        try:
            self.socket.bind((self.host, self.port))
            self.socket.listen(1)
            print(f"🚀 雷达批次接收器启动，监听 {self.host}:{self.port}")
            print(f"📦 期望批次大小: {BATCH_SIZE} bytes ({LIDAR_BATCH_SIZE} packets × {LIDAR_PACKET_SIZE} bytes)")
            print("🔍 等待ESP32连接...")
            
            while True:
                client_socket, address = self.socket.accept()
                print(f"✅ ESP32已连接: {address}")
                self.handle_client(client_socket)
                
        except KeyboardInterrupt:
            print("\n⏹️ 用户中断，正在停止...")
        except Exception as e:
            print(f"❌ 服务器错误: {e}")
        finally:
            if self.socket:
                self.socket.close()
            self.print_final_stats()
    
    def handle_client(self, client_socket):
        """处理客户端连接"""
        try:
            buffer = b''
            
            while True:
                # 接收数据
                data = client_socket.recv(4096)
                if not data:
                    print("🔌 ESP32断开连接")
                    break
                
                buffer += data
                self.total_bytes_received += len(data)
                
                # 处理完整的批次
                while len(buffer) >= BATCH_SIZE:
                    batch_data = buffer[:BATCH_SIZE]
                    buffer = buffer[BATCH_SIZE:]
                    
                    self.process_batch(batch_data)
                    
        except Exception as e:
            print(f"❌ 处理客户端时出错: {e}")
        finally:
            client_socket.close()
    
    def process_batch(self, batch_data: bytes):
        """处理单个批次数据"""
        if len(batch_data) != BATCH_SIZE:
            self.size_errors += 1
            print(f"⚠️ 批次大小错误: 期望 {BATCH_SIZE}, 收到 {len(batch_data)}")
            return
        
        self.total_batches_received += 1
        sequences = []
        
        # 解析批次中的8个包
        for i in range(LIDAR_BATCH_SIZE):
            packet_start = i * LIDAR_PACKET_SIZE
            packet_data = batch_data[packet_start:packet_start + LIDAR_PACKET_SIZE]
            
            if self.validate_packet(packet_data, i):
                sequence = packet_data[2] & 0x0F
                sequences.append(sequence)
                self.total_packets_received += 1
        
        # 验证序列连续性
        self.validate_batch_sequences(sequences)
        
        # 定期打印统计信息
        if self.total_batches_received % 100 == 0:  # 每100个批次打印一次
            self.print_stats()
        
        # 详细日志（每10个批次）
        if self.total_batches_received % 10 == 0:
            print(f"📦 批次 #{self.total_batches_received}: 序列 {sequences}")
    
    def validate_packet(self, packet_data: bytes, expected_index: int) -> bool:
        """验证单个数据包"""
        if len(packet_data) < 4:
            return False
        
        # 检查包头
        if packet_data[0] != LIDAR_HEADER[0] or packet_data[1] != LIDAR_HEADER[1]:
            self.header_errors += 1
            print(f"❌ 包 {expected_index}: 包头错误 {packet_data[0]:02X} {packet_data[1]:02X}")
            return False
        
        # 检查序列号
        sequence = packet_data[2] & 0x0F
        if sequence != expected_index:
            self.sequence_errors += 1
            print(f"❌ 包 {expected_index}: 序列号错误，期望 {expected_index}, 收到 {sequence}")
            return False
        
        return True
    
    def validate_batch_sequences(self, sequences: List[int]):
        """验证批次序列连续性"""
        expected = list(range(8))  # [0, 1, 2, 3, 4, 5, 6, 7]
        
        if sequences != expected:
            self.sequence_errors += 1
            missing = set(expected) - set(sequences)
            extra = set(sequences) - set(expected)
            
            if missing:
                print(f"⚠️ 批次 #{self.total_batches_received}: 缺失序列 {sorted(missing)}")
            if extra:
                print(f"⚠️ 批次 #{self.total_batches_received}: 多余序列 {sorted(extra)}")
    
    def print_stats(self):
        """打印当前统计信息"""
        elapsed = time.time() - self.start_time
        batch_rate = self.total_batches_received / elapsed if elapsed > 0 else 0
        packet_rate = self.total_packets_received / elapsed if elapsed > 0 else 0
        data_rate = self.total_bytes_received / (1024 * elapsed) if elapsed > 0 else 0
        
        print(f"""
📊 实时统计 (运行时间: {elapsed:.1f}s)
├─ 批次: {self.total_batches_received} ({batch_rate:.1f} batches/s)
├─ 数据包: {self.total_packets_received} ({packet_rate:.1f} packets/s)
├─ 字节: {self.total_bytes_received} ({data_rate:.1f} KB/s)
├─ 序列错误: {self.sequence_errors}
├─ 包头错误: {self.header_errors}
└─ 大小错误: {self.size_errors}
""")
    
    def print_final_stats(self):
        """打印最终统计信息"""
        elapsed = time.time() - self.start_time
        
        print(f"""
🏁 最终统计报告
{'='*50}
运行时间: {elapsed:.1f} 秒
总批次数: {self.total_batches_received}
总数据包: {self.total_packets_received}
总字节数: {self.total_bytes_received:,}
平均批次率: {self.total_batches_received/elapsed:.2f} batches/s
平均包率: {self.total_packets_received/elapsed:.2f} packets/s
平均数据率: {self.total_bytes_received/(1024*elapsed):.2f} KB/s

❌ 错误统计:
├─ 序列错误: {self.sequence_errors}
├─ 包头错误: {self.header_errors}
└─ 大小错误: {self.size_errors}

✅ 数据完整性: {((self.total_packets_received - self.sequence_errors - self.header_errors) / max(1, self.total_packets_received) * 100):.2f}%
""")

def main():
    if len(sys.argv) >= 2:
        port = int(sys.argv[1])
    else:
        port = 3334
    
    receiver = LidarBatchReceiver(port=port)
    receiver.start_server()

if __name__ == "__main__":
    main()