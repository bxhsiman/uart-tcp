import socket
import struct

HOST = "0.0.0.0"   # 监听所有网卡
PORT = 6001        # 端口号

def run_server():
    buff = b''
    header = b'\x0a\x00\x00\x00'
    
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        # 允许端口复用，避免重启时报地址占用
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((HOST, PORT))
        s.listen(1)
        print(f"[*] TCP server listening on {HOST}:{PORT}")
        print(f"[*] 等待接收连续数据流，只解析 0a000000 后面的uint32...")

        while True:
            conn, addr = s.accept()
            print(f"\n[+] 接收到来自 {addr} 的连接")
            buff = b''  # 为每个连接重置缓冲区
            
            with conn:
                while True:
                    try:
                        # 接收数据
                        data = conn.recv(4096)
                        if not data:  # 对端关闭连接
                            print("[-] 客户端断开连接")
                            break
                        
                        # 添加到缓冲区
                        buff += data
                        
                        # 处理缓冲区中的数据
                        processed = False
                        while len(buff) >= 8:  # 至少需要包头+uint32的长度
                            # 检查是否是有效的包头
                            if buff[:4] == header:
                                # 提取并解析uint32
                                seq_bytes = buff[4:8]
                                
                                try:
                                    seq_num = struct.unpack('<I', seq_bytes)[0]
                                    print(f"[√] 解析到uint32: {seq_num} (原始字节: {seq_bytes.hex()})")
                                    
                                    # 丢弃已处理的8字节（包头+uint32）
                                    buff = buff[8:]
                                    processed = True
                                    
                                except Exception as e:
                                    print(f"[!] 解析uint32失败: {e}")
                                    # 解析失败，丢弃包头，继续处理
                                    buff = buff[4:]
                                    processed = True
                            else:
                                # 不是有效的包头，丢弃第一个字节，继续查找
                                buff = buff[1:]
                                processed = True
                        
                        # 如果一轮处理中没有处理任何数据，且缓冲区很大，说明有异常数据
                        if not processed and len(buff) > 1000:
                            print(f"[!] 缓冲区堆积 {len(buff)} 字节无效数据，清空缓冲区")
                            buff = b''
                            
                    except Exception as e:
                        print(f"[!] 处理数据时出错: {e}")
                        break

if __name__ == "__main__":
    run_server()