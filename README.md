# ESP32-S3 USB 打印机主机

本项目让 ESP32-S3 作为 USB Host，将 Windows 打印机驱动生成的原始 PRN
作业发送给 HP DeskJet 2300 series PCL-3。项目使用纯 ESP-IDF，不依赖 Arduino。

已验证硬件与协议：

- ESP32-S3 原生 USB：GPIO19=D−、GPIO20=D+；
- HP DeskJet 2300：VID/PID `03f0:3654`；
- Interface 1，协议 `07/01/02`；
- Bulk OUT `0x08`、Bulk IN `0x89`，Full-Speed，MPS 64；
- ESP-IDF v6.1、`espressif/usb` 1.5.0；
- 12,627 字节实测 PRN 完整发送并正常出纸。

## 目录

- [`docs/方案设计.md`](docs/方案设计.md)：硬件接线、安全供电、软件架构和排障。
- [`hp2300_print_idf/`](hp2300_print_idf/README.md)：最终 USB 打印程序。

## 快速开始

打印文件可能包含文档内容、用户名和打印后台元数据，因此仓库不包含测试使用的
`1.prn`，并通过 `.gitignore` 排除全部 `.prn` 文件。

1. 使用 HP DJ 2300 series PCL-3 驱动“打印到文件”，生成 PRN。
2. 将文件复制为 `hp2300_print_idf/main/1.prn`。
3. 按方案文档完成带限流和防回灌能力的 USB Host 供电与接线。
4. 关闭占用下载串口的软件，执行：

```powershell
cd hp2300_print_idf
.\build_flash.ps1 -Port COM8
```

固件每次启动自动提交一次作业。重复复位会重复打印。

## 验证结论

实机日志显示打印机状态为 `0x18`（未报告缺纸、Selected、未报告错误），
PRN 从 `0/12627` 传输至 `12627/12627`，随后持续读取到正常状态。USB 接收完
全部字节并不等同于机械出纸完成；本次测试同时通过实际出纸确认成功。
