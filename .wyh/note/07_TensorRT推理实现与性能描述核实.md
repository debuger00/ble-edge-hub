# TensorRT 推理实现与性能描述核实

记录日期：2026-09-07

待核实的项目描述：

> 推理加速：将 ONNX 模型转换为 TensorRT Engine，部署至 Jetson Orin Nano；按设备维护独立滑动窗口，实现五分类行为识别，单次推理由 PyTorch 的 16.8 ms 降至 5.1 ms，耗时降低约 70%。

## 1. 结论

当前项目中，TensorRT 的核心实现位于 `src/inference/tensorrt_backend.cpp`，滑动窗口与类别映射位于 `src/services/inference_engine.cpp`。

代码能够证明项目实现了加载已有 TensorRT Engine、调用 GPU 推理、按设备维护独立窗口以及五分类结果映射。当前项目中未找到 ONNX 转 Engine 的脚本或命令，已检查的代码和文档也未提供 16.8 ms、5.1 ms 的对照测试记录。因此，需要将“已实现的功能”和“尚待补充证据的转换过程、部署及性能结论”分开理解。

本笔记基于源码与文件检查，没有重新运行 GPU 基准测试，也没有验证实际部署设备型号。

## 2. TensorRT 核心实现

文件：[tensorrt_backend.cpp](../../src/inference/tensorrt_backend.cpp)

核心类：`TensorRtBackend`，实现业务层定义的 `InferenceBackend` 接口。

### 2.1 初始化阶段：加载 Engine 和分配资源

`TensorRtBackend` 构造函数主要执行以下步骤：

1. 读取配置指定的 `.engine` 二进制文件。
2. 调用 `nvinfer1::createInferRuntime()` 创建 TensorRT Runtime。
3. 调用 `runtime_->deserializeCudaEngine()` 反序列化 Engine。
4. 调用 `engine_->createExecutionContext()` 创建执行上下文。
5. 创建 CUDA Stream，分配输入、输出显存。
6. 获取输入、输出的名称或绑定索引。

Engine、执行上下文、CUDA Stream 和输入输出显存在初始化后供后续推理复用。

注意：`deserializeCudaEngine()` 是加载已有 Engine，不是把 ONNX 转换成 Engine。

### 2.2 推理阶段：infer()

实际推理入口为：

```cpp
int infer(const float* data, std::size_t length) override
```

处理流程：

```text
检查输入元素数量
    → cudaMemcpyAsync：CPU 输入复制到 GPU
    → TensorRT 执行推理
    → cudaMemcpyAsync：GPU 输出复制回 CPU
    → cudaStreamSynchronize：等待完成
    → argmax：返回最大输出值对应的类别索引
```

真正下发推理的调用按 TensorRT 主版本区分：

```cpp
// TensorRT 10 及以上：绑定张量地址后调用
context_->enqueueV3(stream_);

// 低版本代码分支：通过 bindings 传递地址
context_->enqueueV2(bindings.data(), stream_, nullptr);
```

这里存在异步 CUDA 调用，但 `infer()` 会等待 Stream 完成后才返回分类结果。

## 3. 按设备维护独立滑动窗口

文件：

- [inference_engine.hpp](../../include/iot/services/inference_engine.hpp)
- [inference_engine.cpp](../../src/services/inference_engine.cpp)

窗口容器定义为：

```cpp
std::unordered_map<std::string, DeviceWindow> windows_;
```

每个 `DeviceWindow` 保存一个三轴样本队列和独立的步长计数器。处理样本时，通过设备 MAC 获取对应窗口：

```cpp
auto& window = windows_[sample.mac];
window.samples.push_back(*sample.accel);
```

超过窗口长度就移除最旧样本；窗口不足时不推理。窗口达到长度后，累计更新次数，满足步长条件才执行一次推理。

当前 [config.yaml](../../config/config.yaml) 中的配置：

```yaml
inference:
  enabled: true
  engine_path: "/home/aiam/opt/001demo/onnx/convtran.engine"
  window_size: 50
  stride: 5
```

每个窗口包含 50 个三轴加速度样本，即 150 个输入元素。代码按通道连续排列：

```text
[x1, x2, ..., x50, y1, y2, ..., y50, z1, z2, ..., z50]
```

业务层整理数据后调用：

```cpp
const int index = backend_->infer(input.data(), input.size());
```

细节：当前步长计数从窗口首次达到 50 个样本时开始增加；当 `stride = 5` 时，连续有效样本下第一次推理发生在第 54 个样本，此后每新增 5 个样本推理一次。

## 4. 五分类在哪里定义

`src/services/inference_engine.cpp` 中定义了类别顺序：

```cpp
constexpr std::array<const char*, 5> kClasses{
    "standing", "running", "grazing", "trotting", "walking"
};
```

| 输出索引 | 标签 | 含义 |
| --- | --- | --- |
| 0 | standing | 站立 |
| 1 | running | 奔跑 |
| 2 | grazing | 采食 |
| 3 | trotting | 小跑 |
| 4 | walking | 行走 |

后端返回最大输出值对应的索引，业务层检查索引范围后映射成标签。这个顺序必须与模型训练时的类别顺序一致；仅有这段映射代码不能证明模型识别准确率。

## 5. 完整调用链与线程关系

```text
DataPipeline::publish(sample)
    → InferenceWorker::enqueue(sample)
    → 有界推理队列
    → InferenceWorker::run() 工作线程取出样本
    → InferenceEngine::process(sample)
    → 更新该设备的窗口，判断窗口和步长条件
    → TensorRtBackend::infer(...)
    → 类别索引映射为行为标签
    → prediction_handler_(设备 MAC, 行为标签)
```

相关文件：

- [data_pipeline.cpp](../../src/services/data_pipeline.cpp)
- [inference_worker.cpp](../../src/services/inference_worker.cpp)

各设备的窗口独立，但共享推理后端，由工作线程依次处理。这里没有为每个设备创建独立 Engine 或独立 GPU 推理线程。

## 6. 构建开关与运行配置

[CMakeLists.txt](../../CMakeLists.txt) 中默认开启：

```cmake
option(IOT_ENABLE_TENSORRT "Compile the native TensorRT backend" ON)
```

启用后会查找 CUDA Toolkit、`NvInfer.h` 和 `libnvinfer`，编译 `tensorrt_backend.cpp`，链接 TensorRT 与 CUDA Runtime，并定义 `IOT_ENABLE_TENSORRT=1`。

运行时还需要配置 `inference.enabled: true`，且 Engine 能成功加载。未编译 TensorRT 或 Engine 加载失败时，业务层会记录日志，推理后端不会就绪。

当前配置实际加载 `/home/aiam/opt/001demo/onnx/convtran.engine`。虽然当前项目的 `model/` 中也有 `convtran.onnx` 和 `convtran.engine`，但这份配置没有指向它们。

## 7. ONNX 转 Engine 的实现在哪里

当前 C++ 项目中没有找到转换脚本、`trtexec` 命令或 ONNX 解析及 Engine 构建代码。文件存在只能说明已有模型产物，不能还原其生成过程。

沿着配置路径，还检查到另一目录中的实现：

- [001demo/onnx/trt_engine.cpp](/home/aiam/opt/001demo/onnx/trt_engine.cpp)：提供 `init_engine()`、`do_inference()`、`destroy_engine()` 等 C++ 接口。
- [001demo/services/inference_engine.py](/home/aiam/opt/001demo/services/inference_engine.py)：通过 `ctypes` 调用 C++ 推理接口。

这套实现同样是加载已有 Engine 并执行推理。已检查的内容不能确定当初的转换命令、FP32/FP16/INT8 设置或其他构建参数，不能直接将加速归因于某一种精度优化。

## 8. 16.8 ms → 5.1 ms 的依据

找到基准脚本：[001demo/benchmark_inference.py](/home/aiam/opt/001demo/benchmark_inference.py)。

脚本使用随机生成的 150 个 float32 元素作为输入，通过 `ctypes` 调用 `libtrt_infer.so`：

- 预热 100 次。
- 默认正式测量 2000 次。
- 输出平均值、标准差、最小值、最大值及 P50、P95、P99。
- 计时包含 Python 到 C++ 的调用开销，以及输入传输、TensorRT 推理、输出传输、同步和 argmax。
- 不包含在线采样、等待窗口形成、推理队列等待或 WebSocket 推送耗时。

该脚本只测试 TensorRT，没有 PyTorch 对照测试。已检查的代码和文档中未找到 16.8 ms、5.1 ms 的测试结果记录，本次也没有重新测量。

数值计算本身正确：

```text
耗时降幅 = (16.8 - 5.1) / 16.8 × 100% ≈ 69.6%
加速比   = 16.8 / 5.1 ≈ 3.29 倍
```

要将这组数字作为可复现的项目结论，需要补充两种后端在相同设备、模型、输入、批大小和计时范围下的测试记录，并记录精度、软件版本、设备功耗模式及预热和测量次数。

## 9. 项目介绍时如何表述

目前源码直接支持的表述：

> 实现 TensorRT C++ 推理后端，加载序列化 Engine，复用执行上下文与 GPU 缓冲区；按设备 MAC 维护独立的三轴加速度滑动窗口，通过独立工作线程执行五分类行为识别。

解释实现位置时可以说：

> TensorRT 推理封装在 TensorRtBackend 中，初始化阶段加载 Engine 并分配 GPU 资源，推理阶段调用 enqueueV3 或 enqueueV2。InferenceEngine 负责按设备维护窗口、组装输入以及映射分类标签，InferenceWorker 负责队列和工作线程。

“ONNX 转换”“部署至 Jetson Orin Nano”和“16.8 ms 降至 5.1 ms”应结合转换记录、部署记录及对照测试结果说明；目前这次源码检查没有完整验证这些内容。
