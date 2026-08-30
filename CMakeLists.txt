cmake_minimum_required(VERSION 3.13)
project(ble_iot_edge VERSION 1.0.0 LANGUAGES CXX)

option(IOT_BUILD_TESTS "Build protocol unit tests" ON)
option(IOT_ENABLE_TENSORRT "Compile the native TensorRT backend" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(iot_core
    src/config.cpp
    src/protocol/gateway_codec.cpp
    src/protocol/sensor_parsers.cpp
    src/services/database_writer.cpp
    src/services/inference_engine.cpp
    src/services/websocket_server.cpp
    src/services/tcp_server.cpp)
target_include_directories(iot_core PUBLIC include)
target_link_libraries(iot_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
target_compile_options(iot_core PRIVATE -Wall -Wextra -Wpedantic)

if(IOT_ENABLE_TENSORRT)
  find_package(CUDA REQUIRED)
  find_path(TENSORRT_INCLUDE_DIR NvInfer.h HINTS /usr/include/aarch64-linux-gnu ${CUDA_INCLUDE_DIRS})
  find_library(TENSORRT_LIBRARY nvinfer HINTS /usr/lib/aarch64-linux-gnu)
  if(NOT TENSORRT_INCLUDE_DIR OR NOT TENSORRT_LIBRARY)
    message(FATAL_ERROR "TensorRT headers or library not found")
  endif()
  target_sources(iot_core PRIVATE src/inference/tensorrt_backend.cpp)
  target_include_directories(iot_core PRIVATE
    ${TENSORRT_INCLUDE_DIR}
    ${CUDA_INCLUDE_DIRS}
    ${CUDA_TOOLKIT_INCLUDE})
  target_link_libraries(iot_core PRIVATE ${TENSORRT_LIBRARY} ${CUDA_CUDART_LIBRARY})
  target_compile_definitions(iot_core PUBLIC IOT_ENABLE_TENSORRT=1)
endif()

add_executable(ble-iot-edge src/main.cpp)
target_link_libraries(ble-iot-edge PRIVATE iot_core)

install(TARGETS ble-iot-edge RUNTIME DESTINATION bin)
install(DIRECTORY config web DESTINATION share/ble-iot-edge)

if(IOT_BUILD_TESTS)
  enable_testing()
  add_executable(protocol_tests tests/protocol_tests.cpp)
  target_link_libraries(protocol_tests PRIVATE iot_core)
  target_compile_definitions(protocol_tests PRIVATE IOT_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME protocol_tests COMMAND protocol_tests)
endif()
