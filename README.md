# ESP32-S3 USB 打印机主机

本项目让 ESP32-S3 作为 USB Host，通过本机 Wi-Fi 热点接收目标打印机驱动生成的
原始 PRN 文件并发送给 USB 打印机。项目使用纯 ESP-IDF，不依赖 Arduino。

已验证硬件与协议：

- ESP32-S3 原生 USB：GPIO19=D−、GPIO20=D+；
- HP DeskJet 2300：VID/PID `03f0:3654`；
- Interface 1，协议 `07/01/02`；
- Bulk OUT `0x08`、Bulk IN `0x89`，Full-Speed，MPS 64；
- ESP-IDF v6.1、`espressif/usb` 1.5.0；
- 12,627 字节实测 PRN 完整发送并正常出纸。

## 目录

- [`docs/方案设计.md`](docs/方案设计.md)：硬件接线、安全供电、软件架构和排障。
- [`docs/开发计划.md`](docs/开发计划.md)：Wi-Fi 打印网关的需求基线、API、状态机、实施阶段和验收标准。
- [`hp2300_print_idf/`](hp2300_print_idf/README.md)：当前 Wi-Fi 打印网关固件及接口说明。

## 快速开始

打印文件可能包含文档内容、用户名和打印后台元数据，因此仓库不包含测试使用的
`1.prn`，并通过 `.gitignore` 排除全部 `.prn` 文件。

1. 按方案文档完成带限流和防回灌能力的 USB Host 供电与接线。
2. 关闭占用下载串口的软件，在 `hp2300_print_idf` 下执行：

```powershell
cd hp2300_print_idf
.\build_flash.ps1 -Port COM8
```

首次启动后连接 `mybips.com` 热点（初始 Wi-Fi 密码 `cCwX6goRgJ`），访问
`http://192.168.188.1/`。用初始管理密码 `mybips.com` 登录，首次初始化时为
**每块板子**设置不同的热点名称、新 Wi-Fi 密码及新管理密码。保存后重连新热点，
再按页上传由当前打印机驱动生成的 PRN。固件不会在复位时自动重印。

## 验证结论

实机日志显示打印机状态为 `0x18`（未报告缺纸、Selected、未报告错误），
PRN 从 `0/12627` 传输至 `12627/12627`，随后持续读取到正常状态。USB 接收完
全部字节并不等同于机械出纸完成；本次测试同时通过实际出纸确认成功。

## 打印机兼容性

当前固件已移除 HP VID/PID 白名单，动态寻找标准 USB Printer Class Protocol 1/2
接口；但只有上述 HP DeskJet 2300 型号经过实机出纸验证，不能据此宣称兼容其他型号。

跨型号支持不能只放开 VID/PID：PRN 必须由目标型号的驱动生成，或确认目标打印机
支持其中的打印语言。通用化所需的接口发现、`GET_DEVICE_ID` 能力检查及不同 USB
打印协议处理方案见[方案设计的兼容性章节](docs/方案设计.md#9-其他型号打印机兼容方案)。
