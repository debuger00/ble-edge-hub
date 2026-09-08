```bash
(base) ➜  002demoCpp git:(main) ✗ cmake -S . -B build
-- The CXX compiler identification is GNU 11.4.0
-- Detecting CXX compiler ABI info
-- Detecting CXX compiler ABI info - done
-- Check for working CXX compiler: /usr/bin/c++ - skipped
-- Detecting CXX compile features
-- Detecting CXX compile features - done
-- Performing Test CMAKE_HAVE_LIBC_PTHREAD
-- Performing Test CMAKE_HAVE_LIBC_PTHREAD - Success
-- Found Threads: TRUE
-- Found CUDAToolkit: /usr/local/cuda/targets/aarch64-linux/include (found version "12.6.68")
-- Configuring done (1.0s)
-- Generating done (0.0s)
-- Build files have been written to: /home/aiam/opt/002demoCpp/build
(base) ➜  002demoCpp git:(main) ✗ cmake --build build
[  6%] Building CXX object CMakeFiles/iot_core.dir/src/config.cpp.o
[ 13%] Building CXX object CMakeFiles/iot_core.dir/src/protocol/gateway_codec.cpp.o
[ 20%] Building CXX object CMakeFiles/iot_core.dir/src/protocol/sensor_parsers.cpp.o
[ 26%] Building CXX object CMakeFiles/iot_core.dir/src/services/data_pipeline.cpp.o
[ 33%] Building CXX object CMakeFiles/iot_core.dir/src/services/database_writer.cpp.o
[ 40%] Building CXX object CMakeFiles/iot_core.dir/src/services/inference_engine.cpp.o
[ 46%] Building CXX object CMakeFiles/iot_core.dir/src/services/inference_worker.cpp.o
[ 53%] Building CXX object CMakeFiles/iot_core.dir/src/services/websocket_server.cpp.o
[ 60%] Building CXX object CMakeFiles/iot_core.dir/src/services/tcp_server.cpp.o
[ 66%] Building CXX object CMakeFiles/iot_core.dir/src/inference/tensorrt_backend.cpp.o
[ 73%] Linking CXX static library libiot_core.a
[ 73%] Built target iot_core
[ 80%] Building CXX object CMakeFiles/ble-iot-edge.dir/src/main.cpp.o
[ 86%] Linking CXX executable ble-iot-edge
[ 86%] Built target ble-iot-edge
[ 93%] Building CXX object CMakeFiles/protocol_tests.dir/tests/protocol_tests.cpp.o
[100%] Linking CXX executable protocol_tests
[100%] Built target protocol_tests
```