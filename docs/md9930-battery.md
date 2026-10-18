# MD9930 电量读取

在 `sensor_types.md9930` 下配置 `battery_handle: 0x000F` 开启电量查询。
该值来自 007 的 FFF2 实测 value handle；其他设备或固件需要核对 GATT 映射。
省略此项不查询电量，兼容旧配置。当前句柄配置按传感器类型共享。

服务按照 `battery_interval_seconds`（当前 300 秒，即 5 分钟）轮询 ready 设备，且遵守采集时段。
网关命令为 0x12，内容为设备数量 01、MAC、两字节句柄、读类型 02、长度 01、占位 00。
电量回复通过 0x20 的设备 MAC 和特征句柄单独分流，只接受单字节 0–100 百分比。
有效值通过现有 battery WebSocket 消息广播，时间为服务端处理时间；不作为运动采样入库或推理。
网页保存最近收到的电量和时间。传感器断线或未回复时保留旧值，需注意显示的更新时间。

重启后端加载新配置后，设备 ready 后等待一个轮询周期，检查日志：
`battery received id=007 mac=54:6C:0E:B8:37:E4 level=72%`。
如果没有收到，核对设备是否 ready、采集时段和 FFF2 value handle；无效回复会输出原始 payload。

验证：protocol_tests、web_frontend_tests，以及 schedule_integration 的模拟网关测试。
模拟测试覆盖 ready 前不查询、读命令格式、无效电量拒绝和 0/72/100 电量经真实 WebSocket 推送。
模拟测试不能替代四台实机的句柄和网关读回复格式确认。
