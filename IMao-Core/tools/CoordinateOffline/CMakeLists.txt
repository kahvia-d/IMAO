cmake_minimum_required(VERSION 3.24)
project(IMaoCoordinateOffline LANGUAGES CXX)
if(NOT MSVC)
    message(FATAL_ERROR "The offline adapter uses the project's Windows/MSVC Paddle libraries")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
get_filename_component(IMAO_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(IMAO_DEPENDENCY_ROOT "${IMAO_SOURCE_ROOT}" CACHE PATH "Checkout with existing local dependencies")
set(PADDLE_LIB "${IMAO_DEPENDENCY_ROOT}/third_party/paddle-inference-3.0.0" CACHE PATH "Paddle root")
set(OPENCV_DIR "${IMAO_DEPENDENCY_ROOT}/third_party/build/opencv-4.11.0" CACHE PATH "OpenCV build")
find_package(OpenCV REQUIRED PATHS "${OPENCV_DIR}" NO_DEFAULT_PATH)

# Compile the actual preprocessing body without adding a configurable ROI to the runtime.
# A changed extraction boundary fails configuration explicitly instead of using an old copy.
set(_preprocess_source "${IMAO_SOURCE_ROOT}/IMao-Core/src/Coordinate/IdentifyWorldCoordinates/IdentifyWorldCoordinates.cpp")
set(_parser_source "${IMAO_SOURCE_ROOT}/IMao-Core/src/Coordinate/IdentifyWorldCoordinates/CoordinateCandidate.cpp")
set(_adapter_source "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_preprocess_source}" "${_parser_source}" "${_adapter_source}")
file(SHA256 "${_preprocess_source}" _preprocess_sha256)
file(SHA256 "${_parser_source}" _parser_sha256)
file(SHA256 "${_adapter_source}" _adapter_sha256)
set(_path_preprocessingSource "${_preprocess_source}")
set(_path_parserSource "${_parser_source}")
set(_path_nativeAdapterSource "${_adapter_source}")
set(_path_nativeBuildConfiguration "${CMAKE_CURRENT_LIST_FILE}")
set(_path_parserHeader "${IMAO_SOURCE_ROOT}/IMao-Core/src/Coordinate/IdentifyWorldCoordinates/CoordinateCandidate.h")
set(_path_coordinateStructHeader "${IMAO_SOURCE_ROOT}/IMao-Core/src/Coordinate/CoordinateStruct.h")
set(_path_hudLayoutHeader "${IMAO_SOURCE_ROOT}/IMao-Core/src/Coordinate/HudLayout.h")
set(_path_ppocrLibrary "${IMAO_SOURCE_ROOT}/IMao-Core/paddleocr_cpp_infer/lib/ppocr.lib")
set(_path_ppocrHeader "${IMAO_SOURCE_ROOT}/IMao-Core/paddleocr_cpp_infer/include/ocr_rec.h")
set(_path_ppocrPreprocessHeader "${IMAO_SOURCE_ROOT}/IMao-Core/paddleocr_cpp_infer/include/preprocess_op.h")
set(_path_ppocrUtilityHeader "${IMAO_SOURCE_ROOT}/IMao-Core/paddleocr_cpp_infer/include/utility.h")
set(_fingerprint "{}")
foreach(_key preprocessingSource parserSource nativeAdapterSource nativeBuildConfiguration
    parserHeader coordinateStructHeader hudLayoutHeader ppocrLibrary ppocrHeader ppocrPreprocessHeader ppocrUtilityHeader)
    file(SHA256 "${_path_${_key}}" _sha)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_path_${_key}}")
    string(JSON _fingerprint SET "${_fingerprint}" "${_key}" "\"${_sha}\"")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/CoordinateBuildFingerprint.generated.h"
    "// Build-time hashes of the directly used project sources/headers/library.\ninline constexpr const char* kCoordinateBuildFingerprint = R\"IMAO(${_fingerprint})IMAO\";\n")
file(READ "${_preprocess_source}" _source)
string(FIND "${_source}" "std::vector<cv::Mat> PreprocessCoordinateRegion(" _start)
string(FIND "${_source}" "\nclass CoordinateRecognizerRuntime {" _end)
if(_start LESS 0 OR _end LESS _start)
    message(FATAL_ERROR "Cannot locate the production preprocessing body; update the offline extraction boundary")
endif()
math(EXPR _length "${_end}-${_start}")
string(SUBSTRING "${_source}" ${_start} ${_length} _body)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/CoordinatePreprocessing.generated.h" "// Generated from the current runtime source. Do not edit.\n${_body}\n")

add_executable(IMaoCoordinateOffline main.cpp
    "${IMAO_SOURCE_ROOT}/IMao-Core/src/Coordinate/IdentifyWorldCoordinates/CoordinateCandidate.cpp")
target_compile_options(IMaoCoordinateOffline PRIVATE /utf-8 /bigobj)
target_compile_definitions(IMaoCoordinateOffline PRIVATE NOMINMAX USE_MKL STATIC_LIB YAML_CPP_STATIC_DEFINE GOOGLE_GLOG_DLL_DECL=)
target_compile_definitions(IMaoCoordinateOffline PRIVATE
    IMAO_PREPROCESS_SHA256="${_preprocess_sha256}" IMAO_PARSER_SHA256="${_parser_sha256}"
    IMAO_ADAPTER_SHA256="${_adapter_sha256}" IMAO_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}")
target_include_directories(IMaoCoordinateOffline PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}" "${IMAO_SOURCE_ROOT}/IMao-Core/src"
    "${IMAO_DEPENDENCY_ROOT}/IMao-Core/packages/nlohmann"
    "${IMAO_SOURCE_ROOT}/IMao-Core/paddleocr_cpp_infer"
    "${PADDLE_LIB}/paddle/include" ${OpenCV_INCLUDE_DIRS})
foreach(_dependency protobuf glog gflags xxhash yaml-cpp zlib mklml onednn)
    target_include_directories(IMaoCoordinateOffline PRIVATE "${PADDLE_LIB}/third_party/install/${_dependency}/include")
    target_link_directories(IMaoCoordinateOffline PRIVATE "${PADDLE_LIB}/third_party/install/${_dependency}/lib")
endforeach()
target_link_libraries(IMaoCoordinateOffline PRIVATE
    "${IMAO_SOURCE_ROOT}/IMao-Core/paddleocr_cpp_infer/lib/ppocr.lib"
    "${PADDLE_LIB}/paddle/lib/paddle_inference.lib"
    mklml libiomp5md mkldnn glog gflags_static libprotobuf xxhash yaml-cpp
    ${OpenCV_LIBS} shlwapi)
