#include <driver/rtc_io.h>
#include <esp_http_client.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <wifi_station.h>

#include <stdexcept>
#include <vector>
#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "config.h"
#include "display/lcd_display.h"
#include "dual_network_board.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "serial_printer_tool.h"

#include "ai_vox3_audio_codec.h"
#include "power_manager.h"

#define TAG "AIVOX3"

extern const uint8_t printer_lucky_ticket_start[] asm("_binary_printer_lucky_ticket_escpos_start");
extern const uint8_t printer_lucky_ticket_end[] asm("_binary_printer_lucky_ticket_escpos_end");
extern const uint8_t printer_lucky_ticket_fast_start[] asm("_binary_printer_lucky_ticket_fast_escpos_start");
extern const uint8_t printer_lucky_ticket_fast_end[] asm("_binary_printer_lucky_ticket_fast_escpos_end");
extern const uint8_t printer_country_card_egypt_start[] asm("_binary_printer_country_card_egypt_escpos_start");
extern const uint8_t printer_country_card_egypt_end[] asm("_binary_printer_country_card_egypt_escpos_end");
extern const uint8_t printer_wealth_ticket_start[] asm("_binary_printer_wealth_ticket_escpos_start");
extern const uint8_t printer_wealth_ticket_end[] asm("_binary_printer_wealth_ticket_escpos_end");

class AIVOX3 : public DualNetworkBoard {
  private:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    PowerManager *power_manager_;
    i2c_master_bus_handle_t codec_i2c_bus_;
    LcdDisplay *display_;
    SerialPrinterTool *printer_tool_ = nullptr;

    uint8_t GetAsciiFontCommandValue(int font_size) {
        if (font_size <= 1) {
            return 0x00;
        }
        if (font_size == 2) {
            return 0x11;
        }
        return 0x22;
    }

    uint8_t GetUtf8FontCommandValue(int font_size) {
        if (font_size <= 1) {
            return 0x00;
        }
        // 汉字模式先统一按倍宽倍高处理
        return 0x0C;
    }

    std::string BuildPrintPayload(const std::string &body, bool utf8_text_mode = false,
                                  int font_size = PRINTER_TEXT_FONT_SIZE_DEFAULT) {
        std::string payload;
        // 预留初始化、字号和编码切换命令空间
        payload.reserve(24 + body.size());
        // ESC @ 初始化
        payload.append("\x1B\x40", 2);
        if (utf8_text_mode) {
            // FS & 进入汉字模式
            payload.append("\x1C\x26", 2);
            // ESC 9 1 切换到 UTF-8 双字节编码
            payload.append("\x1B\x39\x01", 3);
            // FS ! n 设置汉字字号
            payload.append("\x1C\x21", 2);
            payload.push_back(static_cast<char>(GetUtf8FontCommandValue(font_size)));
        } else {
            // GS ! n 设置西文字号
            payload.append("\x1D\x21", 2);
            payload.push_back(static_cast<char>(GetAsciiFontCommandValue(font_size)));
        }

        payload.append(body);
        payload.push_back('\n');

        if (utf8_text_mode) {
            // FS . 退出汉字模式
            payload.append("\x1C\x2E", 2);
        }
        return payload;
    }

    static esp_err_t HandlePrintUrlHttpEvent(esp_http_client_event_t *evt) {
        if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == nullptr || evt->data_len <= 0) {
            return ESP_OK;
        }

        auto payload = static_cast<std::vector<unsigned char> *>(evt->user_data);
        if (payload == nullptr) {
            return ESP_FAIL;
        }

        // HTTP 返回的是 ESC/POS 原始字节流，逐块拼到内存后再统一校验发送
        const auto *data = static_cast<const unsigned char *>(evt->data);
        payload->insert(payload->end(), data, data + evt->data_len);
        return ESP_OK;
    }

    std::vector<unsigned char> DownloadPrintPayload(const std::string &url) {
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
            throw std::runtime_error("print_url 仅支持 http/https");
        }

        std::vector<unsigned char> payload;
        payload.reserve(64 * 1024);

        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.event_handler = HandlePrintUrlHttpEvent;
        config.user_data = &payload;
        config.timeout_ms = 20000;
        config.buffer_size = 1024;

        ESP_LOGI(TAG, "开始下载打印数据: url=%s", url.c_str());
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == nullptr) {
            throw std::runtime_error("HTTP 客户端初始化失败");
        }

        esp_err_t err = esp_http_client_perform(client);
        int status_code = esp_http_client_get_status_code(client);
        int64_t content_length = esp_http_client_get_content_length(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK) {
            throw std::runtime_error("下载打印数据失败: " + std::to_string(static_cast<int>(err)));
        }
        if (status_code < 200 || status_code >= 300) {
            throw std::runtime_error("下载打印数据 HTTP 状态异常: " + std::to_string(status_code));
        }
        if (payload.empty()) {
            throw std::runtime_error("下载打印数据为空");
        }
        if (content_length > 0 && static_cast<int64_t>(payload.size()) != content_length) {
            throw std::runtime_error("下载打印数据长度不完整");
        }
        if (payload.size() > PRINTER_BASE64_MAX_CHARS) {
            throw std::runtime_error("下载打印数据过大，已超过设备限制");
        }

        ESP_LOGI(TAG,
                 "打印数据下载完成: bytes=%u content_length=%lld status=%d",
                 static_cast<unsigned>(payload.size()),
                 static_cast<long long>(content_length),
                 status_code);
        return payload;
    }

    // 把 "1B 40 12 54" / "1B401254" 这类十六进制文本解析成字节
    static std::vector<unsigned char> ParseHexBytes(const std::string &text) {
        std::vector<unsigned char> bytes;
        int high = -1;
        for (char c : text) {
            int digit;
            if (c >= '0' && c <= '9') {
                digit = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                digit = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                digit = c - 'A' + 10;
            } else if (c == ' ' || c == ',' || c == '\n' || c == '\r' || c == '\t' || c == ':' || c == '-') {
                continue;
            } else {
                throw std::runtime_error(std::string("hex 含非法字符: ") + c);
            }

            if (high < 0) {
                high = digit;
            } else {
                bytes.push_back(static_cast<unsigned char>((high << 4) | digit));
                high = -1;
            }
        }
        if (high >= 0) {
            throw std::runtime_error("hex 字符个数为奇数");
        }
        if (bytes.empty()) {
            throw std::runtime_error("hex 数据为空");
        }
        return bytes;
    }

    void InitializePowerManager() { power_manager_ = new PowerManager(BATTERY_LEVEL_PIN, BATTERY_CHARGING_PIN); }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            if (GetNetworkType() == NetworkType::WIFI) {
                if (app.GetDeviceState() == kDeviceStateStarting ||
                    app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                    // cast to WifiBoard
                    auto &wifi_board = static_cast<WifiBoard &>(GetCurrentBoard());
                    wifi_board.EnterWifiConfigMode();
                }
            }
            app.ToggleChatState();
        });

        boot_button_.OnLongPress([this]() {
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting || app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                SwitchNetworkType();
            }
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeTools() {
        // 初始化打印串口：UART2 + IO5(TX)/IO6(RX)
        printer_tool_ = new SerialPrinterTool(PRINTER_UART_PORT,
                                              PRINTER_UART_TX_PIN,
                                              PRINTER_UART_RX_PIN,
                                              PRINTER_UART_BAUD_RATE,
                                              PRINTER_BASE64_MAX_CHARS,
                                              PRINTER_FEED_LINES_AFTER_PRINT);

        std::string init_error;
        if (!printer_tool_->Initialize(&init_error)) {
            ESP_LOGE(TAG, "打印串口初始化失败: %s", init_error.c_str());
        }

        auto &mcp_server = McpServer::GetInstance();
        // 先让服务端读取参数画像，确认型号/纸宽/波特率/板型
        mcp_server.AddTool(
            "self.printer.get_profile",
            "获取当前打印链路参数。服务端下发打印前先调用此工具，确认型号、纸宽、波特率和板型一致。",
            PropertyList(),
            [this](const PropertyList &) -> ReturnValue {
                cJSON *result = cJSON_CreateObject();
                cJSON_AddStringToObject(result, "board", BOARD_TYPE);
                cJSON_AddStringToObject(result, "printer_model", PRINTER_MODEL);
                cJSON_AddNumberToObject(result, "paper_width_mm", PRINTER_PAPER_WIDTH_MM);
                cJSON_AddNumberToObject(result, "baud_rate", PRINTER_UART_BAUD_RATE);
                cJSON_AddStringToObject(result, "encoding", "base64");
                cJSON_AddNumberToObject(result, "tx_pin", PRINTER_UART_TX_PIN);
                cJSON_AddNumberToObject(result, "rx_pin", PRINTER_UART_RX_PIN);
                cJSON_AddBoolToObject(result, "ready", printer_tool_ != nullptr && printer_tool_->IsReady());
                if (printer_tool_ != nullptr && !printer_tool_->IsReady()) {
                    cJSON_AddStringToObject(result, "error", printer_tool_->GetInitError().c_str());
                }
                return result;
            });

        // 仅接受匹配参数的打印任务，避免发错规格
        mcp_server.AddTool(
            "self.printer.send_base64",
            "向 638 热敏打印机发送 base64 打印流。仅允许 80mm、9600、ai-vox3 的任务，参数不匹配将拒绝执行。",
            PropertyList({
                Property("data", kPropertyTypeString),
                Property("printer_model", kPropertyTypeString),
                Property("paper_width_mm", kPropertyTypeInteger),
                Property("baud_rate", kPropertyTypeInteger),
                Property("target_board", kPropertyTypeString),
            }),
            [this](const PropertyList &properties) -> ReturnValue {
                if (printer_tool_ == nullptr) {
                    throw std::runtime_error("打印工具未初始化");
                }

                auto printer_model = properties["printer_model"].value<std::string>();
                auto paper_width_mm = properties["paper_width_mm"].value<int>();
                auto baud_rate = properties["baud_rate"].value<int>();
                auto target_board = properties["target_board"].value<std::string>();
                auto data = properties["data"].value<std::string>();
                ESP_LOGI(TAG,
                         "send_base64 参数: data_len=%u printer_model=%s paper_width_mm=%d baud_rate=%d target_board=%s",
                         static_cast<unsigned>(data.size()),
                         printer_model.c_str(),
                         paper_width_mm,
                         baud_rate,
                         target_board.c_str());
                // 严格校验服务端下发参数
                if (printer_model != PRINTER_MODEL) {
                    throw std::runtime_error("printer_model 不匹配");
                }
                if (paper_width_mm != PRINTER_PAPER_WIDTH_MM) {
                    throw std::runtime_error("paper_width_mm 不匹配");
                }
                if (baud_rate != PRINTER_UART_BAUD_RATE) {
                    throw std::runtime_error("baud_rate 不匹配");
                }
                if (target_board != BOARD_TYPE) {
                    throw std::runtime_error("target_board 不匹配");
                }

                int bytes = printer_tool_->SendBase64(data);
                cJSON *result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "ok", true);
                cJSON_AddNumberToObject(result, "bytes", bytes);
                return result;
            });

        // 通过 URL 下载完整 ESC/POS 图片打印流，避免大 base64 被服务端改写
        mcp_server.AddTool(
            "self.printer.print_url",
            "从 URL 下载 638 热敏打印机 ESC/POS 原始打印流并发送。仅允许 80mm、9600、ai-vox3 的任务，URL 内容必须是 application/octet-stream 打印字节流。",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("printer_model", kPropertyTypeString),
                Property("paper_width_mm", kPropertyTypeInteger),
                Property("baud_rate", kPropertyTypeInteger),
                Property("target_board", kPropertyTypeString),
            }),
            [this](const PropertyList &properties) -> ReturnValue {
                if (printer_tool_ == nullptr) {
                    throw std::runtime_error("打印工具未初始化");
                }

                auto url = properties["url"].value<std::string>();
                auto printer_model = properties["printer_model"].value<std::string>();
                auto paper_width_mm = properties["paper_width_mm"].value<int>();
                auto baud_rate = properties["baud_rate"].value<int>();
                auto target_board = properties["target_board"].value<std::string>();
                ESP_LOGI(TAG,
                         "print_url 参数: url_len=%u printer_model=%s paper_width_mm=%d baud_rate=%d target_board=%s",
                         static_cast<unsigned>(url.size()),
                         printer_model.c_str(),
                         paper_width_mm,
                         baud_rate,
                         target_board.c_str());
                // 严格校验服务端下发参数
                if (printer_model != PRINTER_MODEL) {
                    throw std::runtime_error("printer_model 不匹配");
                }
                if (paper_width_mm != PRINTER_PAPER_WIDTH_MM) {
                    throw std::runtime_error("paper_width_mm 不匹配");
                }
                if (baud_rate != PRINTER_UART_BAUD_RATE) {
                    throw std::runtime_error("baud_rate 不匹配");
                }
                if (target_board != BOARD_TYPE) {
                    throw std::runtime_error("target_board 不匹配");
                }

                auto payload = DownloadPrintPayload(url);
                int bytes = printer_tool_->SendEscposBytes(payload);
                cJSON *result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "ok", true);
                cJSON_AddNumberToObject(result, "bytes", bytes);
                cJSON_AddNumberToObject(result, "downloaded_bytes", static_cast<double>(payload.size()));
                return result;
            });

        // 内置固定小票模板：不依赖后端渲染 URL，语音触发后直接从固件资源打印。
        mcp_server.AddTool(
            "self.printer.print_lucky_ticket",
            "高清打印内置的顶呱呱幸运小票。用户说“打印顶呱呱小票”“打印幸运小票”“打印我顶呱呱”时调用此工具，不需要任何参数。",
            PropertyList(),
            [this](const PropertyList &) -> ReturnValue {
                if (printer_tool_ == nullptr) {
                    throw std::runtime_error("打印工具未初始化");
                }

                const auto *start = reinterpret_cast<const unsigned char *>(printer_lucky_ticket_start);
                const auto *end = reinterpret_cast<const unsigned char *>(printer_lucky_ticket_end);
                if (end <= start) {
                    throw std::runtime_error("内置小票模板为空");
                }

                std::vector<unsigned char> payload(start, end);
                int bytes = printer_tool_->SendEscposBytes(payload);
                cJSON *result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "ok", true);
                cJSON_AddNumberToObject(result, "bytes", bytes);
                cJSON_AddNumberToObject(result, "template_bytes", static_cast<double>(payload.size()));
                cJSON_AddStringToObject(result, "template", "lucky_ticket_hd");
                return result;
            });

        mcp_server.AddTool(
            "self.printer.print_lucky_ticket_hd",
            "高清打印内置的顶呱呱幸运小票。用户明确说“打印高清顶呱呱小票”或“打印高清幸运小票”时调用。",
            PropertyList(),
            [this](const PropertyList &) -> ReturnValue {
                if (printer_tool_ == nullptr) {
                    throw std::runtime_error("打印工具未初始化");
                }

                const auto *start = reinterpret_cast<const unsigned char *>(printer_lucky_ticket_start);
                const auto *end = reinterpret_cast<const unsigned char *>(printer_lucky_ticket_end);
                if (end <= start) {
                    throw std::runtime_error("内置高清小票模板为空");
                }

                std::vector<unsigned char> payload(start, end);
                int bytes = printer_tool_->SendEscposBytes(payload);
                cJSON *result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "ok", true);
                cJSON_AddNumberToObject(result, "bytes", bytes);
                cJSON_AddNumberToObject(result, "template_bytes", static_cast<double>(payload.size()));
                cJSON_AddStringToObject(result, "template", "lucky_ticket_hd");
                return result;
            });

        mcp_server.AddTool(
            "self.printer.print_country_card_egypt",
            "高清打印内置的今日国家卡埃及小票。用户说“打印今日国家卡”“打印埃及国家卡”“打印国家卡埃及”时调用，不需要任何参数。",
            PropertyList(),
            [this](const PropertyList &) -> ReturnValue {
                if (printer_tool_ == nullptr) {
                    throw std::runtime_error("打印工具未初始化");
                }

                const auto *start = reinterpret_cast<const unsigned char *>(printer_country_card_egypt_start);
                const auto *end = reinterpret_cast<const unsigned char *>(printer_country_card_egypt_end);
                if (end <= start) {
                    throw std::runtime_error("内置埃及国家卡模板为空");
                }

                std::vector<unsigned char> payload(start, end);
                int bytes = printer_tool_->SendEscposBytes(payload);
                cJSON *result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "ok", true);
                cJSON_AddNumberToObject(result, "bytes", bytes);
                cJSON_AddNumberToObject(result, "template_bytes", static_cast<double>(payload.size()));
                cJSON_AddStringToObject(result, "template", "country_card_egypt");
                return result;
            });

        mcp_server.AddTool(
            "self.printer.print_wealth_ticket",
            "高清打印内置的先别慌先发财小票。用户说“打印先别慌先发财”“打印发财小票”“打印螃蟹发财小票”时调用，不需要任何参数。",
            PropertyList(),
            [this](const PropertyList &) -> ReturnValue {
                if (printer_tool_ == nullptr) {
                    throw std::runtime_error("打印工具未初始化");
                }

                const auto *start = reinterpret_cast<const unsigned char *>(printer_wealth_ticket_start);
                const auto *end = reinterpret_cast<const unsigned char *>(printer_wealth_ticket_end);
                if (end <= start) {
                    throw std::runtime_error("内置发财小票模板为空");
                }

                std::vector<unsigned char> payload(start, end);
                int bytes = printer_tool_->SendEscposBytes(payload);
                cJSON *result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "ok", true);
                cJSON_AddNumberToObject(result, "bytes", bytes);
                cJSON_AddNumberToObject(result, "template_bytes", static_cast<double>(payload.size()));
                cJSON_AddStringToObject(result, "template", "wealth_ticket");
                return result;
            });

        // 打印机自检页：内容由打印机固件生成，用于独立验证 TX 接线与波特率
        mcp_server.AddTool(
            "self.printer.selftest",
            "让打印机打印自检页（含程序版本、接口类型、波特率）。用于诊断串口链路，不依赖任何打印内容构造逻辑。",
            PropertyList(),
            [this](const PropertyList &) -> ReturnValue {
                if (printer_tool_ == nullptr) {
                    throw std::runtime_error("打印工具未初始化");
                }

                int bytes = printer_tool_->SendSelfTest();
                cJSON *result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "ok", true);
                cJSON_AddNumberToObject(result, "bytes", bytes);
                cJSON_AddStringToObject(result, "hint", "若未出纸，说明字节未到达打印机：检查 TX/RX 是否交叉、GND 是否共地、波特率是否为 9600");
                return result;
            });

        // 实时状态查询：唯一能证明打印机真的在线的手段
        mcp_server.AddTool(
            "self.printer.get_status",
            "查询打印机实时状态（DLE EOT 1-4），返回是否在线、缺纸、上盖开等。responded=false 说明打印机没有回传，链路不通。",
            PropertyList(),
            [this](const PropertyList &) -> ReturnValue {
                if (printer_tool_ == nullptr) {
                    throw std::runtime_error("打印工具未初始化");
                }

                cJSON *result = cJSON_CreateObject();
                cJSON *raw = cJSON_CreateObject();
                bool any_responded = false;
                int offline_status = -1;

                for (int n = 1; n <= 4; ++n) {
                    int value = printer_tool_->QueryRealtimeStatus(n);
                    std::string key = "n" + std::to_string(n);
                    if (value < 0) {
                        cJSON_AddNullToObject(raw, key.c_str());
                        continue;
                    }
                    any_responded = true;
                    cJSON_AddNumberToObject(raw, key.c_str(), value);
                    if (n == 2) {
                        offline_status = value;
                    }
                }

                cJSON_AddBoolToObject(result, "responded", any_responded);
                cJSON_AddItemToObject(result, "raw", raw);

                if (!any_responded) {
                    cJSON_AddStringToObject(
                        result, "diagnosis",
                        "打印机无任何回传。可能原因：打印机 TX 未接到 ESP32 IO6、GND 未共地、波特率不匹配，或打印机未上电");
                } else if (offline_status >= 0) {
                    // 规格书 DLE EOT 2 脱机状态位定义
                    cJSON_AddBoolToObject(result, "cover_open", (offline_status & 0x04) != 0);
                    cJSON_AddBoolToObject(result, "feed_button_pressed", (offline_status & 0x08) != 0);
                    cJSON_AddBoolToObject(result, "paper_end", (offline_status & 0x20) != 0);
                    cJSON_AddBoolToObject(result, "error", (offline_status & 0x40) != 0);
                }
                return result;
            });

        // 原始 hex 透传：可发送任意字节序列，不追加走纸、不做光栅校验。
        // 用于在不重新编译固件的前提下验证厂商文档里的字节序列。
        mcp_server.AddTool(
            "self.printer.send_hex",
            "把十六进制字符串解析成字节原样发给打印机，不追加任何走纸命令。用于验证厂商文档字节序列，例如自测页 1B4012 54。空格和换行会被忽略。",
            PropertyList({
                Property("hex", kPropertyTypeString),
            }),
            [this](const PropertyList &properties) -> ReturnValue {
                if (printer_tool_ == nullptr) {
                    throw std::runtime_error("打印工具未初始化");
                }

                auto hex = properties["hex"].value<std::string>();
                auto payload = ParseHexBytes(hex);
                ESP_LOGI(TAG, "send_hex: chars=%u bytes=%u",
                         static_cast<unsigned>(hex.size()),
                         static_cast<unsigned>(payload.size()));

                int bytes = printer_tool_->SendRawExact(payload);
                cJSON *result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "ok", true);
                cJSON_AddNumberToObject(result, "bytes", bytes);
                return result;
            });

        // 标签模式文本打印：按厂商标签指令集构造 1A 系列命令
        mcp_server.AddTool(
            "self.printer.print_label_text",
            "用标签指令集（1A 系列）打印文本。若 POS 模式的 print_text 不出纸，用这个测试打印机是否处于标签模式。当前仅支持 ASCII，中文需 GBK 编码暂不支持。",
            PropertyList({
                Property("text", kPropertyTypeString),
                Property("x", kPropertyTypeInteger, 10, 0, 2000),
                Property("y", kPropertyTypeInteger, 10, 0, 2000),
                Property("page_width", kPropertyTypeInteger, 576, 8, 2000),
                Property("page_height", kPropertyTypeInteger, 80, 8, 2000),
            }),
            [this](const PropertyList &properties) -> ReturnValue {
                if (printer_tool_ == nullptr) {
                    throw std::runtime_error("打印工具未初始化");
                }

                auto text = properties["text"].value<std::string>();
                int x = properties["x"].value<int>();
                int y = properties["y"].value<int>();
                int page_width = properties["page_width"].value<int>();
                int page_height = properties["page_height"].value<int>();

                std::vector<unsigned char> payload;
                auto push_u16 = [&payload](int value) {
                    payload.push_back(static_cast<unsigned char>(value & 0xFF));
                    payload.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
                };

                // ESC @ 初始化
                payload.push_back(0x1B);
                payload.push_back(0x40);
                // FS & 汉字模式 + ESC 9 00 切到 GBK 双字节编码（厂商文档写的是 00 而非 01）
                payload.push_back(0x1C);
                payload.push_back(0x26);
                payload.push_back(0x1B);
                payload.push_back(0x39);
                payload.push_back(0x00);
                // 1A 5B 01 页开始
                payload.push_back(0x1A);
                payload.push_back(0x5B);
                payload.push_back(0x01);
                push_u16(0);            // 页参考原点 x
                push_u16(0);            // 页参考原点 y
                push_u16(page_width);   // 页宽
                push_u16(page_height);  // 页高
                payload.push_back(0x00);  // rotate=0
                // 1A 54 00 文本对象（形式 a）
                payload.push_back(0x1A);
                payload.push_back(0x54);
                payload.push_back(0x00);
                push_u16(x);
                push_u16(y);
                payload.insert(payload.end(), text.begin(), text.end());
                payload.push_back(0x00);  // 字符串 0x00 结束
                // 1A 5D 00 页结束
                payload.push_back(0x1A);
                payload.push_back(0x5D);
                payload.push_back(0x00);
                // 1A 4F 00 页打印
                payload.push_back(0x1A);
                payload.push_back(0x4F);
                payload.push_back(0x00);

                ESP_LOGI(TAG, "print_label_text: text_len=%u page=%dx%d pos=(%d,%d) bytes=%u",
                         static_cast<unsigned>(text.size()), page_width, page_height, x, y,
                         static_cast<unsigned>(payload.size()));

                int bytes = printer_tool_->SendRawExact(payload);
                cJSON *result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "ok", true);
                cJSON_AddNumberToObject(result, "bytes", bytes);
                return result;
            });

        // 本地打印自测：直接打印 hello
        mcp_server.AddTool(
            "self.printer.test_hello",
            "直接通过串口打印 hello，用于本地链路测试。",
            PropertyList(),
            [this](const PropertyList &) -> ReturnValue {
                if (printer_tool_ == nullptr) {
                    throw std::runtime_error("打印工具未初始化");
                }

                int bytes = printer_tool_->SendRaw(BuildPrintPayload("hello"));
                cJSON *result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "ok", true);
                cJSON_AddNumberToObject(result, "bytes", bytes);
                return result;
            });

        // MY-638 接收 MCP 文本时，JSON 字符串本身是 UTF-8；统一切换打印机到
        // UTF-8 双字节模式，避免中文被当成单字节代码页解析后出现乱码。
        mcp_server.AddTool(
            "self.printer.print_text",
            "直接通过串口打印 UTF-8 中英文文本。支持中文、英文、数字和常用标点。",
            PropertyList({
                Property("text", kPropertyTypeString),
                Property("font_size", kPropertyTypeInteger, PRINTER_TEXT_FONT_SIZE_DEFAULT, 1, 3),
            }),
            [this](const PropertyList &properties) -> ReturnValue {
                if (printer_tool_ == nullptr) {
                    throw std::runtime_error("打印工具未初始化");
                }

                auto text = properties["text"].value<std::string>();
                auto font_size = properties["font_size"].value<int>();
                int bytes = printer_tool_->SendRaw(BuildPrintPayload(text, true, font_size));
                cJSON *result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "ok", true);
                cJSON_AddNumberToObject(result, "bytes", bytes);
                return result;
            });

        // 直接打印 UTF-8 文本，依赖打印机支持 UTF-8 双字节编码
        mcp_server.AddTool(
            "self.printer.print_utf8_text",
            "直接通过串口打印 UTF-8 文本。用于测试 638 内置中文打印能力。",
            PropertyList({
                Property("text", kPropertyTypeString),
                Property("font_size", kPropertyTypeInteger, PRINTER_TEXT_FONT_SIZE_DEFAULT, 1, 3),
            }),
            [this](const PropertyList &properties) -> ReturnValue {
                if (printer_tool_ == nullptr) {
                    throw std::runtime_error("打印工具未初始化");
                }

                auto text = properties["text"].value<std::string>();
                auto font_size = properties["font_size"].value<int>();
                int bytes = printer_tool_->SendRaw(BuildPrintPayload(text, true, font_size));
                cJSON *result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "ok", true);
                cJSON_AddNumberToObject(result, "bytes", bytes);
                return result;
            });
    }

  public:
    AIVOX3()
        : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC, 0), boot_button_(BOOT_BUTTON_GPIO),
          volume_up_button_(VOLUME_UP_BUTTON_GPIO), volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializePowerManager();
        InitializeButtons();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    virtual Led *GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec *GetAudioCodec() override {
        static AIVOX3AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE,
                                            AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK,
                                            AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
                                            AUDIO_CODEC_ES8311_ADDR, AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display *GetDisplay() override { return display_; }

    virtual Backlight *GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        level = power_manager_->GetBatteryLevel();
        return true;
    }
};

DECLARE_BOARD(AIVOX3);
