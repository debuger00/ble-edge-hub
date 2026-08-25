#pragma once

#include "iot/services/inference_engine.hpp"

#include <filesystem>
#include <memory>

namespace iot::inference {

std::unique_ptr<services::InferenceBackend> make_tensorrt_backend(
    const std::filesystem::path& engine_path, std::size_t input_elements, std::size_t output_elements);

}  // namespace iot::inference

