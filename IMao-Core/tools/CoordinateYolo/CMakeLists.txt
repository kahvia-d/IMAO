cmake_minimum_required(VERSION 3.24)
project(IMaoCoordinateYolo LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(IMAO_DEPENDENCY_ROOT "${ROOT}" CACHE PATH "Existing dependency checkout")
find_package(OpenCV REQUIRED PATHS "${IMAO_DEPENDENCY_ROOT}/third_party/build/opencv-4.11.0" NO_DEFAULT_PATH)
add_executable(IMaoCoordinateYolo main.cpp
    "${ROOT}/IMao-Core/src/Diagnostics/ReproCapture.cpp"
    "${ROOT}/IMao-Core/src/Coordinate/IdentifyWorldCoordinates/YoloCoordinateRecognizer.cpp"
    "${ROOT}/IMao-Core/src/Coordinate/IdentifyWorldCoordinates/YoloCoordinateDecoder.cpp")
target_include_directories(IMaoCoordinateYolo PRIVATE "${ROOT}/IMao-Core/src" "${ROOT}/IMao-Core/packages/nlohmann" ${OpenCV_INCLUDE_DIRS})
target_compile_options(IMaoCoordinateYolo PRIVATE /source-charset:utf-8 /execution-charset:utf-8 /EHsc /bigobj)
target_compile_definitions(IMaoCoordinateYolo PRIVATE NOMINMAX)
target_link_libraries(IMaoCoordinateYolo PRIVATE ${OpenCV_LIBS})
add_executable(CoordinateHandoffTests "${ROOT}/IMao-Core/tests/CoordinateHandoffTests.cpp")
target_include_directories(CoordinateHandoffTests PRIVATE "${ROOT}/IMao-Core/src")
enable_testing()
add_test(NAME CoordinateHandoffTests COMMAND CoordinateHandoffTests)
add_executable(CoordinateYoloDecoderTests "${ROOT}/IMao-Core/tests/YoloCoordinateDecoderTests.cpp"
    "${ROOT}/IMao-Core/src/Coordinate/IdentifyWorldCoordinates/YoloCoordinateDecoder.cpp")
target_include_directories(CoordinateYoloDecoderTests PRIVATE "${ROOT}/IMao-Core/src" "${ROOT}/IMao-Core/packages/nlohmann" ${OpenCV_INCLUDE_DIRS})
target_compile_definitions(CoordinateYoloDecoderTests PRIVATE NOMINMAX)
target_compile_options(CoordinateYoloDecoderTests PRIVATE /source-charset:utf-8 /execution-charset:utf-8 /EHsc /bigobj)
target_link_libraries(CoordinateYoloDecoderTests PRIVATE ${OpenCV_LIBS})
add_test(NAME CoordinateYoloDecoderTests COMMAND CoordinateYoloDecoderTests)
add_executable(CoordinateContinuityTests "${ROOT}/IMao-Core/tests/CoordinateContinuityTests.cpp"
    "${ROOT}/IMao-Core/src/Coordinate/IdentifyWorldCoordinates/CoordinateRecoveryController.cpp"
    "${ROOT}/IMao-Core/src/App/ScreenMotionTracker.cpp")
target_include_directories(CoordinateContinuityTests PRIVATE "${ROOT}/IMao-Core/src" "${ROOT}/IMao-Core/packages/nlohmann" ${OpenCV_INCLUDE_DIRS})
target_compile_definitions(CoordinateContinuityTests PRIVATE NOMINMAX)
target_compile_options(CoordinateContinuityTests PRIVATE /source-charset:utf-8 /execution-charset:utf-8 /EHsc /bigobj)
target_link_libraries(CoordinateContinuityTests PRIVATE ${OpenCV_LIBS})
add_test(NAME CoordinateContinuityTests COMMAND CoordinateContinuityTests)

add_executable(ReadoutMotionTests "${ROOT}/IMao-Core/tests/ReadoutMotionTests.cpp")
target_include_directories(ReadoutMotionTests PRIVATE "${ROOT}/IMao-Core/src" "${ROOT}/IMao-Core/packages/nlohmann" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(ReadoutMotionTests PRIVATE ${OpenCV_LIBS})
target_compile_definitions(ReadoutMotionTests PRIVATE NOMINMAX)
add_test(NAME ReadoutMotionTests COMMAND ReadoutMotionTests)

add_executable(LocalizationSchedulingTests "${ROOT}/IMao-Core/tests/LocalizationSchedulingTests.cpp")
target_include_directories(LocalizationSchedulingTests PRIVATE "${ROOT}/IMao-Core/src")
target_compile_definitions(LocalizationSchedulingTests PRIVATE NOMINMAX)
add_test(NAME LocalizationSchedulingTests COMMAND LocalizationSchedulingTests)

add_executable(YoloBackendTests "${ROOT}/IMao-Core/tests/YoloBackendTests.cpp"
    "${ROOT}/IMao-Core/src/Diagnostics/ReproCapture.cpp"
    "${ROOT}/IMao-Core/src/Coordinate/IdentifyWorldCoordinates/YoloCoordinateRecognizer.cpp"
    "${ROOT}/IMao-Core/src/Coordinate/IdentifyWorldCoordinates/YoloCoordinateDecoder.cpp")
target_include_directories(YoloBackendTests PRIVATE "${ROOT}/IMao-Core/src" "${ROOT}/IMao-Core/packages/nlohmann" ${OpenCV_INCLUDE_DIRS})
target_compile_options(YoloBackendTests PRIVATE /source-charset:utf-8 /execution-charset:utf-8 /EHsc /bigobj)
target_compile_definitions(YoloBackendTests PRIVATE NOMINMAX)
target_link_libraries(YoloBackendTests PRIVATE ${OpenCV_LIBS})
add_test(NAME YoloBackendTests COMMAND YoloBackendTests "${ROOT}/Assets/models/coordinate-yolo")
