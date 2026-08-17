# AI-VOX3 板级说明

## 硬件概况

AI-VOX3 使用 ESP32-S3R8，板载 8MB PSRAM 和 16MB Flash，支持 LCD/OLED、ES8311 音频编解码、双麦克风、扬声器、SD 卡、按键和 WS2812B。

本项目在 AI-VOX3 的侧边 PH2.0/GPIO 接口上增加 MY-638 热敏打印机。打印机对象在设备 MCP 工具注册前初始化，可由语音意图直接调用。

## 打印机接线

AI-VOX3 侧边接口从上到下为：GND、5V、GPIO5、GPIO6。

| AI-VOX3 | MY-638 | 说明 |
| --- | --- | --- |
| GND | GND | 必须共地 |
| GPIO5 | RX | ESP32 发送打印数据 |
| GPIO6 | TX | 打印机返回状态 |
| 5V 或独立电源 | VCC | 推荐稳定 5-9V、至少 1.5A |

不要带电插拔打印头排线。使用独立电源时，电源负极仍需与 AI-VOX3 GND 相连。

## 固件参数

打印机参数定义在 `config.h`：

```c
#define PRINTER_UART_PORT UART_NUM_2
#define PRINTER_UART_TX_PIN GPIO_NUM_5
#define PRINTER_UART_RX_PIN GPIO_NUM_6
#define PRINTER_UART_BAUD_RATE 9600
#define PRINTER_MODEL "638"
#define PRINTER_PAPER_WIDTH_MM 80
```

当前固定为 9600、8N1、无流控。修改波特率时，打印机本体与固件必须同步修改。

## 代码结构

- `ai_vox3_board.cc`：初始化串口打印机并注册 MCP 工具。
- `../common/serial_printer_tool.cc`：串口分块发送、Base64 解码、ESC/POS 校验、浓度处理、自检和状态查询。
- `printer_*.escpos`：编译时嵌入固件的内置小票。
- `../../CMakeLists.txt`：只在 AI-VOX3 构建中嵌入打印资源。

当前统一浓度指令为 `1B 37 08 8C 14`。该参数在清晰度、打印头负载和连续大图稳定性之间取平衡；不要直接使用 `FF FF FF`，否则可能出现过热、掉点和固定竖纹。

## MCP 工具

| 工具名 | 参数/行为 |
| --- | --- |
| `self.printer.get_profile` | 无参数，返回打印配置和初始化状态 |
| `self.printer.send_base64` | Base64 字节流及型号、纸宽、波特率、板型校验 |
| `self.printer.print_url` | 下载 `application/octet-stream` ESC/POS 数据 |
| `self.printer.print_lucky_ticket` | 内置高清顶呱呱模板 |
| `self.printer.print_lucky_ticket_hd` | 高清模板的显式别名 |
| `self.printer.print_country_card_egypt` | 内置埃及国家卡模板 |
| `self.printer.print_wealth_ticket` | 内置发财小票模板 |
| `self.printer.selftest` | 发送 `ESC @ + DC2 T` |
| `self.printer.get_status` | 发送 `DLE EOT 1..4` 并解析状态 |
| `self.printer.send_hex` | 原样发送十六进制字节，不追加走纸 |
| `self.printer.print_label_text` | 使用 1A 标签指令打印 ASCII |
| `self.printer.test_hello` | 本地链路测试 |
| `self.printer.print_text` | 打印中英文文本，字号 1-3 |

## 编译配置

`config.json` 包含：

- `ai-vox3`：普通版本，自定义唤醒词“你好呱呱”。
- `ai-vox3-aec`：开启设备端 AEC 的版本。

构建命令：

```bash
python scripts/release.py ai-vox3 --name ai-vox3
```

也可以手动选择 AI-VOX3 后构建：

```bash
idf.py set-target esp32s3
idf.py menuconfig
# 在 Board Type 中选择 AI-VOX3
idf.py build
idf.py -p COM6 -b 115200 flash
```

这里的 `-b 115200` 仅表示电脑烧录 ESP32 的速度，与打印机的 9600 波特率无关。

## 故障定位

- 完全不出纸：先调用 `self.printer.selftest`，检查 TX/RX 交叉、共地、供电和波特率。
- 乱码：优先检查打印机本体与 `config.h` 的波特率是否一致。
- 整体偏浅：更换热敏纸，检查纸张正反面和电源电流，再微调加热时间。
- 固定半边或固定竖条不打印：打印自检页；自检页也异常说明是压纸、打印头、排线或驱动板故障，不是图片算法。
- ESP32 打印时重启：通常是供电压降，打印机应使用独立大电流电源并与主控共地。
