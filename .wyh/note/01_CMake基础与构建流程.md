# 01 CMake 基础与构建流程

## 1. CMake 是什么

CMake 是一个跨平台的构建系统生成工具。它读取 `CMakeLists.txt`，检查编译器和依赖，然后生成 Makefile、Ninja 文件或 Visual Studio 工程文件。

典型流程：

```text
CMakeLists.txt → 配置 → 生成构建文件 → 编译 → 测试 → 安装
```

## 2. 最基本的三条命令

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

### 2.1 配置项目

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

| 参数 | 含义 |
|---|---|
| `-S .` | 源码目录是当前目录，即包含 `CMakeLists.txt` 的目录 |
| `-B build` | 构建目录是 `build/` |
| `-D变量=值` | 设置 CMake 缓存变量 |
| `CMAKE_BUILD_TYPE=Release` | 使用 Release 构建模式 |

源码目录和构建目录分离，可以避免生成文件污染源码目录。

### 2.2 编译项目

```bash
cmake --build build -j2
```

`build` 是构建目录，`-j2` 表示最多同时运行两个编译任务。

### 2.3 运行测试

```bash
ctest --test-dir build --output-on-failure
```

`--output-on-failure` 表示测试失败时显示详细输出。

## 3. `CMAKE_BUILD_TYPE=Release`

`CMAKE_BUILD_TYPE` 是 CMake 的构建类型变量。`Release` 表示面向发布的构建，通常会开启优化，并定义 `NDEBUG`，从而关闭标准 `assert` 检查。

常见构建类型如下，具体参数可能因编译器和工具链而不同：

| 构建类型 | 常见选项 | 用途 |
|---|---|---|
| `Debug` | `-g -O0` | 调试，保留调试信息，基本不优化 |
| `Release` | `-O3 -DNDEBUG` | 发布，开启优化 |
| `RelWithDebInfo` | `-O2 -g -DNDEBUG` | 优化，同时保留调试信息 |
| `MinSizeRel` | `-Os -DNDEBUG` | 尽量减小程序体积 |

这里的 `-D` 是 CMake 设置变量的语法，不等同于直接给 C++ 编译器传递宏定义。

可以分别使用两个构建目录：

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j2

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j2
```

推荐 Debug 和 Release 使用不同目录，避免缓存混淆。

## 4. 当前项目的推荐命令

本项目使用 C++17，且 `clangd` 需要编译数据库：

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build build-release -j2
ctest --test-dir build-release --output-on-failure
```

`CMAKE_EXPORT_COMPILE_COMMANDS=ON` 会生成：

```text
build-release/compile_commands.json
```

该文件记录每个源文件真正使用的编译命令，包括头文件路径、C++ 标准、宏定义和编译选项。`clangd` 使用它实现补全、检查和代码跳转。

## 5. Target 概念

现代 CMake 以 Target 为核心。Target 可以是可执行文件、静态库、动态库或只保存编译配置的接口库。

本项目的核心配置：

```cmake
add_library(iot_core
    src/config.cpp
    src/protocol/gateway_codec.cpp
)

add_executable(ble-iot-edge src/main.cpp)
target_link_libraries(ble-iot-edge PRIVATE iot_core)
```

含义是：先用源文件生成 `iot_core` 库，再用 `src/main.cpp` 生成 `ble-iot-edge`，最后让主程序链接 `iot_core`。

## 6. 常用 Target 命令

### 6.1 头文件搜索路径

```cmake
target_include_directories(iot_core PUBLIC include)
```

代码中的：

```cpp
#include "iot/config.hpp"
```

需要把 `include/` 作为搜索根目录。

### 6.2 编译选项

```cmake
target_compile_options(iot_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
```

### 6.3 预处理宏

```cmake
target_compile_definitions(iot_core PUBLIC IOT_ENABLE_TENSORRT=1)
```

效果类似于编译时增加：

```bash
-DIOT_ENABLE_TENSORRT=1
```

C++ 中可以这样判断：

```cpp
#ifdef IOT_ENABLE_TENSORRT
// TensorRT 相关代码
#endif
```

### 6.4 链接库

```cmake
target_link_libraries(iot_core PRIVATE
    ${TENSORRT_LIBRARY}
    ${CUDA_CUDART_LIBRARY}
)
```

## 7. `PRIVATE`、`PUBLIC` 和 `INTERFACE`

它们表示配置是否传递给依赖当前 Target 的其他 Target：

| 关键字 | 当前 Target 使用 | 依赖当前 Target 的 Target 使用 |
|---|---:|---:|
| `PRIVATE` | 是 | 否 |
| `PUBLIC` | 是 | 是 |
| `INTERFACE` | 否 | 是 |

例如：

```cmake
target_include_directories(iot_core PUBLIC include)
```

因为 `ble-iot-edge` 链接了 `iot_core`，所以它也能继承 `include` 目录。

## 8. 选项和条件编译

```cmake
option(IOT_ENABLE_TENSORRT
       "Compile the native TensorRT backend"
       OFF)
```

这会创建一个默认值为 `OFF` 的布尔选项。配置时可以打开：

```bash
cmake -S . -B build -DIOT_ENABLE_TENSORRT=ON
```

配合：

```cmake
if(IOT_ENABLE_TENSORRT)
    # 查找 CUDA 和 TensorRT
    # 编译 TensorRT 后端
    # 链接 TensorRT 和 CUDA
endif()
```

本项目打开 TensorRT 后会查找 CUDA、`NvInfer.h` 和 `libnvinfer.so`，编译 `src/inference/tensorrt_backend.cpp`，并链接 TensorRT/CUDA Runtime。

## 9. 测试和安装

测试配置：

```cmake
enable_testing()
add_executable(protocol_tests tests/protocol_tests.cpp)
target_link_libraries(protocol_tests PRIVATE iot_core)
add_test(NAME protocol_tests COMMAND protocol_tests)
```

运行测试：

```bash
ctest --test-dir build --output-on-failure
```

安装配置：

```cmake
install(TARGETS ble-iot-edge RUNTIME DESTINATION bin)
install(DIRECTORY config web DESTINATION share/ble-iot-edge)
```

执行安装：

```bash
cmake --install build
```

## 10. 查看和调试 CMake 配置

查看缓存变量：

```bash
cmake -LAH -N build
```

查看实际编译命令：

```bash
cmake --build build --verbose
```

查看当前构建类型：

```bash
grep CMAKE_BUILD_TYPE build/CMakeCache.txt
```

## 11. 单配置和多配置生成器

Linux 上常见的 Unix Makefiles 和 Ninja 属于单配置生成器，需要在配置阶段指定：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

Visual Studio、Xcode 等属于多配置生成器，通常在编译阶段指定：

```bash
cmake --build build --config Release
```

## 12. 推荐学习顺序

1. `-S`、`-B`、配置目录和构建目录；
2. `add_library`、`add_executable`；
3. `target_include_directories`；
4. `target_link_libraries`；
5. `target_compile_options` 和 `target_compile_definitions`；
6. `PRIVATE`、`PUBLIC`、`INTERFACE`；
7. `option` 和 `if`；
8. `find_package` 和外部库；
9. `enable_testing`、`add_test` 和 `install`；
10. `CMakePresets.json`、交叉编译、CUDA 和 Jetson 工具链。
