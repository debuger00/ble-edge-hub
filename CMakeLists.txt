cmake_minimum_required(VERSION 3.17)
project(ble_iot_edge VERSION 1.0.0 LANGUAGES CXX)

option(IOT_BUILD_TESTS "Build protocol unit tests" ON)
option(IOT_ENABLE_TENSORRT "Compile the native TensorRT backend" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

add_library(iot_core
    src/config.cpp
    src/protocol/gateway_codec.cpp
    src/protocol/sensor_parsers.cpp
    src/protocol/websocket.cpp
    src/services/data_pipeline.cpp
    src/services/database_writer.cpp
    src/services/inference_engine.cpp
    src/services/inference_worker.cpp
    src/services/websocket_server.cpp
    src/services/tcp_server.cpp) # 因为没有明确`STATIC` 或 `SHARE`  其类型由 `BUILD_SHARED_LIBS` 决定。未设置时通常生成静态库：
target_include_directories(iot_core PUBLIC include)
target_link_libraries(iot_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
target_compile_options(iot_core PRIVATE -Wall -Wextra -Wpedantic)

if(IOT_ENABLE_TENSORRT)
  find_package(CUDAToolkit REQUIRED)
  find_path(TENSORRT_INCLUDE_DIR NvInfer.h
    HINTS /usr/include/aarch64-linux-gnu ${CUDAToolkit_INCLUDE_DIRS})
  find_library(TENSORRT_LIBRARY nvinfer HINTS /usr/lib/aarch64-linux-gnu)# 查找 libnvinfer.so 库文件
  if(NOT TENSORRT_INCLUDE_DIR OR NOT TENSORRT_LIBRARY)
    message(FATAL_ERROR "TensorRT headers or library not found")
  endif()
  target_sources(iot_core PRIVATE src/inference/tensorrt_backend.cpp)
  target_include_directories(iot_core PRIVATE
    ${TENSORRT_INCLUDE_DIR}
    ${CUDAToolkit_INCLUDE_DIRS})
  target_link_libraries(iot_core PRIVATE ${TENSORRT_LIBRARY} CUDA::cudart)
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
  find_program(IOT_NODE_EXECUTABLE node)
  if(IOT_NODE_EXECUTABLE)
    add_test(NAME web_frontend_tests
      COMMAND ${IOT_NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/web_frontend_tests.js)
  endif()
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_executable(websocket_fixture tests/websocket_fixture.cpp)
    target_link_libraries(websocket_fixture PRIVATE iot_core)
    add_test(NAME websocket_integration
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/websocket_integration.py
              $<TARGET_FILE:websocket_fixture>)
    set_tests_properties(websocket_integration PROPERTIES TIMEOUT 30)
  endif()
endif()
