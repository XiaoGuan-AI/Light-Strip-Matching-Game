# ESP WiFi Connect - AI Printer 定制版

本组件是 [78/esp-wifi-connect](https://components.espressif.com/components/78/esp-wifi-connect) 的本地定制版本，包含 AI Printer 项目的自定义 WiFi 配置 UI。

## 为什么使用本地组件？

ESP-IDF 组件管理器会在构建时下载/更新 `managed_components` 目录下的组件，这会覆盖我们对 HTML 文件的修改。通过创建本地组件，我们可以：

1. **持久化自定义 UI** - 不会被组件更新覆盖
2. **版本控制** - 自定义的 HTML 文件纳入 Git 管理
3. **优先级更高** - ESP-IDF 优先使用 `components` 目录下的本地组件

## 文件结构

```
esp-wifi-connect/
├── CMakeLists.txt              # 组件构建配置
├── README.md                   # 本文件
├── assets/
│   ├── wifi_configuration.html      # 自定义 WiFi 配置页面
│   └── wifi_configuration_done.html # 自定义配置完成页面
├── include/
│   ├── dns_server.h
│   ├── ssid_manager.h
│   ├── wifi_configuration_ap.h
│   └── wifi_station.h
├── dns_server.cc
├── ssid_manager.cc
├── wifi_configuration_ap.cc
└── wifi_station.cc
```

## 自定义内容

### WiFi 配置页面 (`wifi_configuration.html`)

- 🎨 深色主题 + 玻璃态设计
- 🏷️ AI Printer 品牌标识
- ✨ 现代化 UI 组件
- 🌐 中英文双语支持

### 配置完成页面 (`wifi_configuration_done.html`)

- ✅ 动画成功图标
- ⏱️ 倒计时重启提示
- 🎨 与主页面风格一致

## 品牌信息

- **产品名称**: AI Printer
- **开发团队**: Supie AI-Lab

## 更新说明

如需更新上游组件的功能代码，请手动合并：

1. 下载最新版 78/esp-wifi-connect
2. 比较 `.cc` 和 `.h` 文件的变更
3. 手动合并必要的功能更新
4. **保留** 自定义的 HTML 文件

## 版本

- 基于上游版本: 2.3.2
- 定制版本: 1.0.0
- 最后更新: 2026-01-09
