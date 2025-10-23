# ESP32 UART-TCP JSON转发桥

基于ESP32-C2的UART到TCP网络桥接固件,专为LiDAR数据传输设计。通过WiFi将UART串口数据以JSON格式封装后转发到TCP服务器。

## 主要特性

- **高速UART**: 支持921600波特率串口通信
- **JSON封装**: 数据自动封装为JSON格式,包含设备MAC地址和Base64编码的payload
- **LiDAR协议支持**: 自动识别和验证LiDAR数据包格式(44字节/包, 8包/帧)
- **TCP客户端模式**: 自动连接服务器并支持断线重连
- **WiFi SoftAP配置**: 可通过Web界面配置WiFi和服务器参数
- **实时监控**: Web界面显示运行状态和数据统计

## JSON数据包格式

系统将接收到的二进制UART数据封装为JSON格式后发送:

```json
{
  "mac": "AA:BB:CC:DD:EE:FF",
  "len": 352,
  "payload": "CgAAAP...base64编码的数据..."
}
```

### 字段说明

| 字段 | 类型 | 说明 |
|------|------|------|
| `mac` | String | 设备WiFi STA接口的MAC地址,格式: `XX:XX:XX:XX:XX:XX` |
| `len` | Number | 原始二进制数据的字节长度(Base64编码前) |
| `payload` | String | 经过Base64编码的原始二进制数据 |

### 数据流程

```
LiDAR设备
  ↓ (UART 921600bps)
ESP32 UART接收
  ↓ (数据包验证和帧组装)
Base64编码
  ↓ (JSON封装)
TCP发送到服务器
  ↓
{"mac":"...", "len":352, "payload":"..."}
```

## 核心配置参数

所有配置参数位于 `main/config.h` 文件中:

### 网络配置

```c
/* WiFi STA默认配置 (可通过Web界面修改) */
#define WIFI_SSID           "miwifi"        // 默认WiFi SSID
#define WIFI_PASS           "(12345678)"    // 默认WiFi密码
#define WIFI_MAX_RETRY      9999            // WiFi重连最大次数
#define WIFI_TX_POWER       44              // WiFi发射功率 (2-80)

/* TCP客户端配置 (可通过Web界面修改) */
#define REMOTE_SERVER_IP    "180.184.52.3"  // 服务器IP地址
#define REMOTE_SERVER_PORT  6001            // 服务器端口
#define TCP_RECONNECT_MS    500             // TCP断线重连间隔(ms)
```

### WiFi管理配置

```c
#define ENABLE_SOFTAP       1               // 1=开启配置热点, 0=关闭
#define SOFTAP_SSID_PREFIX  "ESP_"          // 热点SSID前缀 (后接MAC地址)
#define SOFTAP_PASSWORD     "12345678"      // 热点密码 (至少8位)
#define SOFTAP_MAX_CONN     4               // 热点最大连接数
#define HTTP_SERVER_PORT    80              // Web配置页面端口
```

### UART配置

```c
#define UART_PORT_NUM       UART_NUM_1      // UART端口号
#define UART_BAUD_RATE      921600          // 波特率
#define UART_TX_PIN         1               // TX引脚
#define UART_RX_PIN         2               // RX引脚
#define UART_BUF_SIZE       2048            // UART缓冲区大小
```

### LiDAR数据包配置

```c
#define LIDAR_PACKET_SIZE   44              // 单个数据包大小(字节)
#define LIDAR_FRAME_PACKETS 8               // 每帧包含的数据包数量
#define LIDAR_FRAME_SIZE    352             // 完整帧大小 (44×8)
#define LIDAR_HEADER_0      0x0A            // 数据包头字节1
#define LIDAR_HEADER_1      0x00            // 数据包头字节2
#define FRAME_BUFFER_COUNT  2               // 缓冲帧数量
```

### 日志控制

```c
#define ENABLE_DEBUG_LOG    0               // 1=详细调试日志, 0=关闭
#define ENABLE_INFO_LOG     0               // 1=信息日志, 0=关闭
#define ENABLE_WARN_LOG     1               // 1=警告日志, 0=关闭
#define ENABLE_ERROR_LOG    1               // 1=错误日志, 0=关闭
```

### LED状态指示

```c
#define LED_PERIOD_NORMAL   1000            // 正常连接时LED闪烁周期(ms)
#define LED_PERIOD_ERROR    200             // 连接异常时LED闪烁周期(ms)
```

## WiFi功率映射表

`WIFI_TX_POWER` 参数映射关系 (来自ESP-IDF文档):

| 配置值 | 实际功率 | 配置值 | 实际功率 |
|--------|----------|--------|----------|
| 8      | 2 dBm    | 52     | 13 dBm   |
| 20     | 5 dBm    | 56     | 14 dBm   |
| 28     | 7 dBm    | 60     | 15 dBm   |
| 34     | 8 dBm    | 66     | 16 dBm   |
| 44     | 11 dBm   | 72     | 18 dBm   |
| 80     | 20 dBm   |        |          |

## 快速开始

### 环境要求

- ESP-IDF v5.4.1 或更高版本
- Python 3.10+
- ESP32-C2开发板

### 编译和烧录

```bash
# 1. 配置项目 (首次使用或修改配置时)
idf.py menuconfig

# 2. 编译
idf.py build

# 3. 设置设备权限
sudo chmod 777 /dev/ttyACM0

# 4. 烧录
idf.py flash

# 5. 查看日志
idf.py monitor

# 或者一键操作
idf.py build flash monitor
```

### 配置设备

#### 方法1: 通过Web界面配置 (推荐)

1. 设备上电后会创建名为 `ESP_XXXXXX` 的WiFi热点 (XXXXXX为MAC地址后6位)
2. 使用密码 `12345678` 连接该热点
3. 浏览器访问 `http://192.168.4.1`
4. 在Web界面配置:
   - WiFi SSID和密码
   - TCP服务器IP和端口
5. 点击保存,设备会自动重启并使用新配置

#### 方法2: 修改config.h

直接修改 `main/config.h` 中的默认值并重新编译烧录。

## 项目结构

```
uart-tcp/
├── main/
│   ├── main.c          # WiFi初始化和TCP客户端逻辑
│   ├── dataproc.c      # UART数据处理、JSON封装、Base64编码
│   ├── dataproc.h      # 数据处理模块头文件
│   ├── webserver.c     # Web配置服务器
│   ├── webserver.h     # Web服务器头文件
│   ├── config.h        # 所有配置参数 (⭐主要配置文件)
│   ├── CMakeLists.txt  # 组件编译配置
│   └── idf_component.yml  # 依赖管理 (cJSON库)
├── CMakeLists.txt      # 项目CMake配置
├── sdkconfig           # ESP-IDF配置
├── CLAUDE.md           # Claude AI助手指南
└── README.md           # 本文件
```

## LiDAR数据包协议

### 数据包格式 (44字节)

| 偏移 | 字段 | 大小 | 说明 |
|------|------|------|------|
| 0    | Header | 2B | 固定为 `0x0A 0x00` |
| 2    | Sequence | 1B | 序列号 0-7 |
| 3    | Reserved | 1B | 保留字节,固定为 `0x00` |
| 4-43 | Data | 40B | 有效载荷数据 |

### 数据帧格式 (352字节)

- 1帧 = 8个数据包
- 序列号从0到7依次递增
- 序列0标识新帧开始

### 数据验证

系统会自动验证:
- ✅ 包头是否为 `0x0A 0x00`
- ✅ 序列号是否在0-7范围内
- ✅ 保留字节是否为 `0x00`
- ✅ 序列号是否按顺序递增
- ✅ 帧是否完整(包含所有8个包)

## 测试工具

### TCP服务器测试 (Python)

```bash
# 接收并显示JSON数据
python3 get_data.py

# 或使用服务器模式(高级统计)
python3 get_data_server.py
```

## 接收端JSON解析示例

### Python示例

```python
import socket
import json
import base64

def receive_lidar_data(host='0.0.0.0', port=6001):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind((host, port))
    sock.listen(1)
    print(f"等待连接: {host}:{port}")

    conn, addr = sock.accept()
    print(f"设备已连接: {addr}")

    buffer = ""
    while True:
        data = conn.recv(4096).decode('utf-8')
        if not data:
            break

        buffer += data

        # 尝试解析JSON (可能一次接收多个JSON对象)
        while True:
            try:
                obj, idx = json.JSONDecoder().raw_decode(buffer)
                buffer = buffer[idx:].lstrip()

                # 解析JSON字段
                mac = obj['mac']
                original_len = obj['len']
                payload_b64 = obj['payload']

                # Base64解码
                binary_data = base64.b64decode(payload_b64)

                print(f"收到数据 - MAC: {mac}, 长度: {original_len}, "
                      f"解码后: {len(binary_data)}字节")

                # 处理二进制数据...

            except json.JSONDecodeError:
                break  # 等待更多数据

if __name__ == '__main__':
    receive_lidar_data()
```

### Node.js示例

```javascript
const net = require('net');

const server = net.createServer((socket) => {
    console.log('设备已连接:', socket.remoteAddress);

    let buffer = '';

    socket.on('data', (data) => {
        buffer += data.toString('utf-8');

        // 尝试解析JSON
        let startIdx = 0;
        while (true) {
            try {
                const result = buffer.substring(startIdx);
                const obj = JSON.parse(result);

                // 解析字段
                const mac = obj.mac;
                const len = obj.len;
                const payload = Buffer.from(obj.payload, 'base64');

                console.log(`MAC: ${mac}, 长度: ${len}, 数据: ${payload.length}字节`);

                // 处理数据...

                buffer = '';
                break;
            } catch (e) {
                break;  // 等待更多数据
            }
        }
    });
});

server.listen(6001, '0.0.0.0', () => {
    console.log('TCP服务器监听 0.0.0.0:6001');
});
```

## 性能指标

- **UART速率**: 921600 bps (约 115 KB/s)
- **LiDAR数据**: 每帧352字节
- **JSON开销**: 约1.33倍 (Base64编码 + JSON结构)
- **TCP吞吐**: 实测可达 ~100 KB/s

## 常见问题

### Q: 如何修改服务器IP和端口?

**A:** 两种方法:
1. 连接设备热点,访问 `http://192.168.4.1` Web界面修改
2. 修改 `main/config.h` 中的 `REMOTE_SERVER_IP` 和 `REMOTE_SERVER_PORT`

### Q: JSON格式为什么要用Base64?

**A:** UART接收的是二进制数据,JSON只能传输文本,Base64可以安全地将二进制数据编码为文本格式。

### Q: 为什么包含MAC地址?

**A:** 便于服务器区分多个设备的数据来源,适用于多设备部署场景。

### Q: len字段有什么用?

**A:** `len` 表示原始数据长度,方便接收端验证Base64解码后的数据是否完整。

### Q: 如何关闭Web配置功能?

**A:** 修改 `main/config.h` 中的 `ENABLE_SOFTAP` 为 `0`,重新编译烧录。

### Q: 如何调整日志输出?

**A:** 修改 `main/config.h` 中的日志开关:
- `ENABLE_DEBUG_LOG`: 详细调试信息
- `ENABLE_INFO_LOG`: 一般信息
- `ENABLE_WARN_LOG`: 警告信息
- `ENABLE_ERROR_LOG`: 错误信息

## 技术细节

### 依赖库

- **cJSON**: JSON编解码 (通过ESP-IDF组件管理器安装)
- **mbedtls**: Base64编码/解码
- **ESP-IDF标准库**: WiFi、TCP、UART驱动

### 内存管理

- 动态分配Base64缓冲区和JSON字符串
- 使用完毕后立即释放内存
- 帧缓冲区使用静态分配,避免频繁malloc

### 线程安全

- Socket句柄使用互斥锁保护
- 帧缓冲区使用互斥锁保护
- 配置参数使用互斥锁保护

## 开发指南

详细的开发说明和AI助手指南请参考 `CLAUDE.md` 文件。

## 许可证

本项目代码遵循 ESP-IDF 许可证。

## 贡献

欢迎提交Issue和Pull Request。

## 联系方式

如有问题,请通过GitHub Issue联系。
