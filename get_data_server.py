#!/usr/bin/env python3
# tcp_server_hex_dump.py
#
# Usage:
#   python tcp_server_hex_dump.py --host 0.0.0.0 --port 3333 --outfile dump.hex
#
# 每个字节写成两位大写十六进制并以空格分隔：
#   0A 00 00 00 10 01 ...
# 当检测到 "0A 00" 序列（包头）时，先输出 '\n'，再输出 "0A 00 ".

import argparse
import socket
import sys
from pathlib import Path

HEADER_SEQ = (0x0A, 0x00)        # 包头：0A 00

def dump_stream(conn: socket.socket, out_path: Path):
    """把 conn 持续收到的数据写入 out_path（十六进制 & 包头换行）"""
    buf_size  = 1024
    prev_byte = None

    with out_path.open("ab", buffering=0) as f:      # 追加、无缓冲
        while True:
            chunk = conn.recv(buf_size)
            if not chunk:                            # 客户端关闭
                print("对方已断开。")
                break

            for b in chunk:                          # 逐字节检测包头
                if prev_byte is None:
                    prev_byte = b
                    continue

                if (prev_byte, b) == HEADER_SEQ:
                    f.write(b"\n")                   # 插入换行

                f.write(f"{prev_byte:02X} ".encode())
                prev_byte = b

        if prev_byte is not None:                    # 写最后一个残留字节
            f.write(f"{prev_byte:02X} ".encode())

def serve_once(listen_host: str, port: int, outfile: Path):
    """监听并与单个客户端会话，结束后返回"""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((listen_host, port))
        srv.listen(1)
        print(f"Listening on {listen_host}:{port} ... (Ctrl‑C 退出)")

        conn, addr = srv.accept()
        with conn:
            print(f"已连接：{addr}")
            dump_stream(conn, outfile)

def main():
    p = argparse.ArgumentParser(description="TCP server that dumps incoming bytes to a hex file.")
    p.add_argument("--host", default="0.0.0.0", help="Listen address (default 0.0.0.0)")
    p.add_argument("--port", required=True, type=int, help="Listen port")
    p.add_argument("--outfile", default="dump.hex", help="Destination file path")
    args = p.parse_args()

    out_path = Path(args.outfile)

    try:
        while True:                  # 断线后继续等待下一个客户端
            serve_once(args.host, args.port, out_path)
    except KeyboardInterrupt:
        print("\n已退出。")
    except OSError as e:
        print(f"Socket 错误: {e}", file=sys.stderr)

if __name__ == "__main__":
    main()
