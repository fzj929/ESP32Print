# ESP32-S3 USB 打印维护固件

本固件以已实测的 HP DeskJet 2300（USB `03f0:3654`）传输实现为基础，增加本地 Wi-Fi 维护页面，并移除了打印机 VID/PID 白名单。现在会动态寻找标准 USB Printer Class Protocol 1/2 接口及 Bulk OUT 端点；Bulk IN 为可选。其他型号尚未实机验证，不能据此宣称兼容。板子首次上电开启 `mybips.com` 热点（密码 `cCwX6goRgJ`），连接后访问 `http://192.168.188.1/`。管理初始密码是 `mybips.com`；首次登录必须同时设置新管理密码和这块板自己的热点名称、Wi-Fi 密码，才能上传打印文件。多台板子应使用不同的热点名称，便于用户按打印机选板。

在页面输入作业号和总页数，按顺序选择每页由**当前打印机驱动**生成的 PRN。一次只允许一个活动打印会话；每页完整接收后推送 USB，上一页达到 `usb_delivered` 才开放下一页。单页上限 2 MiB；固件按接收块计算 SHA-256，收到请求头中的摘要时会比对。普通 HTTP 页面可能无法使用浏览器 WebCrypto，此时开发模式允许缺少摘要头，仅校验传输长度；服务器客户端应传 `X-Page-SHA256`。`usb_delivered` **不保证纸张机械出纸完成**；中断或超时不会自动重试。

HTTP 接口：

- `POST /api/v1/auth/login`：JSON `{"password":"..."}`，返回 Bearer token。
- `PUT /api/v1/auth/password`：JSON `{"old_password":"...","new_password":"..."}`。
- 首次调用 `PUT /api/v1/auth/password` 还必须提交 `wifi_ssid` 和 `wifi_password`；三项配置在同一 NVS 事务中保存。热点名称为 1～32 字节（支持 UTF-8），Wi-Fi 密码为 8～63 位可打印 ASCII 字符。成功后约 2 秒重启，客户端需要连接新热点，IP 仍为 `192.168.188.1`。
- `POST /api/v1/auth/logout` 和 `/api/v1/auth/refresh`：吊销或轮换 Token。
- `GET /api/v1/device/status` 和 `POST /api/v1/device/reboot`：诊断及空闲重启。
- `GET/PUT /api/v1/device/wifi`：已初始化管理员查询热点名称，或在无活动打印作业时修改热点名称和密码；不返回现有 Wi-Fi 密码，修改成功后重启并断开当前连接。
- `GET /api/v1/printer/info`：返回 VID/PID、USB 字符串、接口、协议、端点和可读取到的 IEEE 1284 Device ID；读取失败时明确标记为不可用。
- `GET /api/v1/printer/status`：查询连接、缺纸/选中/错误、作业状态及 USB 进度。`physical_job_state` 和 `mechanical_completion` 为 `unknown`，不能把 USB 已接收当作纸张打印完成。
- `POST /api/v1/print-sessions`：JSON `{"job_id":"job-001","total_pages":2}`，创建独占会话。
- `GET/DELETE /api/v1/print-sessions/{id}`：查询或取消会话。
- `POST /api/v1/print-sessions/{id}/heartbeat`：续租。
- `POST /api/v1/print-sessions/{id}/pages/{n}`：`application/octet-stream` 原始页面；推荐 `X-Page-SHA256`，成功返回 HTTP 202。
- `POST /api/v1/print-sessions/{id}/resolve-unknown`：在人工检查后显式提交 `{"action":"release_without_retry"}`；板卡重启 USB Host，绝不自动重印。

USB 枚举、连接、端口状态、登录、文件接收、USB 推送进度及故障通过 ESP-IDF 日志输出到调试串口 UART0（TX GPIO43，RX GPIO44，115200）。原生 USB 使用 D− GPIO19、D+ GPIO20；本程序不控制 VBUS，必须使用已验证的安全 5V Host 供电。状态灯接 GPIO48。

打印机 USB 线拔出后会释放旧设备、接口和传输对象；重插后等待 USB 重新枚举并自动连接，不自动重发中断的页面。若断线发生在 USB 发送中，页面结果标记为 `failed_unknown`，必须人工核查。仅在 USB Host 无法安全回收仍在途的传输对象时，固件才自动重启以恢复主机栈。USB Printer Class 的 `GET_PORT_STATUS` 仅提供缺纸、选中和错误位，不提供通用的“打印中”或“机械完成”位。

在项目目录执行 `./build_flash.ps1 -BuildOnly` 编译，或 `./build_flash.ps1 -Port COM8` 烧录；先按实际端口更改 `COM8`。如果使用旧版 `sdkconfig`，请确认分区方案为 `Custom partition table`、文件名为 `partitions.csv`。本分区表增加双 OTA 槽并移动应用偏移；升级既有设备前必须制定分区及 NVS 迁移/备份方案，**不能直接按旧烧录流程覆盖量产设备**。运行时不再内嵌 `main/1.prn`，也不会因复位自动重印。

恢复输入候选为 GPIO4（上电持续高电平约 2 秒触发、恢复后等待释放；需外部下拉并由原理图确认该脚未占用）。GPIO0、3、45、46 是 ESP32-S3 的启动绑带脚，因此不选用。编号区域预留 eFuse BLOCK3 / USER_DATA，完整 32 字节独占且不与 Custom MAC 共用。当前只做 BLOCK3 的**只读格式、CRC 和写保护检查**；`provisioning/identity_image.py --device-id <uuid> --output <file>` 仅生成镜像，不会烧写。`CONFIG_BIPS_DEVELOPMENT_MODE=y` 允许未配置设备测试打印及 GPIO4 开发恢复；关闭后打印会安全锁定，直到量产身份/证书与恢复码流程完成。GPIO4 和 BLOCK3 的选取仍需结合板卡原理图与 eFuse 总分配表评审。[绑带脚资料](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html)、[eFuse 区域资料](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/efuse.html)。

开发模式 GPIO4 密码恢复会清除同一认证命名空间中的管理密码与热点配置，热点恢复到默认名称及密码，下一次打开页面必须重新初始化。请只在可信现场使用此测试恢复方式。

当前仍是工程测试固件，**不是开发计划的全部实现或正式产品**。尚缺验证型号注册表、每机恢复码 claim、证书绑定、HTTPS、签名 OTA、Secure Boot/Flash Encryption 量产验证、完整幂等历史、各种 USB 故障的长期恢复验证，以及多型号/多用户长时间实机验收。HTTP 密码、Token 和文件仍以明文在热点内传输。
