# CLAUDE.md — BLE IoT Edge Computing Platform

## 项目概述

软硬结合的 BLE 物联网边缘计算平台。蓝牙网关通过 TCP 透传多个 BLE 传感器的数据，服务端解析协议、执行 AI 行为识别推理，并通过 WebSocket 推送至前端可视化大屏。

**支持的传感器型号：**

| 传感器 | 制造商 | 连接方式 |
|--------|--------|---------|
| IMU948 | 辰翼电子 (十轴姿态传感器) | 网关 BLE → TCP 透传 |
| MD9930 | 北京微点 (智能姿态模块) | 网关 BLE → TCP 透传 |

---

## 协议参考文档

项目中有三个核心协议文档，开发时务必参考：

- **蓝牙网关协议 (KTBG602)**：`000资料/KTBG602网关蓝牙十六进制格式协议V1.23_*.md`
  - 帧头 `AC CA` = 命令/数据包（含 0x10 连接、0x11 断开、0x12 透传、0x1F 查询连接状态、0x20 设备数据上报、0x21 状态变化、0x22 心跳、0x27 参数更新、0xF0 命令回复）
  - 帧头 `AD BA` = 扫描上报包
  - 校验：从长度字段到校验码前一字节的 XOR（单字节，重复两次填入）
  - 命令帧结构：`AC CA`(2) + 长度(2, 大端) + 网关MAC(6) + MsgId(2) + 类型(1) + Payload(N) + 校验(1)

- **IMU948 传感器协议**：`000资料/imu948.md`
  - BLE GATT: Service `ae30`, Write `ae01` (write no response), Notify `ae02` (需使能 CCCD)
  - GATT Handle: notify=0x0008, cccd=0x0009, write=0x0006
  - 数据体 tag `0x11` = 订阅数据上报 (含 sub_tag + timestamp + 按订阅标识排列的数据)
  - 数据体 tag `0x10` = 设备属性状态上报 (电池电量、固件版本、MAC 等)
  - 发送 `0x29` 解除 IMU948 连接后的 30 秒断开保护；此命令仅用于 IMU948，不可发送给 MD9930
  - 发送 `0x10` 查询设备状态/电量
  - 发送 `0x12` 设置传感器参数（11字节 payload，小端，含 FPS + 滤波系数 + 订阅标识）
  - **订阅标识 (16-bit)**：bit0=无重力加速度, bit1=含重力加速度, bit2=角速度, bit3=磁场, bit4=温压高, bit5=四元数, bit6=欧拉角, bit7=三维位置, bit8=活动检测, bit9=导航加速度, bit10=ADC, bit11=GPIO
  - 缩放系数：ACCEL=0.00478515625, ANGLE_SPEED=0.06103515625, MAG=0.15106201171875, QUAT=0.000030517578125, ANGLE=0.0054931640625, TEMP=0.01, AIR_PRESSURE=0.0002384185791, HEIGHT=0.0010728836

- **MD9930 传感器协议**：`000资料/md9930_技术文档.txt`
  - GATT: Service FFF0, GyroData(FFF1, notify), BatteryLevel(FFF2, read), BleCommand(FFF3, write), BleData(FFF4, write), InfoNotify(FFF5, read)
  - GATT Handle: notify=0x000B（2026-10-16 实测通知 value handle）, cccd=0x000C, write=0x0015, command=0x0012
  - 18字节数据帧：Accel×3(LE, s16) + Gyro×3(BE, s16) + Counter(BE, u16) + Ticks(BE, u32, 10μs，V20 实测)
  - 加速度分辨率 0.488 mg/LSB，角速度分辨率 0.07 °/s/LSB
  - 设置频率：先向 BleData(FFF4) 写 6 字节(前2字节=周期ms BE + 后4补零)，再向 BleCommand(FFF3) 写 0x0A
  - 时间同步：向 BleCommand 写 0x22（之后 Ticks 有效，有效期 10 小时）
  - 可调精度：0x20→BleData 写入精度值 再 BleCommand 写 0x20

---



## 约定与注意事项

- 所有 MAC 地址统一使用大写格式 `"XX:XX:XX:XX:XX:XX"`
- 协议解析使用大端序 (`>H`, `>I`) 作为默认字节序，传感器数据按各自文档处理
- IMU948 传感器数据体为**小端序**，MD9930 加速度 LE、角速度 BE
- 未知 MAC 设备默认 fallback 到 `imu948` 解析器
- `services/device_profiles.py` 必须在启动时调用 `init_from_config(config)` 初始化
- TCP 端口 7628，WebSocket 端口 8765，前端静态服务端口 8000
- IMU948 保活：就绪后向其写特征发送一次 `0x29`；MD9930 按其专用协议配置。
- 网关 0x10 连接命令：超时字段填 0 表示长连接；原始协议中的非零值会限制建连和连接后的空闲时间。
- 自动重连：设备就绪时自动加入 `auto_reconnect_macs`，每 60 秒检测离线设备并重连

- 网关 0x27 上报中参数类型 0x03 表示设备端断线，后续字节为 HCI 原因码；记录原因，避免把链路超时全部归因为后端重连。
