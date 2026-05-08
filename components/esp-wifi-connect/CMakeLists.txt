# AI Printer 自定义 WiFi 配置组件
# 覆盖 managed_components 中的 78__esp-wifi-connect
# 包含自定义 UI 设计的 WiFi 配置页面

idf_component_register(
    SRCS
        "wifi_configuration_ap.cc"
        "wifi_station.cc"
        "ssid_manager.cc"
        "dns_server.cc"
    INCLUDE_DIRS
        "include"
    EMBED_TXTFILES
        "assets/wifi_configuration.html"
        "assets/wifi_configuration_done.html"
    REQUIRES
    	"esp_timer"
        "esp_http_server"
        "esp_wifi"
        "nvs_flash"
        "json"
)
