#!/usr/bin/env python3
# tcp_client_hex_dump.py
#
# Usage:
#   python tcp_client_hex_dump.py --host 192.168.4.1 --port 3333 --outfile dump.hex
#
# 每个字节写成两位大写十六进制并以空格分隔：
#   0A 00 00 00 10 01 ...
# 每当出现 "0A 00" 序列（包头）时，先输出一个 '\n'，再输出 "0A 00 ".

import argparse
import socket
import sys
from pathlib import Path

HEADER_SEQ = (0x0A, 0x00)        # 包头：0A 00

def dump_stream(sock: socket.socket, out_path: Path):
    """
    从 sock 连续读取数据并写到 out_path。
    碰到 HEADER_SEQ 就在写入前插入换行。
    """
    buf_size = 1024
    prev_byte = None

    with out_path.open("ab", buffering=0) as f:  # 以二进制追加、无缓冲
        while True:
            chunk = sock.recv(buf_size)
            if not chunk:
                print("服务器关闭连接，退出。")
                break

            # 逐字节处理，以便检测交叉 chunk 的包头
            for b in chunk:
                if prev_byte is None:
                    prev_byte = b
                    continue

                # 检测到 prev_byte + 当前字节 组成的包头
                if (prev_byte, b) == HEADER_SEQ:
                    f.write(b"\n")                 # 插入换行

                # 写上一个字节（因为现在已知道它是否是包头起始）
                f.write(f"{prev_byte:02X} ".encode())

                prev_byte = b

        # 连接结束后，把最后一个缓存字节写出来
        if prev_byte is not None:
            f.write(f"{prev_byte:02X} ".encode())

def main():
    parser = argparse.ArgumentParser(description="TCP client that dumps incoming bytes to a hex file.")
    parser.add_argument("--host", required=True, help="Server IP / hostname")
    parser.add_argument("--port", required=True, type=int, help="Server port")
    parser.add_argument("--outfile", default="dump.hex", help="Destination file path")
    args = parser.parse_args()

    server_addr = (args.host, args.port)
    out_path = Path(args.outfile)

    try:
        with socket.create_connection(server_addr, timeout=10) as sock:
            print(f"已连接到 {server_addr}, 开始接收...")
            dump_stream(sock, out_path)
    except (OSError, socket.timeout) as e:
        print(f"连接失败或中断: {e}", file=sys.stderr)

if __name__ == "__main__":
    main()
