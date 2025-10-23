import socket
import json
import base64
import struct

HOST = "0.0.0.0"   # 监听所有网卡
PORT = 6001        # 端口号

def decode_payload_to_hex_and_parse(base64_payload):
    """将base64 payload解码为十六进制字符串并解析标志位后的uint32"""
    try:
        # Base64解码
        decoded_bytes = base64.b64decode(base64_payload)
        # print(f"{decoded_bytes[4:8].hex()}")
        # 解析数据
        if len(decoded_bytes) >= 8:  # 至少需要8字节（4字节标志位 + 4字节uint32）
            # 检查标志位是否为 0a 00 00 00
            signature = decoded_bytes[:4]
            if signature == b'\x0a\x00\x00\x00':
                # 提取第5-8字节（索引4-7）并解析为小端序uint32
                uint32_value = struct.unpack('<I', decoded_bytes[4:8])[0]
                signature_info = "标志位匹配: 0a000000"
            else:
                # 如果标志位不匹配，也尝试解析第5-8字节
                uint32_value = struct.unpack('<I', decoded_bytes[4:8])[0]
                signature_info = f"标志位不匹配: {signature.hex()}"
        else:
            uint32_value = "数据长度不足8字节"
            signature_info = "数据长度不足"
        
        # 转换为十六进制字符串，每两个字符一组
        hex_string = decoded_bytes.hex()
        # 格式化输出，每32个字符换行
        formatted_hex = '\n'.join([hex_string[i:i+32] for i in range(0, len(hex_string), 32)])
        
        return formatted_hex, uint32_value, signature_info
    except Exception as e:
        return f"解码错误: {e}", "解析失败", f"错误: {e}"

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
                buffer = ""
                while True:
                    data = conn.recv(1024).decode('utf-8', errors='ignore')
                    if not data:  # 对端关闭连接
                        print("[-] Client disconnected")
                        break
                    
                    buffer += data
                    
                    # 处理完整的JSON对象
                    while buffer:
                        # 查找完整的JSON对象（以{开始，以}结束）
                        start = buffer.find('{')
                        if start == -1:
                            buffer = ""
                            break
                            
                        brace_count = 0
                        end = -1
                        for i, char in enumerate(buffer[start:]):
                            if char == '{':
                                brace_count += 1
                            elif char == '}':
                                brace_count -= 1
                                if brace_count == 0:
                                    end = start + i + 1
                                    break
                        
                        if end == -1:
                            break  # 没有找到完整的JSON对象，等待更多数据
                            
                        json_str = buffer[start:end]
                        buffer = buffer[end:]
                        
                        try:
                            # 解析JSON
                            data_obj = json.loads(json_str)
                            
                            # 提取mac、len和payload
                            mac = data_obj.get('mac', 'N/A')
                            length = data_obj.get('len', 'N/A')
                            payload = data_obj.get('payload', '')
                            
                            # print(f"\n=== 收到数据 ===")
                            # print(f"MAC: {mac}")
                            # print(f"长度: {length}")
                            # print(f"Payload (Base64): {payload[:50]}...")  # 只显示前50个字符
                            
                            # 解码并解析
                            hex_output, uint32_value, signature_info = decode_payload_to_hex_and_parse(payload)
                            
                            # print(f"标志位: {signature_info}")
                            # uint32_value 转为hex打印
                            print(f"{hex(uint32_value) , uint32_value}")
                            
                            # print(f"Payload (Hex):")
                            # print(hex_output)
                            # print("=" * 50)
                            
                        except json.JSONDecodeError as e:
                            print(f"JSON解析错误: {e}")
                            print(f"原始数据: {json_str}")

if __name__ == "__main__":
    run_server()