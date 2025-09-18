# 开启6001端口 将接收的内容全部打印

import socket

HOST = "0.0.0.0"   # 监听所有网卡
PORT = 6001        # 端口号

def run_server():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        # 允许端口复用，避免重启时报地址占用
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((HOST, PORT))
        s.listen(1)
        print(f"[*] TCP server listening on {HOST}:{PORT}")

        while True:
            conn, addr = s.accept()
            print(f"[+] Connection from {addr}")
            with conn:
                while True:
                    data = conn.recv(1024)
                    if not data:  # 对端关闭连接
                        print("[-] Client disconnected")
                        break
                    print(data.decode(errors="ignore"))  # 打印收到的内容

if __name__ == "__main__":
    run_server()
