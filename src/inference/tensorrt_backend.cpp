#include "iot/inference/tensorrt_backend.hpp"

#include <NvInfer.h>
#include <NvInferVersion.h>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace iot::inference {
namespace {

void cuda_check(cudaError_t status, const char* operation) {
  // CUDA API 出错时立即抛异常，避免后续继续使用无效的 GPU 资源。
  if (status != cudaSuccess)
    throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(status));
}

class Logger final : public nvinfer1::ILogger {
  void log(Severity severity, const char* message) noexcept override {
    // 只输出警告及更严重的 TensorRT 日志，减少普通信息对业务日志的干扰。
    if (severity <= Severity::kWARNING) std::clog << "[TensorRT] " << message << '\n';
  }
};

template <typename T>
struct TrtDeleter {
  void operator()(T* value) const noexcept {
    // 不同 TensorRT 版本的对象释放方式不同。
#if NV_TENSORRT_MAJOR < 8
    if (value) value->destroy();
#else
    delete value;
#endif
  }
};

class TensorRtBackend final : public services::InferenceBackend {
 public:
  TensorRtBackend(const std::filesystem::path& path, std::size_t inputs, std::size_t outputs)
      : input_elements_(inputs), output_elements_(outputs) {
    // 读取已经构建好的 TensorRT 序列化 Engine 文件。
    std::ifstream file(
        path,
        std::ios::binary | std::ios::ate);  // 以二进制模式打开文件， ate 把光标插入最好，方便读文件有多少字节
    if (!file) throw std::runtime_error("cannot open TensorRT engine: " + path.string());
    const auto size = file.tellg();
    if (size <= 0) throw std::runtime_error("TensorRT engine is empty");
    std::vector<char> blob(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(blob.data(), size);  //从当前位置，也就是文件开头，尝试读取 size 个字节，存入 blob：

    // 创建 TensorRT Runtime，并从二进制数据恢复 ICudaEngine。
    runtime_.reset(nvinfer1::createInferRuntime(logger_));
    if (!runtime_) throw std::runtime_error("createInferRuntime failed");
    engine_.reset(runtime_->deserializeCudaEngine(
        blob.data(), blob.size()));  //将这段二进制数据 ===反序列化===成一个引擎对象，并返回指针
    if (!engine_)
      throw std::runtime_error(
          "deserializeCudaEngine failed; engine may target another TensorRT/GPU version");

    // ExecutionContext 保存一次推理执行所需的运行时状态。
    context_.reset(engine_->createExecutionContext());
    if (!context_) throw std::runtime_error("createExecutionContext failed");

    // 创建 CUDA stream，并为输入、输出申请 GPU 显存。
    cuda_check(cudaStreamCreate(&stream_), "cudaStreamCreate");
    cuda_check(cudaMalloc(&input_, input_elements_ * sizeof(float)), "cudaMalloc(input)");
    cuda_check(cudaMalloc(&output_, output_elements_ * sizeof(float)), "cudaMalloc(output)");
#if NV_TENSORRT_MAJOR >= 10
    // TensorRT 10 使用 IO Tensor 名称绑定输入输出地址。
    for (int i = 0; i < engine_->getNbIOTensors(); ++i) {
      const char* name = engine_->getIOTensorName(i);
      if (engine_->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT)
        input_name_ = name;
      else
        output_name_ = name;
    }
    if (input_name_.empty() || output_name_.empty())
      throw std::runtime_error("engine input/output tensors not found");
#else
    // 旧版 TensorRT 使用 binding 下标绑定输入输出地址。
    for (int i = 0; i < engine_->getNbBindings(); ++i) {
      if (engine_->bindingIsInput(i))
        input_index_ = i;
      else
        output_index_ = i;
    }
    if (input_index_ < 0 || output_index_ < 0)
      throw std::runtime_error("engine input/output bindings not found");
#endif
  }
  ~TensorRtBackend() override {
    // 释放 GPU 缓冲区和 CUDA stream；TensorRT 对象由 unique_ptr 自动释放。
    if (input_) cudaFree(input_);
    if (output_) cudaFree(output_);
    if (stream_) cudaStreamDestroy(stream_);
  }
  int infer(const float* data,
            std::size_t length) override {  // ################################################## 这里最重要
    // 上层传入的是已经整理好的连续 float 数组。
    if (length != input_elements_) throw std::invalid_argument("unexpected inference input size");

    // 将输入从 CPU 异步拷贝到 GPU。
    cuda_check(cudaMemcpyAsync(input_, data, length * sizeof(float), cudaMemcpyHostToDevice, stream_),
               "copy input");
#if NV_TENSORRT_MAJOR >= 10
    // 绑定输入输出显存，并在指定 CUDA stream 上执行推理。
    if (!context_->setTensorAddress(input_name_.c_str(), input_) ||
        !context_->setTensorAddress(output_name_.c_str(), output_) || !context_->enqueueV3(stream_))
      throw std::runtime_error("TensorRT enqueueV3 failed");
#else
    // 旧版 API 需要传入按 binding 下标排列的显存地址数组。
    std::vector<void*> bindings(static_cast<std::size_t>(engine_->getNbBindings()));
    bindings[static_cast<std::size_t>(input_index_)] = input_;
    bindings[static_cast<std::size_t>(output_index_)] = output_;
    if (!context_->enqueueV2(bindings.data(), stream_, nullptr))
      throw std::runtime_error("TensorRT enqueueV2 failed");
#endif

    // 将推理输出从 GPU 异步拷贝回 CPU。
    std::vector<float> output(output_elements_);
    cuda_check(cudaMemcpyAsync(output.data(), output_, output.size() * sizeof(float), cudaMemcpyDeviceToHost,
                               stream_),
               "copy output");

    // 等待输入拷贝、TensorRT 推理和输出拷贝全部完成。
    cuda_check(cudaStreamSynchronize(stream_), "synchronize inference");

    // 返回输出数组中最大值的下标，作为分类结果。
    return static_cast<int>(std::distance(output.begin(), std::max_element(output.begin(), output.end())));
  }

 private:
  Logger logger_;
  std::unique_ptr<nvinfer1::IRuntime, TrtDeleter<nvinfer1::IRuntime>> runtime_;       // IRuntime对象
  std::unique_ptr<nvinfer1::ICudaEngine, TrtDeleter<nvinfer1::ICudaEngine>> engine_;  // ICudaEngine对象
  std::unique_ptr<nvinfer1::IExecutionContext, TrtDeleter<nvinfer1::IExecutionContext>>
      context_;  // IExecutionContext对象
  cudaStream_t stream_{};
  void* input_{};
  void* output_{};
  std::size_t input_elements_{};
  std::size_t output_elements_{};
#if NV_TENSORRT_MAJOR >= 10
  std::string input_name_;
  std::string output_name_;
#else
  int input_index_{-1};
  int output_index_{-1};
#endif
};

}  // namespace

std::unique_ptr<services::InferenceBackend> make_tensorrt_backend(const std::filesystem::path& path,
                                                                  std::size_t inputs, std::size_t outputs) {
  return std::make_unique<TensorRtBackend>(path, inputs, outputs);
}

}  // namespace iot::inference
