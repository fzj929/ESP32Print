# HP DeskJet 2300 ESP-IDF USB 打印程序

目标硬件为 ESP32-S3 和 HP `03F0:3654`。程序使用 ESP-IDF USB Host，启动后：

1. 枚举打印机并验证接口 1 / alt 0 / Printer Class 协议 2；
2. 占用 Bulk OUT `0x08` 和 Bulk IN `0x89`；
3. 使用 `GET_PORT_STATUS` 等待打印机就绪；
4. 将固件内嵌的 `main/1.prn` 原样发送一次；
5. 继续通过 UART0 输出打印机状态，不自动重复作业。

UART0 为 TX=GPIO43、RX=GPIO44、115200 baud。原生 USB 为 D-=GPIO19、
D+=GPIO20。程序不会控制 VBUS，必须使用已经验证的安全 5V Host 供电方式。

## 编译和烧录

先将驱动生成的私有作业复制为 `main/1.prn`。仓库通过 `.gitignore` 排除 PRN，
因为这类文件可能包含文档内容和打印后台元数据。关闭占用 COM8 的串口工具，
在 PowerShell 中执行：

```powershell
cd hp2300_print_idf
.\build_flash.ps1 -Port COM8
```

只编译：

```powershell
.\build_flash.ps1 -BuildOnly
```

固件启动即尝试打印一份。不要通过反复打开串口或复位来测试日志，否则可能重复打印。

## RGB 状态灯（GPIO48）

每次亮 200ms、灭 200ms，每组结束停 1.6 秒：

| 状态 | 显示 |
|---|---|
| 等待 USB | 红色闪 1 次 |
| 打开/检查打印机 | 青色闪 2 次 |
| 打印机未就绪 | 黄色闪 3 次 |
| 正在发送 | 蓝色闪 4 次 |
| 状态不可用 | 紫色闪 5 次 |
| 打印机断开 | 红色闪 6 次 |
| 传输错误 | 红色闪 7 次 |
| 非目标设备 | 橙色闪 8 次 |
| USB 已接收全部文件 | 绿色常亮 |

绿色常亮只说明 12,627 个文件字节均由 USB 接收，不代表机械打印过程已经完成。

## 实机验证

已在 ESP32-S3（芯片 revision v0.2）和 HP DeskJet 2300 上完成实际打印：

- 枚举得到 `03f0:3654`，Interface 1，Bulk OUT `0x08`、Bulk IN `0x89`；
- 打印前及打印后的端口状态均为 `0x18`；
- `1.prn` 的 12,627 字节全部完成 Bulk OUT 传输；
- 打印机实际正常出纸，日志中没有断线、STALL 或传输超时。

## 更换打印文件

把新文件复制为 `main/1.prn`，然后重新编译烧录。程序不会解析或修改 PRN 内容。
