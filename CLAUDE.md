# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an **ESP32-S3 firmware project** that implements a high-speed **UART-to-TCP bridge** specifically designed for **LiDAR data transmission**. The system operates at 921600 baud UART and forwards data bidirectionally between a LiDAR device and TCP connections over WiFi.

## Build and Development Commands

### Basic ESP-IDF Commands
```bash
# Configure the project (run once or when changing configs)
idf.py menuconfig

# Build the project
idf.py build

# Flash to device (requires /dev/ttyACM0 with 777 permissions)
sudo chmod 777 /dev/ttyACM0
idf.py flash

# Monitor serial output
idf.py monitor

# Clean build
idf.py clean

# Build, flash, and monitor in one command
idf.py build flash monitor
```

### Testing Commands
```bash
# Run TCP client test (receives and displays hex data)
python3 get_data.py

# Run TCP server test (for client mode testing)
python3 get_data_server.py
```

### Device Setup
```bash
# Set device permissions (required before each flash)
sudo chmod 777 /dev/ttyACM0
```

## Architecture Overview

### Core Components
- **Modular Design**: Code separated into config.h, dataproc.c/h, and main.c
- **WiFi Management**: STA mode with automatic retry logic  
- **UART Handler**: High-speed serial (921600 baud) with pin 3/4
- **TCP Client**: Only client mode, connects to server
- **LiDAR Data Processing**: Frame validation and buffering
- **FreeRTOS Tasks**: Multi-threaded data forwarding with validation

### File Structure
```
main/
├── main.c         # WiFi, TCP client, application entry
├── dataproc.c/h   # LiDAR packet/frame processing and validation
├── config.h       # All configuration constants
└── CMakeLists.txt # Updated to include dataproc.c
```

### Data Flow
```
LiDAR Device → UART(3/4, 921600) → Frame Assembly → Validation → Buffering → TCP Client → Server
```

### Key Configuration (in config.h)
```c
#define REMOTE_SERVER_IP    "192.168.114.117"
#define REMOTE_SERVER_PORT  3334
#define UART_TX_PIN         3               // Changed from 17
#define UART_RX_PIN         4               // Changed from 18
#define UART_BAUD_RATE      921600
```

## LiDAR Protocol Specification

### Packet Structure
- **Header**: `0A 00` (2 bytes)
- **Sequence**: 1 byte (0-7 for frame packets)
- **Reserved**: `00` (1 byte)
- **Data**: 40 bytes payload
- **Total**: 44 bytes per packet

### Frame Structure  
- **8 packets** (sequence 0-7) = **1 complete frame** (352 bytes total)
- **Frame detection**: Look for sequence 0 packet to identify frame start

## Development Patterns

### Task Architecture
The application uses FreeRTOS tasks with mutex-protected socket access:
- **UART→TCP Task**: Forwards UART data to TCP socket
- **TCP→UART Task**: Forwards TCP data to UART
- **WiFi Event Handler**: Manages connection state

### Socket Management
- Thread-safe access using mutexes
- Automatic reconnection in client mode
- IPv4/IPv6 dual-stack support

### Error Handling
- WiFi retry logic with exponential backoff
- Socket error recovery and reconnection
- UART buffer overflow protection

## Common Development Tasks

### Changing Network Configuration
Edit `main.c` and modify:
```c
#define WIFI_SSID "YourNetworkName"
#define WIFI_PASS "YourPassword"
```

### Switching TCP Mode
```c
#define TCP_BRIDGE_CLIENT 1  // Client mode
#define TCP_BRIDGE_CLIENT 0  // Server mode
```

### Modifying UART Settings
```c
#define UART_BAUD_RATE 921600  // Current baud rate
#define UART_NUM UART_NUM_0    // UART port
```

## Testing and Validation

### Data Validation
- Use `get_data.py` for basic data capture and hex dumps
- Use `get_data_server.py` for enhanced validation with real-time statistics
- Automatic LiDAR packet validation (44 bytes, 0A 00 header, sequence 0-7)
- Frame integrity checking and error reporting
- Real-time throughput and frame rate monitoring

### Debug Output
- Use `idf.py monitor` to view ESP32 debug logs
- Key log levels: INFO for important events, DEBUG for detailed packet analysis
- Check WiFi connection status and socket states
- Monitor UART and TCP data flow rates
- Use `idf.py menuconfig` to adjust log levels if needed

## Project Structure Notes

- **Modular Design**: Separated into config.h, dataproc.c/h, and main.c
- **No Unit Tests**: Testing done via integration with Python scripts  
- **Configuration**: Centralized in config.h header file
- **Build System**: Standard ESP-IDF CMake configuration with multiple source files
- **Data Processing**: Dedicated module for LiDAR packet validation and frame assembly

## Recent Debug Improvements

- **关键修复**: 修复数据包边界检测问题 - 现在正确地从0A 00包头开始截取44字节数据包
- **数据类型强转**: 所有ESP_LOG格式化参数都已正确强转为unsigned int  
- **日志级别优化**: 详细调试信息移至DEBUG级别，减少INFO级别噪音
- **同步监控**: 添加丢弃字节数统计，帮助监控数据包同步状态
- **性能优化**: 减少不必要的字符串操作和日志输出
- **代码简洁性**: 符合TODO要求的简洁可读性标准

### 数据包同步算法
1. 逐字节扫描寻找`0A 00`包头序列
2. 找到包头后继续读取42字节完成44字节数据包
3. 验证完整包的格式和序列号
4. 处理完成后重置状态，继续寻找下一个包头

## Hardware Requirements

- **ESP32-S3 development board**
- **USB connection** to `/dev/ttyACM0`
- **WiFi network** access
- **LiDAR device** with UART output (921600 baud)