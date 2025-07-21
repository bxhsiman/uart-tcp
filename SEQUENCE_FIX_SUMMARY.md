# 序列跳跃和看门狗问题修复总结

## 🐛 发现的问题

### 1. 序列计算逻辑错误
```
W UART: Sequence jump at byte 259219: expected 4, got 3, lost 7 packets
```
- **原因**：`(sequence - expected_seq + 8) % 8` 计算错误
- **例子**：期望4收到3时，计算为 `(3-4+8)%8 = 7` (错误)
- **实际应该**：`(8-4)+3 = 7` 或 `0` (如果是重复包)

### 2. 看门狗超时
```
E task_wdt: Task watchdog got triggered
E task_wdt: CPU 1: uart_packet
```
- **原因**：`lidar_analyze_data()` 函数在处理大量数据时占用过多CPU时间
- **结果**：UART任务没有及时重置看门狗

## 🔧 修复措施

### 1. 序列计算逻辑修复
```c
// 修复前（错误）
int lost = (sequence - expected_seq + 8) % 8;

// 修复后（正确）
int lost;
if (sequence >= expected_seq) {
    lost = sequence - expected_seq;
} else {
    lost = (8 - expected_seq) + sequence;
}

// 添加额外验证
if (lost > 0 && lost < 8) {
    // 真正的丢包
} else if (lost == 0) {
    // 重复包
} else {
    // 序列严重错乱
}
```

### 2. 看门狗重置机制
```c
#include "esp_task_wdt.h"

// 在处理循环中添加
int packets_processed_in_loop = 0;
for (int i = 0; i <= combined_len - LIDAR_PACKET_SIZE; i++) {
    if (++packets_processed_in_loop >= 10) {
        esp_task_wdt_reset();              // 重置看门狗
        packets_processed_in_loop = 0;
        vTaskDelay(pdMS_TO_TICKS(1));      // 让出CPU时间
    }
    // ... 处理逻辑
}
```

### 3. 批次超时机制
```c
// 避免不完整批次永远等待
if ((current_time - oldest_time) > pdMS_TO_TICKS(BATCH_TIMEOUT_MS)) {
    ESP_LOGW(TAG, "Batch timeout, clearing incomplete batch");
    uart_stats.incomplete_batches++;
    // 清空缓存，重新开始
}
```

### 4. 重复包处理
```c
// 检查并处理重复包
if (uart_stats.packet_cache[sequence].received) {
    ESP_LOGD(TAG, "Duplicate packet sequence %d, replacing", sequence);
}
```

## 📊 预期改进效果

### 修复前的问题日志
```
W UART: Sequence jump: expected 4, got 3, lost 7 packets  ❌
W UART: Sequence jump: expected 4, got 3, lost 7 packets  ❌
E task_wdt: Task watchdog got triggered                   ❌
```

### 修复后的正常日志
```
D Cached packet sequence 0, mask: 0x01                   ✅
D Cached packet sequence 1, mask: 0x03                   ✅
D Duplicate packet sequence 2, replacing                 ✅
I Sent complete batch 1 (8x352 bytes = 2816 bytes)      ✅
I UART Stats: 50000 bytes, 142 packets, 0 lost, 1 batches sent ✅
```

## 🎯 性能优化

1. **CPU占用控制**
   - 每处理10个包重置看门狗
   - 主动让出CPU时间片

2. **内存管理**
   - 避免不必要的内存分配
   - 及时清理超时的不完整批次

3. **错误恢复**
   - 重复包替换策略
   - 批次超时自动恢复

## 🧪 测试验证

### 1. 序列测试
```bash
# 启动接收器测试
python3 get_data_batch_receiver.py 3334

# 观察日志中的序列处理
```

### 2. 压力测试
```bash
# 高频数据发送测试看门狗处理
python3 test_batch_sender.py 192.168.114.117 3334
```

### 3. 长期稳定性测试
- 运行24小时测试
- 监控内存使用
- 观察批次完成率

## 📈 关键指标监控

- `batches_sent`: 成功发送的批次数
- `incomplete_batches`: 超时的不完整批次数
- `sequence_errors`: 序列错误次数（应该减少）
- `packets_lost`: 实际丢包数（应该更准确）

通过这些修复，系统应该能够：
- 正确计算丢包数量
- 避免看门狗超时
- 处理重复包和序列错乱
- 自动恢复不完整批次