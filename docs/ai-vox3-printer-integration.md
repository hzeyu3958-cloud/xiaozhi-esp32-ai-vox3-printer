# AI-VOX3 + MY-638 改造记录

本文记录该版本相对上游 `xiaozhi-esp32` 增加的热敏打印能力，便于后续维护、移植和排查。

## 数据链路

```text
用户语音
  -> 小智后端识别意图
  -> 设备 MCP Tool Call
  -> ai_vox3_board.cc
  -> SerialPrinterTool
  -> UART2 / GPIO5、GPIO6
  -> MY-638 ESC/POS 打印机
```

内置模板直接从 ESP32 Flash 读取；动态模板可以由 MCP 传入 Base64，也可以让 ESP32 从 URL 下载二进制 ESC/POS 数据。

## 修改文件

| 文件 | 修改内容 |
| --- | --- |
| `main/boards/ai-vox3/config.h` | 打印机 UART、引脚、波特率、纸宽和限制 |
| `main/boards/ai-vox3/ai_vox3_board.cc` | 初始化打印机并注册 MCP 工具 |
| `main/boards/common/serial_printer_tool.h` | 串口打印类接口 |
| `main/boards/common/serial_printer_tool.cc` | 数据发送、Base64、校验、浓度、自检和状态查询 |
| `main/CMakeLists.txt` | 将 ESC/POS 模板嵌入 AI-VOX3 固件 |
| `main/boards/ai-vox3/printer_*.escpos` | 四个内置打印模板 |
| `638-printer/` | PC 端转换、预览、打印与诊断工具 |

## 串口实现

- UART：UART2。
- TX：GPIO5。
- RX：GPIO6。
- 格式：9600、8N1、无硬件流控。
- ESP-IDF TX 缓冲区：4096 bytes。
- 写入块：128 bytes。
- 块间等待串口发送完成，并保留短暂间隔。

`SendRawLocked` 负责可靠分块写入。普通文本打印结束后追加走纸；包含完整 `GS v 0` 光栅图的负载由数据本身控制走纸，避免图片条带之间产生白缝。

## 数据校验

动态打印流会执行以下检查：

- Base64 字符、padding 和解码长度是否合法。
- 数据是否超过设备允许的最大长度。
- ESC/POS 光栅头中的宽度、高度和实际数据长度是否一致。
- 服务端声明的型号、纸宽、波特率和板型是否匹配当前设备。
- URL 下载是否成功、HTTP 状态是否正常、实际长度是否完整。

这些校验用于避免错误网页、截断数据或错误规格的打印任务直接进入打印机。

## 内置模板

| 文件 | 用途 | 大小（约） |
| --- | --- | --- |
| `printer_lucky_ticket.escpos` | 高清顶呱呱小票 | 106KB |
| `printer_lucky_ticket_fast.escpos` | 低分辨率快速实验版 | 27KB |
| `printer_country_card_egypt.escpos` | 今日国家卡：埃及 | 137KB |
| `printer_wealth_ticket.escpos` | 先别慌先发财 | 113KB |

默认“打印顶呱呱小票”和高清命令都使用高清模板。快速实验版保留在固件资源中，但没有作为默认语音行为。

## 浓度策略

发送层在 `ESC @` 初始化之后统一注入 `1B 37 08 8C 14`。如果模板开头已经有 `ESC 7`，原参数会被替换，避免不同模板浓度不一致。

曾测试过最大值以及更长加热时间。连续大面积黑色时会出现竖向缺列、半边偏浅和打印头负载过高，因此恢复为当前平衡值。固定在同一横向位置的缺列若也出现在打印机内部自检页中，应按硬件故障处理。

## 已知限制

- 9600 波特率下，整页 576 dots 位图需要一至两分多钟。
- MY-638 的 64KB RAM 不适合一次缓存多个大模板，因此当前采用边接收边打印。
- 115200 可以提速，但打印机本体必须先使用厂商工具修改，固件不能单方面切换。
- 内置模板会占用应用镜像空间；新增模板时需关注分区余量。
- `print_label_text` 主要用于判断打印机是否处于 1A 标签模式，当前仅可靠支持 ASCII。

## 验证记录

- ESP-IDF 5.4.2 编译通过。
- 目标芯片为 ESP32-S3，AI-VOX3 使用 16MB 分区表。
- 已验证 COM6 低速烧录完成，应用镜像和资源分区哈希校验通过。
- 打印机通信当前保持 9600，与打印机本体设置一致。
