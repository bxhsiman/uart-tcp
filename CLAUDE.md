# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an ESP32-S3 firmware project that implements a TCP-to-UART bridge. The main application can operate in two modes:
- **TCP Server mode**: Listens for TCP connections and bridges data to/from UART
- **TCP Client mode**: Connects to a remote TCP server and bridges data to/from UART

## Development Commands

### Build Commands
```bash
idf.py build                    # Build the project
idf.py clean                    # Clean build artifacts
idf.py reconfigure             # Reconfigure project
./build_check.sh               # Validate environment and build automatically
```

### Flash and Monitor
```bash
idf.py flash                   # Flash firmware to device
idf.py monitor                 # Monitor serial output
idf.py flash monitor           # Flash and monitor in one command
```

### Configuration
```bash
idf.py menuconfig             # Open configuration menu
```

## Architecture

### Core Components
The project is now modularized with separate components:

- **main/main.c**: Main application entry point and module initialization
- **main/config.c/h**: Configuration management system
- **main/wifi_manager.c/h**: WiFi connection management (STA and SoftAP modes)
- **main/tcp_client.c/h**: TCP client connection handling
- **main/uart_handler.c/h**: UART communication interface
- **main/lidar_packet.c/h**: LiDAR packet processing and framing
- **main/web_server.c/h**: HTTP web server for configuration interface

### Key Configuration
- **Target**: ESP32-S3
- **WiFi**: Supports both STA-only and combined SoftAP+STA modes
- **Configuration**: Runtime configurable via web interface and persistent storage
- **UART**: Configurable baud rate and pins via config system
- **TCP Client**: Configurable server address and port
- **LiDAR Processing**: Handles packet framing and processing

### Development Scripts
- **get_data*.py**: Various TCP client/server utilities for data capture and testing
  - `get_data.py`: Basic TCP client for hex data dumping
  - `get_data_server.py`: TCP server for hex data dumping
  - `get_data_server_batch.py`: Batch processing variant
  - `get_data_server_framed.py`: Framed packet processing variant
- **build_check.sh**: Build environment validation and automated build script
- **test_servers.sh**: Testing script for server configurations

## Development Notes

- Uses ESP-IDF v5.4.1 framework with modular architecture
- Configuration stored in NVS (non-volatile storage) and accessible via web interface
- Supports runtime reconfiguration without reflashing firmware
- Modular callback-based design for inter-component communication
- FreeRTOS task-based architecture with proper synchronization
- Web-based configuration interface when SoftAP mode is enabled

## Build Validation
Use `./build_check.sh` to verify project structure and perform automated build validation.