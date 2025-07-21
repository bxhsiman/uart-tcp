#!/usr/bin/env python3
# Enhanced TCP server for LiDAR data validation and statistics
#
# Usage:
#   python get_data_server.py --host 0.0.0.0 --port 3334 --outfile dump.hex
#
# Features:
# - Validates LiDAR packet format (44 bytes, 0A 00 header, sequence 0-7)
# - Validates frame structure (8 continuous packets)
# - Real-time statistics (frame rate, error rate, throughput)
# - Detailed logging of invalid packets/frames

import argparse
import socket
import sys
import time
import threading
from pathlib import Path
from dataclasses import dataclass
from typing import List, Optional

# LiDAR Protocol Constants
LIDAR_PACKET_SIZE = 44
LIDAR_FRAME_PACKETS = 8
LIDAR_FRAME_SIZE = LIDAR_PACKET_SIZE * LIDAR_FRAME_PACKETS  # 352 bytes
HEADER_BYTE_0 = 0x0A
HEADER_BYTE_1 = 0x00

@dataclass
class LidarPacket:
    header: bytes  # 2 bytes: 0A 00
    sequence: int  # 1 byte: 0-7
    reserved: int  # 1 byte: 00
    data: bytes    # 40 bytes

@dataclass
class Statistics:
    start_time: float
    total_bytes: int = 0
    total_packets: int = 0
    valid_packets: int = 0
    invalid_packets: int = 0
    total_frames: int = 0
    valid_frames: int = 0
    invalid_frames: int = 0
    last_report_time: float = 0
    
    def get_duration(self) -> float:
        return time.time() - self.start_time
    
    def get_throughput_bps(self) -> float:
        duration = self.get_duration()
        return self.total_bytes / duration if duration > 0 else 0
    
    def get_frame_rate(self) -> float:
        duration = self.get_duration()
        return self.valid_frames / duration if duration > 0 else 0
    
    def get_packet_error_rate(self) -> float:
        return (self.invalid_packets / self.total_packets * 100) if self.total_packets > 0 else 0
    
    def get_frame_error_rate(self) -> float:
        return (self.invalid_frames / self.total_frames * 100) if self.total_frames > 0 else 0

class LidarDataProcessor:
    def __init__(self, outfile: Path):
        self.outfile = outfile
        self.stats = Statistics(start_time=time.time())
        self.packet_buffer = bytearray()
        self.current_frame_packets: List[LidarPacket] = []
        self.expected_sequence = 0
        
        # Start statistics reporting thread
        self.stats_thread = threading.Thread(target=self._stats_reporter, daemon=True)
        self.stats_thread.start()
    
    def validate_packet(self, data: bytes) -> Optional[LidarPacket]:
        """Validate a single LiDAR packet"""
        if len(data) != LIDAR_PACKET_SIZE:
            return None
            
        if data[0] != HEADER_BYTE_0 or data[1] != HEADER_BYTE_1:
            return None
            
        sequence = data[2]
        if sequence > 7:
            return None
            
        reserved = data[3]
        if reserved != 0x00:
            return None
            
        return LidarPacket(
            header=data[0:2],
            sequence=sequence,
            reserved=reserved,
            data=data[4:44]
        )
    
    def validate_frame(self, packets: List[LidarPacket]) -> bool:
        """Validate a complete frame (8 packets with sequence 0-7)"""
        if len(packets) != LIDAR_FRAME_PACKETS:
            return False
            
        for i, packet in enumerate(packets):
            if packet.sequence != i:
                return False
                
        return True
    
    def process_data(self, data: bytes):
        """Process incoming data stream"""
        self.stats.total_bytes += len(data)
        self.packet_buffer.extend(data)
        
        # Process complete packets
        while len(self.packet_buffer) >= LIDAR_PACKET_SIZE:
            packet_data = bytes(self.packet_buffer[:LIDAR_PACKET_SIZE])
            self.packet_buffer = self.packet_buffer[LIDAR_PACKET_SIZE:]
            
            self.stats.total_packets += 1
            packet = self.validate_packet(packet_data)
            
            if packet is None:
                self.stats.invalid_packets += 1
                print(f"❌ Invalid packet detected at byte {self.stats.total_bytes - len(self.packet_buffer)}")
                self._log_hex_data(packet_data, "INVALID_PACKET")
                continue
                
            self.stats.valid_packets += 1
            
            # Handle frame assembly
            if packet.sequence == 0:
                # Start of new frame
                if self.current_frame_packets:
                    # Previous frame was incomplete
                    self._handle_incomplete_frame()
                self.current_frame_packets = [packet]
                self.expected_sequence = 1
            elif packet.sequence == self.expected_sequence and self.current_frame_packets:
                self.current_frame_packets.append(packet)
                self.expected_sequence += 1
                
                if len(self.current_frame_packets) == LIDAR_FRAME_PACKETS:
                    # Complete frame received
                    self._handle_complete_frame()
            else:
                # Sequence error
                print(f"❌ Sequence error: expected {self.expected_sequence}, got {packet.sequence}")
                self._handle_sequence_error(packet)
    
    def _handle_complete_frame(self):
        """Handle a complete frame"""
        self.stats.total_frames += 1
        
        if self.validate_frame(self.current_frame_packets):
            self.stats.valid_frames += 1
            print(f"✅ Valid frame #{self.stats.valid_frames}")
            self._log_frame_data(self.current_frame_packets, "VALID_FRAME")
        else:
            self.stats.invalid_frames += 1
            print(f"❌ Invalid frame structure")
            self._log_frame_data(self.current_frame_packets, "INVALID_FRAME")
        
        self.current_frame_packets = []
        self.expected_sequence = 0
    
    def _handle_incomplete_frame(self):
        """Handle incomplete frame when new frame starts"""
        self.stats.invalid_frames += 1
        print(f"❌ Incomplete frame: only {len(self.current_frame_packets)} packets")
        self._log_frame_data(self.current_frame_packets, "INCOMPLETE_FRAME")
        
    def _handle_sequence_error(self, packet: LidarPacket):
        """Handle sequence error"""
        self.stats.invalid_frames += 1
        if self.current_frame_packets:
            self._log_frame_data(self.current_frame_packets, "SEQUENCE_ERROR")
        self.current_frame_packets = []
        
        if packet.sequence == 0:
            self.current_frame_packets = [packet]
            self.expected_sequence = 1
        else:
            self.expected_sequence = 0
    
    def _log_hex_data(self, data: bytes, label: str):
        """Log hex data to file"""
        with self.outfile.open("a") as f:
            f.write(f"\n=== {label} ===\n")
            hex_str = " ".join(f"{b:02X}" for b in data)
            f.write(hex_str + "\n")
    
    def _log_frame_data(self, packets: List[LidarPacket], label: str):
        """Log frame data to file"""
        with self.outfile.open("a") as f:
            f.write(f"\n=== {label} ===\n")
            for i, packet in enumerate(packets):
                full_packet = packet.header + bytes([packet.sequence, packet.reserved]) + packet.data
                hex_str = " ".join(f"{b:02X}" for b in full_packet)
                f.write(f"Packet {i}: {hex_str}\n")
    
    def _stats_reporter(self):
        """Background thread for periodic statistics reporting"""
        while True:
            time.sleep(5)  # Report every 5 seconds
            self._print_statistics()
    
    def _print_statistics(self):
        """Print current statistics"""
        duration = self.stats.get_duration()
        throughput = self.stats.get_throughput_bps()
        frame_rate = self.stats.get_frame_rate()
        packet_error_rate = self.stats.get_packet_error_rate()
        frame_error_rate = self.stats.get_frame_error_rate()
        
        print(f"\n📊 Statistics (Runtime: {duration:.1f}s)")
        print(f"   Throughput: {throughput/1024:.2f} KB/s ({throughput:.0f} bytes/s)")
        print(f"   Frame Rate: {frame_rate:.2f} frames/s")
        print(f"   Packets: {self.stats.valid_packets}/{self.stats.total_packets} valid ({packet_error_rate:.2f}% error)")
        print(f"   Frames:  {self.stats.valid_frames}/{self.stats.total_frames} valid ({frame_error_rate:.2f}% error)")
        print(f"   Data:    {self.stats.total_bytes} bytes total")

def handle_client(conn: socket.socket, processor: LidarDataProcessor):
    """Handle data from a connected client"""
    buf_size = 4096
    
    while True:
        try:
            chunk = conn.recv(buf_size)
            if not chunk:
                print("客户端断开连接")
                break
            processor.process_data(chunk)
        except Exception as e:
            print(f"处理数据错误: {e}")
            break

def serve_once(listen_host: str, port: int, outfile: Path):
    """Listen and handle a single client connection"""
    processor = LidarDataProcessor(outfile)
    
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((listen_host, port))
        srv.listen(1)
        print(f"🚀 Listening on {listen_host}:{port} ... (Ctrl-C to exit)")

        conn, addr = srv.accept()
        with conn:
            print(f"✅ 客户端连接: {addr}")
            handle_client(conn, processor)
            
        # Print final statistics
        print(f"\n🏁 Final Statistics:")
        processor._print_statistics()

def main():
    p = argparse.ArgumentParser(description="Enhanced TCP server for LiDAR data validation")
    p.add_argument("--host", default="0.0.0.0", help="Listen address (default 0.0.0.0)")
    p.add_argument("--port", default=3334, type=int, help="Listen port (default 3334)")
    p.add_argument("--outfile", default="dump_enhanced.hex", help="Destination file path")
    args = p.parse_args()

    out_path = Path(args.outfile)

    try:
        while True:
            serve_once(args.host, args.port, out_path)
    except KeyboardInterrupt:
        print("\n👋 已退出")
    except OSError as e:
        print(f"Socket 错误: {e}", file=sys.stderr)

if __name__ == "__main__":
    main()