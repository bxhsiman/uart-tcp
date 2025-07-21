#!/usr/bin/env python3
# tcp_server_batch.py
#
# 优化版TCP服务器，支持批量接收ESP32发送的LiDAR数据
# 协议格式：[4字节长度(网络字节序)][N字节数据] (可能包含多个包)
#
# Usage:
#   python get_data_server_batch.py --port 3333 --outfile dump.hex

import argparse
import socket
import sys
import time
import struct
from pathlib import Path

# 安全导入 SO_REUSEADDR
try:
    from socket import SO_REUSEADDR
except ImportError:
    SO_REUSEADDR = getattr(socket, 'SO_REUSEADDR', 2)

LIDAR_PACKET_SIZE = 44
HEADER_SEQ = (0x0A, 0x00)
SEQUENCE_MASK = 0x0F

class BatchPacketProcessor:
    def __init__(self, out_path: Path):
        self.out_path = out_path
        self.packet_count = 0
        self.lost_count = 0
        self.error_count = 0
        self.expected_seq = 0
        self.first_packet = True
        self.last_log_time = time.time()
        self.total_bytes = 0
        
    def process_batch_stream(self, conn: socket.socket):
        """处理批量数据流"""
        print("开始处理批量数据流...")
        
        # 增大socket接收缓冲区
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
        
        while True:
            try:
                # 读取4字节长度前缀
                length_data = self._recv_exact(conn, 4)
                if not length_data:
                    break
                    
                # 解析长度（网络字节序）
                batch_length = struct.unpack('!I', length_data)[0]
                
                if batch_length == 0 or batch_length > 65536:  # 合理性检查
                    self._log_error(f"无效的批量长度: {batch_length}")
                    self.error_count += 1
                    continue
                
                # 读取批量数据
                batch_data = self._recv_exact(conn, batch_length)
                if not batch_data:
                    break
                
                self.total_bytes += len(batch_data)
                
                # 可能是单个包或多个包的组合
                if batch_length == LIDAR_PACKET_SIZE:
                    # 单个包
                    self._process_lidar_packet(batch_data)
                else:
                    # 可能包含多个包，需要解析
                    self._process_possible_batch(batch_data)
                    
            except Exception as e:
                self._log_error(f"处理批量数据流时出错: {e}")
                break
                
        print(f"批量数据流处理结束. 总计: {self.packet_count}包, {self.lost_count}丢失, {self.error_count}错误")
        print(f"总字节数: {self.total_bytes}")
    
    def _process_possible_batch(self, data: bytes):
        """处理可能包含多个LiDAR包的数据"""
        offset = 0
        
        while offset + LIDAR_PACKET_SIZE <= len(data):
            # 尝试验证这是否是LiDAR包的开始
            if (offset + 1 < len(data) and 
                data[offset] == HEADER_SEQ[0] and 
                data[offset + 1] == HEADER_SEQ[1]):
                
                # 提取可能的包
                packet = data[offset:offset + LIDAR_PACKET_SIZE]
                if len(packet) == LIDAR_PACKET_SIZE:
                    self._process_lidar_packet(packet)
                    offset += LIDAR_PACKET_SIZE
                else:
                    break
            else:
                # 不是有效包头，跳过一个字节继续搜索
                offset += 1
                
        if offset < len(data):
            remaining = len(data) - offset
            if remaining > 0:
                self._log_error(f"批量数据末尾有 {remaining} 字节未处理")
    
    def _recv_exact(self, conn: socket.socket, num_bytes: int) -> bytes:
        """精确接收指定字节数的数据"""
        data = b''
        while len(data) < num_bytes:
            chunk = conn.recv(num_bytes - len(data))
            if not chunk:
                return None  # 连接关闭
            data += chunk
        return data
    
    def _process_lidar_packet(self, packet: bytes):
        """处理单个LiDAR数据包"""
        if len(packet) != LIDAR_PACKET_SIZE:
            self._log_error(f"包长度错误: {len(packet)} != {LIDAR_PACKET_SIZE}")
            self.error_count += 1
            return
            
        # 验证包头
        if packet[0] != HEADER_SEQ[0] or packet[1] != HEADER_SEQ[1]:
            self._log_error(f"包头错误: {packet[0]:02X} {packet[1]:02X}")
            self.error_count += 1
            return
            
        # 提取序号
        seq_byte = packet[2]
        seq_num = seq_byte & SEQUENCE_MASK
        
        # 检查空字节
        if packet[3] != 0x00:
            self._log_error(f"空字节错误: {packet[3]:02X}")
            self.error_count += 1
            
        # 检查序号连续性
        if not self.first_packet:
            if seq_num != self.expected_seq:
                lost = (seq_num - self.expected_seq) % 8
                if lost > 0:
                    self._log_error(f"序号跳跃: 期望 {self.expected_seq}, 收到 {seq_num}, 丢失 {lost} 包")
                    self.lost_count += lost
        else:
            self.first_packet = False
            print(f"收到第一个LiDAR包，序号: {seq_num}")
        
        # 更新期望序号
        self.expected_seq = (seq_num + 1) % 8
        self.packet_count += 1
        
        # 写入文件
        self._write_packet(packet, seq_num)
        
        # 定期打印统计信息
        current_time = time.time()
        if current_time - self.last_log_time > 5.0:
            rate = self.total_bytes / (1024 * 1024) / (current_time - self.last_log_time + 1e-6)
            print(f"统计: 收到包={self.packet_count}, 丢失包={self.lost_count}, 错误={self.error_count}, 速率={rate:.2f}MB/s")
            self.last_log_time = current_time
            self.total_bytes = 0
    
    def _write_packet(self, packet: bytes, seq_num: int):
        """将包写入文件"""
        with self.out_path.open("ab", buffering=0) as f:
            # 写入时间戳和包信息
            timestamp = time.strftime("%H:%M:%S.%f")[:-3]
            f.write(f"\n[{timestamp}] Packet #{self.packet_count} Seq:{seq_num}\n".encode())
            
            # 写入十六进制数据，每16字节一行
            hex_data = packet.hex(' ').upper()
            for i in range(0, len(hex_data), 48):  # 16字节*3字符/字节 = 48字符
                line = hex_data[i:i+48]
                f.write(f"{line}\n".encode())
    
    def _log_error(self, message: str):
        """记录错误信息"""
        timestamp = time.strftime("%H:%M:%S.%f")[:-3]
        error_msg = f"[{timestamp}] ERROR: {message}"
        print(error_msg)
        
        with self.out_path.open("ab", buffering=0) as f:
            f.write(f"\n{error_msg}\n".encode())

def serve_batch_connection(listen_host: str, port: int, outfile: Path):
    """监听并处理批量连接"""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
        srv.setsockopt(socket.SOL_SOCKET, SO_REUSEADDR, 1)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)  # 增大接收缓冲区
        srv.bind((listen_host, port))
        srv.listen(1)
        print(f"监听 {listen_host}:{port} ... (Ctrl-C 退出)")
        print("等待ESP32连接...")

        conn, addr = srv.accept()
        with conn:
            print(f"ESP32客户端连接: {addr}")
            processor = BatchPacketProcessor(outfile)
            
            # 写入会话开始标记
            with outfile.open("ab", buffering=0) as f:
                timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
                f.write(f"\n\n=== 批量处理会话开始 {timestamp} ===\n".encode())
                f.write("# 协议: 可能包含批量的[4字节长度][N字节数据]\n".encode())
            
            processor.process_batch_stream(conn)
            
            # 写入会话结束统计
            with outfile.open("ab", buffering=0) as f:
                timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
                stats = f"\n=== 会话结束 {timestamp} ===\n"
                stats += f"总计: 收到包={processor.packet_count}, 丢失包={processor.lost_count}, 错误={processor.error_count}\n"
                f.write(stats.encode())

def main():
    p = argparse.ArgumentParser(description="TCP server with batch processing for LiDAR data.")
    p.add_argument("--host", default="0.0.0.0", help="Listen address (default 0.0.0.0)")
    p.add_argument("--port", required=True, type=int, help="Listen port")
    p.add_argument("--outfile", default="dump_batch.hex", help="Destination file path")
    args = p.parse_args()

    out_path = Path(args.outfile)
    
    # 在文件开头写入说明
    with out_path.open("w") as f:
        f.write("# 激光雷达数据包分析日志 (批量处理)\n")
        f.write("# 协议格式: 支持单包或批量[4字节长度][N字节数据]\n")
        f.write("# 包格式: 0A 00 [seq] 00 [40字节数据]\n")
        f.write("# 包长度: 44字节\n")
        f.write("# 序号范围: 0-7\n\n")

    try:
        while True:
            serve_batch_connection(args.host, args.port, out_path)
    except KeyboardInterrupt:
        print("\n服务器停止")
    except OSError as e:
        print(f"Socket 错误: {e}", file=sys.stderr)

if __name__ == "__main__":
    main()