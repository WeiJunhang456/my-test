// 如果当前是 C++ 编译器，则开始 extern "C" 包裹
// 作用：告诉 C++ 编译器，大括号内的函数使用 C 语言的链接方式
#ifdef __cplusplus
extern "C" {
#endif

// 包含标准 C 字符串库头文件
// 作用：提供 strcpy, strlen, sscanf, memset 等字符串操作函数
#include <string.h>
// 包含 FreeRTOS 核心头文件
// 作用：提供 FreeRTOS 基础数据类型和宏定义
#include "freertos/FreeRTOS.h"
// 包含 FreeRTOS 任务管理头文件
// 作用：提供任务创建、延时、删除等函数
#include "freertos/task.h"
// 包含 FreeRTOS 事件组头文件
// 作用：提供事件组创建、等待、设置位等函数
#include "freertos/event_groups.h"
// 包含 ESP32 系统接口头文件
// 作用：提供芯片重启、系统信息获取等函数
#include "esp_system.h"
// 包含 ESP32 WiFi 驱动头文件
// 作用：提供 WiFi 初始化、连接、模式设置等函数
#include "esp_wifi.h"
// 包含 ESP32 事件循环头文件
// 作用：提供事件注册、事件回调处理等函数
#include "esp_event.h"
// 包含 ESP32 日志输出头文件
// 作用：提供 ESP_LOGI, ESP_LOGE 等日志打印宏
#include "esp_log.h"
// 包含 NVS (非易失性存储) 闪存操作头文件
// 作用：提供 NVS 初始化、读写、擦除等函数
#include "nvs_flash.h"
// 包含 ESP32 外设驱动头文件 (这里用于串口)
// 作用：提供 UART 初始化、读写、引脚配置等函数
#include "driver/uart.h"
// 包含 ESP32 网络接口管理头文件 (新版 TCP/IP 栈)
// 作用：替代旧版 tcpip_adapter，管理网络接口
#include "esp_netif.h"
// 包含 ESP32 MQTT 客户端头文件
// 作用：提供 MQTT 客户端创建、连接、发布订阅等函数
#include "mqtt_client.h"
// 包含 ESP32 HTTP 服务器头文件
// 作用：提供轻量级 HTTP 服务器功能
#include "esp_http_server.h"

// 如果当前是 C++ 编译器，则结束 extern "C" 包裹
#ifdef __cplusplus
}
#endif

// ===================== 【配置区】宏定义 =====================
// 定义 NVS 分区的命名空间名称，用于隔离不同应用的数据
#define NVS_NAMESPACE        "wifi_storage"
// 定义存储 WiFi 名称 (SSID) 的键名
#define NVS_KEY_SSID         "ssid"
// 定义存储 WiFi 密码的键名
#define NVS_KEY_PASS         "password"
// 定义事件组中的 Bit 0，用于标记 WiFi 连接成功
#define WIFI_CONNECTED_BIT   BIT0
// 定义与 STM32 通信使用的串口号 (使用 UART1)
#define STM32_UART_NUM       UART_NUM_1
// 定义 ESP32 串口发送引脚 (GPIO 17)
#define STM32_UART_TX_PIN    17
// 定义 ESP32 串口接收引脚 (GPIO 16)
#define STM32_UART_RX_PIN    16
// 定义串口通信波特率
#define UART_BAUD_RATE       115200
// 定义串口数据缓冲区大小 (1024字节)
#define UART_BUF_SIZE        1024
// 定义 MQTT 服务器地址 (URI格式)
#define MQTT_BROKER          "mqtt://test.mosquitto.org"
// 定义 MQTT 订阅的主题 (用于接收云端控制指令)
#define MQTT_SUB_TOPIC       "esp32/stm32/control"
// 定义 MQTT 发布的主题 (用于上报数据到云端)
#define MQTT_PUB_TOPIC       "esp32/stm32/data"
// 定义 AP 配网模式下的热点名称
#define AP_SSID              "ESP32_Config"

// ===================== 【全局变量区】=====================
// 定义日志输出的标签字符串，便于在串口监视器中过滤
static const char *TAG = "ESP32_OK";
// 定义事件组句柄，这是一个全局变量，用于在任务和中断间传递状态
static EventGroupHandle_t s_wifi_event_group;
// 定义 MQTT 客户端句柄
static esp_mqtt_client_handle_t s_mqtt_client;
// 定义一个布尔变量，标记互联网是否正常 (简化版逻辑)
static bool g_internet_ok = false;

// ===================== 【函数1】NVS 闪存初始化 =====================
static void nvs_init(void) {
    // 调用 NVS 初始化函数，并将返回的错误码存入变量 err
    esp_err_t err = nvs_flash_init();
    // 检查错误码：如果是“没有空闲页”或者“版本不兼容”
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 擦除整个 NVS 分区，进行初始化修复
        nvs_flash_erase();
        // 再次尝试初始化 NVS
        err = nvs_flash_init();
    }
    // 打印 Info 级别日志：NVS 初始化完成
    ESP_LOGI(TAG, "✅ NVS Init Done");
}

// ===================== 【函数2】从 NVS 读取 WiFi 配置 =====================
// 输入参数：ssid-指向存储WiFi名的字符数组缓冲区, pwd-指向存储密码的字符数组缓冲区
// 返回值：true-读取成功, false-读取失败(无配置)
static bool nvs_read_wifi(char *ssid, char *pwd) {
    // 定义 NVS 操作句柄变量
    nvs_handle_t handle;
    // 以只读模式打开 NVS 分区，如果打开失败(返回值不是OK)，直接返回 false
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    // 定义一个变量存储 SSID 的最大长度 (32字节)
    size_t ssid_len = 32;
    // 定义一个变量存储密码的最大长度 (64字节)
    size_t pwd_len = 64;

    // 从 NVS 中读取字符串类型的 SSID
    if (nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_len) != ESP_OK) {
        // 如果读取 SSID 失败，先关闭 NVS 句柄
        nvs_close(handle);
        // 返回 false 表示没有读到有效配置
        return false;
    }
    // 从 NVS 中读取字符串类型的密码
    if (nvs_get_str(handle, NVS_KEY_PASS, pwd, &pwd_len) != ESP_OK) {
        // 如果读取密码失败，先关闭 NVS 句柄
        nvs_close(handle);
        // 返回 false
        return false;
    }

    // 读取成功，关闭 NVS 句柄以释放资源
    nvs_close(handle);
    // 打印 Info 日志，输出读到的 WiFi 名称
    ESP_LOGI(TAG, "✅ Read WiFi: %s", ssid);
    // 返回 true 表示读取成功
    return true;
}

// ===================== 【函数3】保存 WiFi 配置到 NVS =====================
static void nvs_save_wifi(const char *ssid, const char *pwd) {
    // 定义 NVS 操作句柄变量
    nvs_handle_t handle;
    // 以读写模式打开 NVS 分区
    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    // 将 SSID 字符串写入 NVS 分区
    nvs_set_str(handle, NVS_KEY_SSID, ssid);
    // 将密码字符串写入 NVS 分区
    nvs_set_str(handle, NVS_KEY_PASS, pwd);
    // 提交更改 (必须调用，否则断电后数据不会保存)
    nvs_commit(handle);
    // 关闭 NVS 句柄
    nvs_close(handle);
    // 打印 Info 日志：保存成功
    ESP_LOGI(TAG, "✅ WiFi Saved");
}

// ===================== 【函数7】WiFi 事件回调处理函数 =====================
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    // 判断事件基础类型是否为 WiFi 事件
    if (event_base == WIFI_EVENT) {
        // 使用 switch 语句判断具体的 WiFi 事件 ID
        switch(event_id) {
            // 事件：WiFi Station 模式启动完成
            case WIFI_EVENT_STA_START:
                // 调用 WiFi 连接函数，开始尝试连接路由器
                esp_wifi_connect();
                // 跳出 switch 语句
                break;
            // 事件：WiFi 断开连接
            case WIFI_EVENT_STA_DISCONNECTED:
                // 清除事件组中的"连接成功"标志位
                xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
                // 调用 WiFi 连接函数，尝试自动重连
                esp_wifi_connect();
                // 打印 Warning 级别日志：正在重连
                ESP_LOGW(TAG, "WiFi Reconnecting...");
                // 跳出 switch 语句
                break;
            // 其他未列出的 WiFi 事件
            default:
                // 不做任何处理
                break;
        }
    }
}

// ===================== 【函数7-1】IP 事件回调处理函数 =====================
static void ip_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data) {
    // 判断事件基础类型是否为 IP 事件 且 具体事件 ID 为"成功获取到 IP 地址"
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // 设置事件组中的"连接成功"标志位 (置1)
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        // 将全局网络状态标记置为 true
        g_internet_ok = true;
        // 打印 Info 日志：WiFi 连接成功
        ESP_LOGI(TAG, "✅ WiFi Connected!");
    }
}

// ===================== 【函数8】Station 模式初始化 (连接路由器) =====================
static bool wifi_sta_init(void) {
    // 定义字符数组用于存储读取到的 SSID
    char ssid[32];
    // 定义字符数组用于存储读取到的密码
    char pwd[64];
    // 调用函数从 NVS 读取 WiFi 配置，如果读取失败直接返回 false
    if (!nvs_read_wifi(ssid, pwd)) return false;

    // 创建一个 FreeRTOS 事件组，并赋值给全局句柄
    s_wifi_event_group = xEventGroupCreate();

    // 初始化 ESP-NETIF (底层网络接口栈)
    // ESP_ERROR_CHECK 用于检查返回值，如果报错会直接终止程序并打印
    ESP_ERROR_CHECK(esp_netif_init());
    // 创建系统默认事件循环 (用于处理 WiFi 和 IP 事件)
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // 创建默认的 WiFi Station 网络接口对象 (绑定 WiFi 和 TCP/IP)
    esp_netif_create_default_wifi_sta();

    // 注册 WiFi 事件处理器，监听所有 WiFi 事件
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    // 注册 IP 事件处理器，只监听"获取到 IP"事件
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL);

    // 获取 WiFi 初始化默认配置
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // 使用配置初始化 WiFi 驱动
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 定义 WiFi 配置结构体变量，并使用聚合初始化将所有成员清零
    wifi_config_t wifi_cfg = {};
    // 将 SSID 字符串拷贝到配置结构体的 Station 字段中
    strcpy((char*)wifi_cfg.sta.ssid, ssid);
    // 将密码字符串拷贝到配置结构体的 Station 字段中
    strcpy((char*)wifi_cfg.sta.password, pwd);

    // 设置 WiFi 工作模式为 Station (客户端) 模式
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // 将配置加载到 WiFi 驱动中
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg)); 
    // 启动 WiFi 硬件
    ESP_ERROR_CHECK(esp_wifi_start());

    // 阻塞式等待事件组中的"连接成功"位被置1，portMAX_DELAY表示永久等待
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    // 返回 true 表示初始化并连接成功
    return true;
}

// ===================== 【函数9】HTTP 服务器回调：返回配网首页 =====================
static esp_err_t root_handler(httpd_req_t *req) {
    // 定义一个字符串常量，存储简易的 HTML 配网网页代码
    const char *web_html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'></head>"
        "<body style='text-align:center;margin-top:60px;'>"
        "<h2>ESP32 Config</h2>"
        "<form action='/save' method='POST'>"
        "<p>SSID: <input type='text' name='ssid' required></p>"
        "<p>PASS: <input type='password' name='pass' required></p>"
        "<p><button type='submit'>Save</button></p>"
        "</form></body></html>";
    // 调用 HTTP 响应发送函数，将网页内容发送给连接的浏览器
    // HTTPD_RESP_USE_STRLEN 表示自动计算字符串长度
    httpd_resp_send(req, web_html, HTTPD_RESP_USE_STRLEN);
    // 返回执行成功
    return ESP_OK;
}

// ===================== 【函数10】HTTP 服务器回调：保存配网信息 =====================
static esp_err_t save_wifi_handler(httpd_req_t *req) {
    // 定义缓冲区数组，并初始化为0，用于接收浏览器发来的 POST 表单数据
    char buf[128] = {0};
    // 接收 HTTP POST 请求体数据
    httpd_req_recv(req, buf, sizeof(buf));
    
    // 定义数组存储解析出的 SSID，并初始化为0
    char ssid[32] = {0};
    // 定义数组存储解析出的密码，并初始化为0
    char pass[64] = {0};
    // 使用 sscanf 解析 URL 编码的表单数据 (格式类似: ssid=MyWiFi&pass=123456)
    // %[^&] 表示读取直到遇到 & 符号为止
    sscanf(buf, "ssid=%[^&]&pass=%s", ssid, pass);

    // 调用函数将解析出的 WiFi 信息保存到 NVS
    nvs_save_wifi(ssid, pass);

    // 定义配网成功提示页面的 HTML 代码
    const char *success_html = "<h2>Success! Restarting...</h2>";
    // 发送成功页面给浏览器
    httpd_resp_send(req, success_html, HTTPD_RESP_USE_STRLEN);

    // 延时 1 秒，让浏览器有时间显示成功页面
    vTaskDelay(pdMS_TO_TICKS(1000));
    // 调用函数重启 ESP32 芯片
    esp_restart();
    // 返回执行成功 (虽然重启后不会执行到这里，但语法上需要返回)
    return ESP_OK;
}

// ===================== 【函数11】启动 HTTP 服务器 =====================
static void start_http_server(void) {
    // 定义 HTTP 服务器句柄，并初始化为空
    httpd_handle_t server = NULL;
    // 获取 HTTP 服务器默认配置
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    // 定义 URI 路由结构 (访问根目录 "/")
    httpd_uri_t uri_root = {.uri="/", .method=HTTP_GET, .handler=root_handler, .user_ctx=NULL};
    // 定义 URI 路由结构 (访问 "/save")
    httpd_uri_t uri_save = {.uri="/save", .method=HTTP_POST, .handler=save_wifi_handler, .user_ctx=NULL};

    // 启动 HTTP 服务器，判断返回值是否成功
    if (httpd_start(&server, &http_cfg) == ESP_OK) {
        // 注册首页路由处理函数
        httpd_register_uri_handler(server, &uri_root);
        // 注册保存路由处理函数
        httpd_register_uri_handler(server, &uri_save);
        // 打印 Info 日志：服务器启动成功
        ESP_LOGI(TAG, "✅ HTTP Server Started");
    }
}

// ===================== 【函数12】AP 配网模式 (开启热点) =====================
static void start_ap_config(void) {
    // 打印 Warning 级别日志：提示进入配网模式
    ESP_LOGW(TAG, "Entering AP Mode...");

    // 初始化 ESP-NETIF 网络接口
    ESP_ERROR_CHECK(esp_netif_init());
    // 创建系统默认事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // 创建默认的 WiFi AP 网络接口对象
    // 注意：此函数内部默认就会将 IP 设置为 192.168.4.1，无需手动设置
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    // 获取 WiFi 初始化默认配置
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // 初始化 WiFi 驱动
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 定义 AP 模式配置结构体，并初始化为 0
    wifi_config_t ap_cfg = {};
    // 拷贝热点名称到配置结构体中
    strcpy((char*)ap_cfg.ap.ssid, AP_SSID);
    // 设置热点名称的实际长度
    ap_cfg.ap.ssid_len = strlen(AP_SSID);
    // 设置最大允许连接的设备数 (2台)
    ap_cfg.ap.max_connection = 2;
    // 设置加密模式为开放 (不设密码)
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    // 设置 WiFi 工作模式为 AP 模式
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    // 加载 AP 模式配置
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    // 启动 WiFi 硬件
    ESP_ERROR_CHECK(esp_wifi_start());

    // 调用函数启动 HTTP 配网网页服务器
    start_http_server();
}

// ===================== 【函数13】MQTT 事件回调处理 =====================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    // 将 void* 类型的事件数据强制转换为 MQTT 事件句柄类型
    // 这是为了修复 C++ 严格类型检查带来的编译错误
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    
    // 使用 if-else 替代 switch，避免枚举类型强转问题
    // 如果事件 ID 是"已连接"
    if (event_id == MQTT_EVENT_CONNECTED) {
        // 订阅指定的 MQTT 主题
        esp_mqtt_client_subscribe(s_mqtt_client, MQTT_SUB_TOPIC, 0);
        // 打印 Info 日志：MQTT 连接成功
        ESP_LOGI(TAG, "✅ MQTT Connected");
    } 
    // 如果事件 ID 是"收到数据"
    else if (event_id == MQTT_EVENT_DATA) {
        // 通过串口将收到的 MQTT 数据转发给 STM32
        uart_write_bytes(STM32_UART_NUM, event->data, event->data_len);
    }
}

// ===================== 【函数14】MQTT 客户端初始化 =====================
static void mqtt_init(void) {
    // 定义 MQTT 配置结构体，并初始化为 0
    esp_mqtt_client_config_t mqtt_cfg = {};
    
    // 配置 MQTT 服务器地址
    // 使用新版嵌套结构体写法 (兼容 espressif32@3.5.0 较新的子版本)
    mqtt_cfg.broker.address.uri = MQTT_BROKER;

    // 根据配置创建 MQTT 客户端实例，返回句柄
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    
    // 注册 MQTT 事件监听回调函数，监听所有事件
    esp_mqtt_client_register_event(s_mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    
    // 启动 MQTT 客户端 (开始自动连接服务器)
    esp_mqtt_client_start(s_mqtt_client);
}

// ===================== 【函数15】串口初始化 =====================
static void uart_init(void) {
    // 定义串口配置结构体，并初始化为 0
    uart_config_t uart_cfg = {};
    // 设置串口波特率
    uart_cfg.baud_rate = UART_BAUD_RATE;
    // 设置数据位为 8 位
    uart_cfg.data_bits = UART_DATA_8_BITS;
    // 设置校验位为无校验
    uart_cfg.parity = UART_PARITY_DISABLE;
    // 设置停止位为 1 位
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    // 设置硬件流控为关闭
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    // 设置源时钟为 APB 时钟
    uart_cfg.source_clk = UART_SCLK_APB;

    // 应用串口参数配置到指定串口
    uart_param_config(STM32_UART_NUM, &uart_cfg);
    // 设置串口使用的 GPIO 引脚 (TX, RX, RTS, CTS)
    uart_set_pin(STM32_UART_NUM, STM32_UART_TX_PIN, STM32_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    // 安装串口驱动，并分配发送/接收缓冲区
    uart_driver_install(STM32_UART_NUM, UART_BUF_SIZE, UART_BUF_SIZE, 0, NULL, 0);
    // 打印 Info 日志：串口初始化完成
    ESP_LOGI(TAG, "✅ UART Init Done");
}

// ===================== 【函数16】串口接收任务 (FreeRTOS 任务) =====================
static void uart_rx_task(void *arg) {
    // 定义数据缓冲区数组
    uint8_t data[UART_BUF_SIZE];
    // 进入死循环，任务通常都是死循环
    while(1) {
        // 读取串口数据，超时时间设置为 20ms
        // 返回值 len 是实际读到的字节数
        int len = uart_read_bytes(STM32_UART_NUM, data, UART_BUF_SIZE, pdMS_TO_TICKS(20));
        // 判断：如果读到的数据长度大于 0 且 互联网连接状态正常
        if (len > 0 && g_internet_ok) {
            // 通过 MQTT 发布消息，将串口收到的数据上报到云端
            esp_mqtt_client_publish(s_mqtt_client, MQTT_PUB_TOPIC, (char*)data, len, 0, 0);
            // 打印 Info 日志：数据已上报
            ESP_LOGI(TAG, "Data Uploaded");
        }
        // 延时 10ms，释放 CPU 资源给其他任务，防止任务卡死
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ===================== 【主函数】程序入口 =====================
// 使用 extern "C" 确保 app_main 函数使用 C 语言链接规范，不被 C++ 编译器改名
extern "C" void app_main(void) {
    // 1. 调用 NVS 初始化函数
    nvs_init();
    // 2. 调用串口初始化函数
    uart_init();

    // 核心逻辑判断
    // 尝试初始化 STA 模式，如果返回 false (说明 NVS 里没有保存的 WiFi)
    if (!wifi_sta_init()) {
        // 则启动 AP 配网模式，开启热点
        start_ap_config();
    } 
    // 如果 wifi_sta_init() 返回 true (说明成功连接上路由器了)
    else {
        // 3. 初始化 MQTT 客户端，连接云端
        mqtt_init();
        // 4. 创建一个 FreeRTOS 任务，用于持续监听串口数据
        // 参数：任务函数, 任务名, 栈大小, 参数, 优先级, 任务句柄
        xTaskCreate(uart_rx_task, "uart_rx_task", 2048, NULL, 5, NULL);
    }
    
    // 打印 Info 日志：系统启动完成
    ESP_LOGI(TAG, "🎉 System Start!");
}
