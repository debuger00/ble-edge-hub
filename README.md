# 🌐 BLE IoT Edge Computing Platform

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.13%2B-064F8C?logo=cmake&logoColor=white)
![SQLite](https://img.shields.io/badge/SQLite-Time--Series-003B57?logo=sqlite&logoColor=white)
![WebSocket](https://img.shields.io/badge/WebSocket-Real--Time-0A7BBB?logo=websocket&logoColor=white)
![TensorRT](https://img.shields.io/badge/TensorRT-Optional-76B900?logo=nvidia&logoColor=white)

一个面向工业物联网场景的蓝牙传感器边缘计算平台。系统接收 BLE 网关转发的传感器数据，完成网络通信、二进制协议解析、设备管理、数据存储、实时推送和行为识别，适合部署在 Linux 边缘设备或 NVIDIA Jetson 平台上。

```text
┌────────────────┐     ┌──────────────┐     ┌─────────────────────┐
│ BLE 传感器      │ ──▶ │ BLE 网关       │ ──▶ │ TCP 接入与协议解析    │
│ IMU948/MD9930 │     │ KTBG602 等    │     └──────────┬──────────┘
└────────────────┘     └──────────────┘                │
                                      ┌───────────────┼────────────────┐
                                      ▼               ▼                ▼
                               SQLite 时序存储   TensorRT 推理    WebSocket 推送
                                                                       │
                                                                       ▼
                                                               Web 可视化大屏
```

## 📖 项目背景

在工业物联网、智能畜牧、设备状态监测和运动行为分析等场景中，传感器通常通过 BLE 采集加速度、陀螺仪、磁力计、电池状态等数据，再由网关通过 TCP 转发到边缘计算设备。

这类系统通常需要同时解决以下问题：

- 传感器数据频率较高，不能因为磁盘写入或模型推理阻塞网络接收；
- TCP 是字节流协议，一条业务消息可能被拆成多个包，也可能多个消息粘在同一个包中；
- 多种传感器的数据格式不同，需要根据设备身份选择对应解析器；
- 设备可能断开、重连或同时接入多个网关；
- 原始数据、实时波形和行为识别结果需要同时提供给存储系统和浏览器；
- 边缘设备资源有限，需要将网络、存储和推理任务解耦。

本项目提供完整的数据处理链路：

```text
设备接入 → 网关通信 → TCP 字节流重组 → 校验和验证 → 传感器解析
                                             ├── 数据库队列 → SQLite 写入线程
                                             ├── 推理队列 → TensorRT 推理线程
                                             └── 发送队列 → WebSocket 广播线程
```

## ✨ 核心特性

### ⚡ 高可靠 TCP 网关通信

- 支持多个网关同时建立 TCP 长连接；
- 处理 TCP 粘包和半包；
- 校验错误或损坏帧出现后，可以继续从字节流中寻找下一条有效帧；
- 支持网关发现设备、连接设备、断开设备和周期性重连；
- 支持交互式命令行和无 TTY 守护运行模式；
- 收到 `SIGINT`/`SIGTERM` 后按顺序停止网络、存储和推理服务。

### 🛡️ 多传感器协议解析

- 支持 IMU948 和 MD9930；
- 通过 MAC 地址匹配设备配置，不允许未知设备直接进入数据链路；
- IMU948 根据数据标志位解析加速度、原始加速度、陀螺仪和磁力计；
- MD9930 解析加速度、陀螺仪、计数器和传感器时钟；
- 对数据长度进行检查，截断或格式错误的数据不会继续向下游传播；
- 传感器类型、采样频率和 GATT Handle 均可通过配置文件调整。

### 🧠 流式行为识别

- 支持 TensorRT C++ 推理后端；
- 为每个传感器维护独立的滑动窗口；
- 支持配置窗口长度 `window_size` 和推理步长 `stride`；
- 使用有界队列和独立工作线程，TensorRT 不阻塞 TCP 接收；
- 当前行为类别包括 `standing`、`running`、`grazing`、`trotting` 和 `walking`；
- TensorRT 是可选组件，没有 CUDA/TensorRT 的设备仍可编译采集、存储和测试功能。

### 💾 异步时序数据存储

- 使用后台线程处理 SQLite 写入；
- 使用有界队列隔离网络接收线程和磁盘 I/O；
- 使用按日期组织的数据库文件，便于归档和清理；
- 使用 WAL 和批量事务提高高频写入效率；
- 自动创建 `sensor_data` 表和 MAC/时间索引；
- 根据 `retention_days` 清理过期数据。

### 📡 实时 WebSocket 推送

服务端通过有界发送队列和独立线程向浏览器广播传感器三轴数据、设备电池电量、行为识别结果和初始设备列表。慢客户端不会直接阻塞 TCP 接收线程；队列过载时丢弃最旧的实时消息。前端使用 HTML、JavaScript 和 Chart.js 展示实时波形、设备在线状态、电池信息和行为识别结果。

## 📊 支持的传感器

| 传感器 | 数据内容 | 典型用途 | 接入方式 |
|---|---|---|---|
| **IMU948** | 加速度、原始加速度、陀螺仪、磁力计、运行时间、电池状态 | 惯性测量和动作采集 | BLE → 网关 → TCP |
| **MD9930** | 加速度、陀螺仪、计数器、传感器时钟 | 姿态和运动数据采集 | BLE → 网关 → TCP |

新增传感器时，需要实现统一的传感器解析接口，并在配置文件中增加设备类型、MAC 地址和 GATT Handle 信息。

## 🏗️ 项目结构

```text
.
├── CMakeLists.txt                  # CMake 构建入口
├── config/
│   └── config.yaml                 # 网络、设备、存储和推理配置
├── include/iot/
│   ├── config.hpp                  # 配置对象和配置校验
│   ├── domain.hpp                  # 传感器数据领域模型
│   ├── json.hpp                    # WebSocket JSON 消息序列化
│   ├── log.hpp                     # 日志工具
│   ├── concurrency/
│   │   └── bounded_queue.hpp       # 通用有界阻塞队列
│   ├── protocol/
│   │   ├── gateway_codec.hpp       # 网关帧解码和控制命令
│   │   └── sensor_parser.hpp       # 传感器解析接口
│   ├── services/
│   │   ├── tcp_server.hpp          # TCP 网关服务
│   │   ├── data_pipeline.hpp       # 传感器消息分发
│   │   ├── websocket_server.hpp    # WebSocket 服务
│   │   ├── database_writer.hpp     # SQLite 异步写入
│   │   ├── inference_engine.hpp    # 推理窗口管理
│   │   └── inference_worker.hpp    # 异步推理工作线程
│   └── inference/
│       └── tensorrt_backend.hpp    # 可选 TensorRT 后端接口
├── src/
│   ├── main.cpp                    # 程序入口和命令行控制台
│   ├── config.cpp                  # 配置解析与校验
│   ├── protocol/
│   │   ├── gateway_codec.cpp       # 网关协议实现
│   │   └── sensor_parsers.cpp      # IMU948/MD9930 解析实现
│   ├── services/
│   │   ├── tcp_server.cpp          # 网关连接和数据分发
│   │   ├── data_pipeline.cpp       # 多通道非阻塞分发
│   │   ├── websocket_server.cpp    # 握手和数据广播
│   │   ├── database_writer.cpp     # SQLite 数据落盘
│   │   ├── inference_engine.cpp    # 滑动窗口推理编排
│   │   └── inference_worker.cpp    # 推理队列消费
│   └── inference/
│       └── tensorrt_backend.cpp    # TensorRT 推理实现
├── tests/
│   └── protocol_tests.cpp          # 协议和推理测试
├── web/
│   ├── index.html                  # 监控大屏页面
│   └── app.js                      # WebSocket 和图表逻辑
├── model/                          # TensorRT engine 文件目录
└── README.md
```

## 🔧 环境要求

### 普通开发环境

- Linux/POSIX 环境；
- CMake `3.13+`；
- 支持 C++17 的编译器，例如 GCC 或 Clang；
- pthread；
- SQLite 运行库：`libsqlite3.so.0` 或 `libsqlite3.so`；
- Python 3（仅用于启动前端静态文件服务器）。

Ubuntu/Debian 系统可以准备基础环境：

```bash
sudo apt update
sudo apt install build-essential cmake libsqlite3-0 python3
```

SQLite 使用运行时动态加载，因此只运行程序时需要 SQLite runtime，不要求安装 SQLite 开发头文件。

### TensorRT 环境（可选）

如果需要启用行为识别，还需要与目标设备匹配的 CUDA、TensorRT runtime、TensorRT 开发头文件，以及匹配目标 GPU 和 TensorRT 版本的 `convtran.engine`。

## 🚀 快速开始

### 1. 获取项目

```bash
git clone <repository-url>
cd 001demo_cpp
```

### 2. 配置传感器和网关

编辑 `config/config.yaml`：

```yaml
tcp:
  host: "0.0.0.0"
  port: 7628

websocket:
  host: "0.0.0.0"
  port: 8765

# 如果网关固件要求真实地址，请替换为网关实际 MAC。
gateway_mac: "FF:FF:FF:FF:FF:FF"

battery_interval_seconds: 30
reconnect_interval_seconds: 60
retention_days: 7
data_dir: "data"

devices:
  - mac: "AA:BB:CC:DD:EE:FF"
    type: imu948
```

`devices` 中填写实际传感器的 MAC 地址。支持的设备类型是 `imu948` 和 `md9930`。

### 3. 编译

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build build -j2
```

编译产物：

```text
build/ble-iot-edge       # 主程序
build/protocol_tests     # 协议测试程序
build/compile_commands.json
```

其中 `compile_commands.json` 可以被 clangd 用于代码补全、错误检查和跳转到定义。

### 4. 运行测试

```bash
ctest --test-dir build --output-on-failure
```

测试不需要真实网关和传感器，主要覆盖协议解析和滑动窗口推理逻辑。

### 5. 启动边缘服务

必须在项目根目录运行，以便正确找到默认配置和模型路径：

```bash
./build/ble-iot-edge --config config/config.yaml
```

也可以直接将配置文件作为位置参数传入：

```bash
./build/ble-iot-edge config/config.yaml
```

默认监听地址：

| 服务 | 默认地址 | 说明 |
|---|---|---|
| TCP 网关服务 | `0.0.0.0:7628` | 接收网关连接和传感器数据 |
| WebSocket 服务 | `0.0.0.0:8765` | 向监控页面广播数据 |

### 6. 启动 Web 监控页面

在另一个终端运行：

```bash
python3 -m http.server 8000 --directory web
```

浏览器访问：

```text
http://localhost:8000
```

如果浏览器和边缘服务不在同一台机器上：

```text
http://<边缘设备IP>:8000
```

前端默认连接当前页面主机的 `8765` 端口。如果 WebSocket 端口不同，可以使用：

```text
http://<边缘设备IP>:8000/?wsPort=9000
```

页面依赖 CDN 提供 Bulma、Chart.js 和时间适配器，浏览器需要能够访问对应 CDN。

## 🖥️ CLI 控制台

服务在交互式终端中运行时，支持以下命令：

```text
list
stats
connect <网关ID> <设备MAC>
disconnect <网关ID> <设备MAC>
q
```

| 命令 | 功能 |
|---|---|
| `list` | 查看网关连接状态、发现的设备和已连接设备 |
| `stats` | 查看数据库、推理和 WebSocket 队列深度及丢弃计数 |
| `connect` | 请求指定网关连接指定传感器 |
| `disconnect` | 请求指定网关断开指定传感器 |
| `q` | 停止服务、排空数据队列并退出 |

网关 ID 会在 TCP 连接建立后由服务端分配，使用 `list` 可以查看当前 ID。

在 systemd、Docker 或其他没有 TTY 的环境中，程序自动进入守护模式，通过以下信号退出：

```bash
kill -TERM <pid>
```

## ⚙️ 配置说明

### 网络配置

```yaml
tcp:
  host: "0.0.0.0"
  port: 7628
websocket:
  host: "0.0.0.0"
  port: 8765
gateway_mac: "FF:FF:FF:FF:FF:FF"
```

- `tcp.host` / `tcp.port`：网关 TCP 监听地址和端口；
- `websocket.host` / `websocket.port`：WebSocket 监听地址和端口；
- `gateway_mac`：下发网关控制命令时使用的网关 MAC。

生产环境不建议无条件监听所有网卡，可以根据实际网络拓扑将 `host` 改为指定网卡地址，并配置防火墙规则。

### 存储和连接策略

```yaml
battery_interval_seconds: 30
reconnect_interval_seconds: 60
retention_days: 7
data_dir: "data"
```

- `battery_interval_seconds`：电池状态查询周期；
- `reconnect_interval_seconds`：设备自动重连周期；
- `retention_days`：数据库保留天数；
- `data_dir`：SQLite 数据根目录。

数据按日期保存：

```text
data/
└── 2026-08-25/
    └── sensor.db
```

数据库主要包含 `sensor_data` 表，字段包括采集时间、设备 MAC、传感器运行时间、加速度、陀螺仪和磁力计数据。

### 传感器配置

```yaml
sensor_types:
  imu948:
    frequency: 25
    notify_handle: 0x0008
    cccd_handle: 0x0009
    write_handle: 0x0006

  md9930:
    frequency: 25
    notify_handle: 0x000A
    cccd_handle: 0x000C
    write_handle: 0x0014
    command_handle: 0x0012

devices:
  - mac: "AA:BB:CC:DD:EE:FF"
    type: imu948
```

- `frequency`：传感器采样频率；
- `notify_handle`：通知特征 Handle；
- `cccd_handle`：通知配置 Handle；
- `write_handle`：写命令 Handle；
- `command_handle`：需要额外命令 Handle 的设备配置；
- `devices`：允许接入的设备 MAC 和类型。

### 行为识别配置

```yaml
inference:
  enabled: true
  engine_path: "model/convtran.engine"
  window_size: 50
  stride: 5
```

- `enabled`：是否尝试启用行为识别；
- `engine_path`：TensorRT engine 路径；
- `window_size`：单次推理使用的采样窗口长度；
- `stride`：窗口达到完整长度后，每隔多少次更新执行一次推理。

### 队列配置

```yaml
queues:
  database_capacity: 30000
  inference_capacity: 5000
  websocket_capacity: 4096
```

- `database_capacity`：等待写入 SQLite 的样本上限，队列满时拒绝新样本；
- `inference_capacity`：等待推理的样本上限，队列满时丢弃最旧样本，优先保证结果实时性；
- `websocket_capacity`：等待广播的消息上限，队列满时丢弃最旧消息，避免慢客户端导致内存无限增长。

按 30 台设备、每台 25 Hz（约 750 条/秒）估算，默认容量分别提供约 40 秒、6.7 秒和 5.5 秒的瞬时积压空间。队列容量只能吸收短时抖动；如果队列长度持续增长，应降低推理或展示频率，或提高下游处理能力。

## 🧠 TensorRT 行为识别

普通 Linux 开发机可以不启用 TensorRT。需要使用 GPU 推理时，先安装与目标 Jetson/GPU、CUDA 和 TensorRT 版本匹配的开发包，并将模型文件放入：

```text
model/convtran.engine
```

然后执行：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DIOT_ENABLE_TENSORRT=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build build -j2
ctest --test-dir build --output-on-failure
```

构建过程会查找 CUDA、`NvInfer.h` 和 `libnvinfer.so`。TensorRT engine 通常不能跨 GPU 架构或跨 TensorRT 版本直接复用。

当前行为识别类别：

```text
standing / running / grazing / trotting / walking
```

如果程序没有使用 `-DIOT_ENABLE_TENSORRT=ON` 编译，即使配置中的 `inference.enabled: true`，程序也会记录提示并继续提供网络接入、数据存储和 WebSocket 服务。

## 📡 WebSocket 数据格式

服务端向浏览器发送 JSON 文本消息。

### 设备列表

```json
{
  "type": "device_list",
  "devices": [
    {"mac": "AA:BB:CC:DD:EE:FF", "type": "imu948"}
  ]
}
```

### 传感器数据

```json
{
  "type": "sensor_data",
  "mac": "AA:BB:CC:DD:EE:FF",
  "timestamp": 1720000000000,
  "sensor_uptime": 123456,
  "data": {
    "accel": {"x": 0.1, "y": 0.2, "z": 9.8},
    "gyro": {"x": 0.0, "y": 0.1, "z": 0.2},
    "device_type": "imu948"
  }
}
```

### 电池状态

```json
{
  "type": "battery",
  "mac": "AA:BB:CC:DD:EE:FF",
  "timestamp": 1720000000000,
  "level": 80
}
```

### 行为识别结果

```json
{
  "type": "prediction",
  "mac": "AA:BB:CC:DD:EE:FF",
  "timestamp": 1720000000000,
  "action": "walking"
}
```

## 🧪 测试和开发

运行全部测试：

```bash
ctest --test-dir build --output-on-failure
```

直接运行协议测试：

```bash
./build/protocol_tests
```

查看实际编译命令：

```bash
cmake --build build --verbose
```

建议使用 clangd 进行 C++ 代码补全和跳转，并在配置时生成编译数据库：

```bash
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

## 📦 安装

通过 CMake 安装主程序、配置目录和前端目录：

```bash
cmake --install build
```

指定安装目录：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/opt/ble-iot-edge

cmake --build build -j2
cmake --install build
```

## 🔍 常见问题

### 找不到 TensorRT

如果不需要 GPU 推理，请不要设置 `-DIOT_ENABLE_TENSORRT=ON`。如果需要启用，请确认以下文件存在：

```text
NvInfer.h
libnvinfer.so
```

并确认 CUDA、TensorRT 的架构与目标设备匹配。

### 程序提示 SQLite runtime unavailable

安装 SQLite 运行库：

```bash
sudo apt install libsqlite3-0
```

### Web 页面无法连接 WebSocket

依次检查：

1. C++ 服务是否已经启动；
2. 浏览器访问的 WebSocket 端口是否与 `config.yaml` 一致；
3. 防火墙是否放行 `7628` 和 `8765`；
4. 浏览器是否可以访问页面依赖的 CDN；
5. 页面地址是否使用了正确的 `wsPort` 参数。

### 网关连接成功但没有传感器数据

检查：

1. 传感器 MAC 是否已经写入 `devices`；
2. `type` 是否为 `imu948` 或 `md9930`；
3. `notify_handle`、`cccd_handle` 和 `write_handle` 是否与设备固件一致；
4. `gateway_mac` 是否符合网关协议要求；
5. 使用 CLI 的 `list` 查看网关和设备状态。

## ⚠️ 当前限制

- WebSocket 当前主要用于服务端向监控页面单向广播，不处理浏览器发送的业务控制命令；
- MD9930 电池特征需要网关支持 GATT read，当前不会使用 IMU948 的电池写命令代替读取；
- TensorRT engine 需要与目标 GPU、CUDA 和 TensorRT 版本匹配；
- 生产环境部署时应限制 TCP/WebSocket 监听地址，并配置防火墙、日志轮转和服务自动重启；
- `web/` 页面依赖外部 CDN，完全离线部署时需要将前端依赖下载到本地。
