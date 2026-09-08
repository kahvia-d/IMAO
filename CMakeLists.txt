cmake_minimum_required(VERSION 3.14)
# Embedded resources have explicit dependencies below. cmcldeps would bypass
# the localized compiler-output adapter used by C++ compilation.
set(CMAKE_NINJA_CMCLDEPS_RC 0)
project(IMao-Core CXX C)
include(cmake/DetectMsvcIncludes.cmake)

SET(DEMO_NAME "IMao-CoreHost")

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "IMao-Core requires the MSVC toolchain on Windows.")
endif()

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    set(CMAKE_CONFIGURATION_TYPES "Release" CACHE STRING "" FORCE)
    
    include_external_msproject(
        IMao-WinUI
        ${CMAKE_SOURCE_DIR}/IMao-WinUI/IMao-WinUI.csproj
    )
    
    include_external_msproject(
        IMao-WinUI.Core
        ${CMAKE_SOURCE_DIR}/IMao-WinUI.Core/IMao-WinUI.Core.csproj
    )

    add_dependencies(IMao-WinUI ${DEMO_NAME})
endif()

if(MSVC)
    add_compile_options(/source-charset:utf-8 /execution-charset:utf-8)
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexec-charset=UTF-8")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fexec-charset=UTF-8")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

SET(PADDLE_LIB "" CACHE PATH "Paddle Inference root (contains paddle/ and third_party/)")
SET(OPENCV_DIR "" CACHE PATH "Directory containing OpenCVConfig.cmake")
option(IMAO_ENABLE_DIAGNOSTICS "Write bounded runtime screenshots and event logs for map matching diagnostics" OFF)
option(IMAO_ALLOW_XML_FEATURE_FALLBACK "Allow development builds to fall back to Map_features.yml" OFF)
option(IMAO_GENERATE_LINK_MAP "Write a link map for native crash offset diagnosis" OFF)

macro(safe_set_static_flag)
    foreach(flag_var
        CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
        CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
      if(${flag_var} MATCHES "/MD")
        string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
      endif(${flag_var} MATCHES "/MD")
    endforeach(flag_var)
endmacro()

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    set(CMAKE_CONFIGURATION_TYPES "Release" CACHE STRING "" FORCE)
endif()

ADD_DEFINITIONS(-DUSE_MKL)

if(PADDLE_LIB STREQUAL "" OR NOT EXISTS "${PADDLE_LIB}/paddle/include/paddle_inference_api.h")
  message(FATAL_ERROR "Set PADDLE_LIB to the Paddle Inference root containing paddle/include/paddle_inference_api.h.")
endif()

if(OPENCV_DIR STREQUAL "" OR NOT EXISTS "${OPENCV_DIR}/OpenCVConfig.cmake")
    message(FATAL_ERROR "Set OPENCV_DIR to the directory containing OpenCVConfig.cmake.")
endif()

include_directories("${PADDLE_LIB}/paddle/include")
link_directories("${PADDLE_LIB}/paddle/lib")
find_package(OpenCV REQUIRED PATHS ${OPENCV_DIR} NO_DEFAULT_PATH)
include_directories(${OpenCV_INCLUDE_DIRS})

add_definitions("/DGOOGLE_GLOG_DLL_DECL=")
add_definitions("/DUNICODE" "/D_UNICODE")
set(FLAG_OPENMP "/openmp")
set(CMAKE_C_FLAGS_DEBUG   "${CMAKE_C_FLAGS_DEBUG} /bigobj ${FLAG_OPENMP}")
set(CMAKE_C_FLAGS_RELEASE  "${CMAKE_C_FLAGS_RELEASE} /bigobj ${FLAG_OPENMP}")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} /bigobj ${FLAG_OPENMP}")
set(CMAKE_CXX_FLAGS_DEBUG  "${CMAKE_CXX_FLAGS_DEBUG} /bigobj ${FLAG_OPENMP}")
set(CMAKE_CXX_FLAGS_RELEASE   "${CMAKE_CXX_FLAGS_RELEASE} /bigobj ${FLAG_OPENMP}")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} /bigobj ${FLAG_OPENMP}")

safe_set_static_flag()
add_definitions(-DSTATIC_LIB)
add_definitions(-DYAML_CPP_STATIC_DEFINE)

message("cmake c debug flags " ${CMAKE_C_FLAGS_DEBUG})
message("cmake c release flags " ${CMAKE_C_FLAGS_RELEASE})
message("cmake cxx debug flags " ${CMAKE_CXX_FLAGS_DEBUG})
message("cmake cxx release flags " ${CMAKE_CXX_FLAGS_RELEASE})

include_directories("${PADDLE_LIB}/third_party/install/protobuf/include")
include_directories("${PADDLE_LIB}/third_party/install/glog/include")
include_directories("${PADDLE_LIB}/third_party/install/gflags/include")
include_directories("${PADDLE_LIB}/third_party/install/xxhash/include")
include_directories("${PADDLE_LIB}/third_party/install/zlib/include")
include_directories("${PADDLE_LIB}/third_party/install/onnxruntime/include")
include_directories("${PADDLE_LIB}/third_party/install/paddle2onnx/include")
include_directories("${PADDLE_LIB}/third_party/install/yaml-cpp/include")
include_directories("${PADDLE_LIB}/third_party/boost")
include_directories("${PADDLE_LIB}/third_party/eigen3")
include_directories("${CMAKE_SOURCE_DIR}/IMao-Core/packages/nlohmann")
include_directories("${CMAKE_SOURCE_DIR}/IMao-Core/packages/Microsoft.Windows.ImplementationLibrary.1.0.240803.1/include")
include_directories("${CMAKE_SOURCE_DIR}/IMao-Core/packages/robmikh.common.0.0.23-beta/include")
include_directories("${CMAKE_SOURCE_DIR}/IMao-Core/paddleocr_cpp_infer")

include_directories("${PADDLE_LIB}/third_party/install/mklml/include")
set(MATH_LIB 
    ${PADDLE_LIB}/third_party/install/mklml/lib/mklml.lib
    ${PADDLE_LIB}/third_party/install/mklml/lib/libiomp5md.lib
)

set(MKLDNN_PATH "${PADDLE_LIB}/third_party/install/onednn")
if(EXISTS ${MKLDNN_PATH})
    include_directories("${MKLDNN_PATH}/include")
    set(MKLDNN_LIB ${MKLDNN_PATH}/lib/mkldnn.lib)
endif()

link_directories("${PADDLE_LIB}/third_party/install/zlib/lib")
link_directories("${PADDLE_LIB}/third_party/install/protobuf/lib")
link_directories("${PADDLE_LIB}/third_party/install/glog/lib")
link_directories("${PADDLE_LIB}/third_party/install/gflags/lib")
link_directories("${PADDLE_LIB}/third_party/install/xxhash/lib")
link_directories("${PADDLE_LIB}/third_party/install/onnxruntime/lib")
link_directories("${PADDLE_LIB}/third_party/install/paddle2onnx/lib")
link_directories("${PADDLE_LIB}/third_party/install/yaml-cpp/lib")
link_directories("${PADDLE_LIB}/paddle/lib")

set(DEPS
    ${PADDLE_LIB}/paddle/lib/paddle_inference${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${MATH_LIB} ${MKLDNN_LIB}
    glog gflags_static libprotobuf xxhash
    libcmt shlwapi
)

Set(DEPS ${DEPS} ${CMAKE_SOURCE_DIR}/IMao-Core/paddleocr_cpp_infer/lib/ppocr.lib)
Set(DEPS ${DEPS} Windowscodecs.lib windowsapp.lib dwmapi.lib d3d11.lib shlwapi.lib bcrypt.lib)
set(DEPS ${DEPS} ${OpenCV_LIBS})

if (EXISTS "${PADDLE_LIB}/third_party/install/snappy/lib")
    set(DEPS ${DEPS} snappy)
endif()
if(EXISTS "${PADDLE_LIB}/third_party/install/snappystream/lib")
    set(DEPS ${DEPS} snappystream)
endif()

if (EXISTS "${PADDLE_LIB}/third_party/install/yaml-cpp/lib")
  set(DEPS ${DEPS} yaml-cpp)
endif()

file(GLOB_RECURSE CppFiles CONFIGURE_DEPENDS
    "IMao-Core/src/*.cpp"
    "IMao-Core/src/App/*.cpp"
    "IMao-Core/src/Base/*.cpp"
    "IMao-Core/src/Coordinate/*.cpp"
    "IMao-Core/src/Feature/*.cpp"
    "IMao-Core/src/ImageProcessing/*.cpp"
    "IMao-Core/src/ImguiDraw/*.cpp"
    "IMao-Core/src/WindowsCapture/*.cpp"
)

file(GLOB_RECURSE HppFiles CONFIGURE_DEPENDS
    "IMao-Core/src/*.h"
    "IMao-Core/src/App/*.h"
    "IMao-Core/src/Base/*.h"
    "IMao-Core/src/Coordinate/*.h"
    "IMao-Core/src/Feature/*.h"
    "IMao-Core/src/ImageProcessing/*.h"
    "IMao-Core/src/ImguiDraw/*.h"
    "IMao-Core/src/WindowsCapture/*.h"
)

file(GLOB_RECURSE ResourceFiles CONFIGURE_DEPENDS
    "IMao-Core/src/*.rc"
    "IMao-Core/src/Resource/*.json"
)
list(FILTER CppFiles EXCLUDE REGEX "[/\\\\]CoreHost[/\\\\].*\\.cpp$")
# Main.cpp is an old standalone debug entry point.  The production executable
# uses CoreHostMain.cpp exclusively so it owns the pipe lifecycle.
list(FILTER CppFiles EXCLUDE REGEX "[/\\\\]Main\\.cpp$")

# rc.exe does not reliably report the JSON files referenced by IMao-Core.rc
# as Ninja dependencies. Keep synchronized map snapshots from producing a
# stale DLL when only an embedded JSON resource changes.
set_property(SOURCE "${CMAKE_SOURCE_DIR}/IMao-Core/src/IMao-Core.rc" APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/IMao-Core/src/Resource/itemsData_World.json"
    "${CMAKE_SOURCE_DIR}/IMao-Core/src/Resource/itemsData_Tethys.json"
    "${CMAKE_SOURCE_DIR}/IMao-Core/src/Resource/itemsData_Fabricatorium.json"
    "${CMAKE_SOURCE_DIR}/IMao-Core/src/Resource/itemsData_Avinoleum.json"
    "${CMAKE_SOURCE_DIR}/IMao-Core/src/Resource/itemsData_Lahai.json"
)
file(GLOB_RECURSE EmbeddedResourceInputs CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/IMao-Core/src/Resource/*")
set_property(SOURCE "${CMAKE_SOURCE_DIR}/IMao-Core/src/IMao-Core.rc" APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/IMao-Core/src/resource.h" ${EmbeddedResourceInputs})

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/IMao-Core/src PREFIX "" FILES ${CppFiles} ${HppFiles} ${ResourceFiles})

add_executable(${DEMO_NAME}
    ${CppFiles} ${HppFiles} ${ResourceFiles}
    "${CMAKE_SOURCE_DIR}/IMao-Core/src/CoreHost/CoreHostMain.cpp"
)

if(IMAO_ENABLE_DIAGNOSTICS)
    target_compile_definitions(${DEMO_NAME} PRIVATE IMAO_ENABLE_DIAGNOSTICS=1)
endif()

if(IMAO_ALLOW_XML_FEATURE_FALLBACK OR IMAO_ENABLE_DIAGNOSTICS)
    target_compile_definitions(${DEMO_NAME} PRIVATE IMAO_ALLOW_XML_FEATURE_FALLBACK=1)
endif()

set_target_properties(${DEMO_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/x64/$<CONFIG> 
)

target_include_directories(${DEMO_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(${DEMO_NAME} ${DEPS})

if(IMAO_GENERATE_LINK_MAP)
    target_link_options(${DEMO_NAME} PRIVATE "/MAP:${CMAKE_SOURCE_DIR}/x64/$<CONFIG>/IMao-CoreHost.map")
endif()

# Builds a small, reproducible helper for converting Kuro's public 1024px map
# tiles directly into this project's World-coordinate SURF database.  Keeping
# this independent from IMao-Core avoids the old temporary 40k x 40k mosaic.
add_executable(KuroMapFeatureBuilder
    tools/KuroMapFeatureBuilder/main.cpp
)
set_target_properties(KuroMapFeatureBuilder PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>
)
target_link_libraries(KuroMapFeatureBuilder PRIVATE ${OpenCV_LIBS} bcrypt.lib)

# Standalone game-capture helper for collecting calibration pairs and short
# movement clips.  It reuses the same BitBlt/PrintWindow capture route as the
# overlay, but deliberately does not initialize OCR, feature matching or the
# overlay itself.
add_executable(IMaoCaptureAssistant
    tools/CaptureAssistant/main.cpp
    IMao-Core/src/WindowsCapture/BitBltCapture/BitBltCapture.cpp
)
set_target_properties(IMaoCaptureAssistant PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>
)
target_include_directories(IMaoCaptureAssistant PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/IMao-Core/src)
target_link_libraries(IMaoCaptureAssistant PRIVATE ${OpenCV_LIBS} user32.lib gdi32.lib mfplat.lib mfreadwrite.lib mfuuid.lib)

add_executable(IMaoFeatureConverter
    IMao-Core/tools/FeatureConverter/main.cpp
    IMao-Core/src/Feature/Processing/FeatureBinaryCodec.cpp
    IMao-Core/src/Feature/Processing/FeatureProcessing.cpp
)
set_target_properties(IMaoFeatureConverter PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>
)
target_include_directories(IMaoFeatureConverter PRIVATE
    ${CMAKE_SOURCE_DIR}/IMao-Core/src
    ${CMAKE_SOURCE_DIR}/IMao-Core/packages/nlohmann
)
target_link_libraries(IMaoFeatureConverter PRIVATE ${OpenCV_LIBS} bcrypt.lib windowsapp.lib psapi.lib)

add_executable(IMaoVisualIndexBuilder
    IMao-Core/tools/VisualIndexBuilder/main.cpp
    IMao-Core/src/Feature/VisualIndex/MapVisualIndex.cpp
    IMao-Core/src/Feature/Processing/FeatureBinaryCodec.cpp
    IMao-Core/src/Feature/Processing/FeatureProcessing.cpp
    IMao-Core/src/Feature/Match/FeatureMatch.cpp
    IMao-Core/src/Feature/KuroTileFeaturePack.cpp
    IMao-Core/src/Feature/CandidateFeaturePack.cpp
    IMao-Core/src/Coordinate/locationCalculator/MapCoordinate.cpp
    IMao-Core/src/Coordinate/locationCalculator/RelativeCoordinates.cpp
    IMao-Core/src/Coordinate/locationCalculator/ScreenCoordinate.cpp
    IMao-Core/src/ImageProcessing/ImageProcessing.cpp
    IMao-Core/src/util.cpp
)
set_target_properties(IMaoVisualIndexBuilder PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>
)
target_include_directories(IMaoVisualIndexBuilder PRIVATE
    ${CMAKE_SOURCE_DIR}/IMao-Core/src
    ${CMAKE_SOURCE_DIR}/IMao-Core/packages/nlohmann
)
target_link_libraries(IMaoVisualIndexBuilder PRIVATE ${OpenCV_LIBS} bcrypt.lib windowsapp.lib dwmapi.lib psapi.lib)

add_executable(IMaoOptimizationTests
    IMao-Core/tests/OptimizationTests.cpp
    IMao-Core/src/App/ScreenMotionTracker.cpp
    IMao-Core/src/App/MapUiStateController.cpp
    IMao-Core/src/App/MapUiVisualDetector.cpp
    IMao-Core/src/App/MapViewportPredictor.cpp
    IMao-Core/src/App/WorldSearchPrior.cpp
    IMao-Core/src/Coordinate/IdentifyWorldCoordinates/CoordinateCandidate.cpp
    IMao-Core/src/Coordinate/IdentifyWorldCoordinates/CoordinateRecoveryController.cpp
    IMao-Core/src/Feature/Processing/FeatureBinaryCodec.cpp
    IMao-Core/src/Feature/VisualIndex/MapVisualIndex.cpp
)
set_target_properties(IMaoOptimizationTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>
)
target_include_directories(IMaoOptimizationTests PRIVATE ${CMAKE_SOURCE_DIR}/IMao-Core/src)
target_compile_definitions(IMaoOptimizationTests PRIVATE IMAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(IMaoOptimizationTests PRIVATE ${OpenCV_LIBS} bcrypt.lib windowsapp.lib)

enable_testing()
add_test(NAME IMaoOptimizationTests COMMAND IMaoOptimizationTests)

add_executable(IMaoMarkerTests IMao-Core/tests/MarkerTests.cpp)
target_include_directories(IMaoMarkerTests PRIVATE
    ${CMAKE_SOURCE_DIR}/IMao-Core/src
    ${CMAKE_SOURCE_DIR}/IMao-Core/packages/nlohmann)
set_target_properties(IMaoMarkerTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>)
add_test(NAME IMaoMarkerTests COMMAND IMaoMarkerTests)

add_executable(IMaoRoutePlanningTests IMao-Core/tests/RoutePlanningTests.cpp)
target_include_directories(IMaoRoutePlanningTests PRIVATE
    ${CMAKE_SOURCE_DIR}/IMao-Core/src
    ${CMAKE_SOURCE_DIR}/IMao-Core/packages/nlohmann)
set_target_properties(IMaoRoutePlanningTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>)
add_test(NAME IMaoRoutePlanningTests COMMAND IMaoRoutePlanningTests)

add_executable(IMaoVisualRegression
    IMao-Core/tools/VisualRegression/main.cpp
    IMao-Core/src/App/MapViewportLocalizer.cpp
    IMao-Core/src/Coordinate/VisualLocalization/GlobalVisualLocalizer.cpp
    IMao-Core/src/Diagnostics/Diagnostics.cpp
    IMao-Core/src/Runtime/StructuredLogger.cpp
    IMao-Core/src/Feature/VisualIndex/MapVisualIndex.cpp
    IMao-Core/src/Feature/Processing/FeatureBinaryCodec.cpp
    IMao-Core/src/Feature/Processing/FeatureProcessing.cpp
    IMao-Core/src/Feature/Match/FeatureMatch.cpp
    IMao-Core/src/Feature/KuroTileFeaturePack.cpp
    IMao-Core/src/Feature/CandidateFeaturePack.cpp
    IMao-Core/src/Coordinate/locationCalculator/MapCoordinate.cpp
    IMao-Core/src/Coordinate/locationCalculator/RelativeCoordinates.cpp
    IMao-Core/src/Coordinate/locationCalculator/ScreenCoordinate.cpp
    IMao-Core/src/ImageProcessing/ImageProcessing.cpp
    IMao-Core/src/util.cpp
)
set_target_properties(IMaoVisualRegression PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>
)
target_include_directories(IMaoVisualRegression PRIVATE
    ${CMAKE_SOURCE_DIR}/IMao-Core/src
    ${CMAKE_SOURCE_DIR}/IMao-Core/packages/nlohmann
)
target_link_libraries(IMaoVisualRegression PRIVATE ${OpenCV_LIBS} bcrypt.lib windowsapp.lib dwmapi.lib psapi.lib)

add_executable(IMaoCoordinateRegression
    IMao-Core/tools/CoordinateRegression/main.cpp
    IMao-Core/src/Coordinate/IdentifyWorldCoordinates/IdentifyWorldCoordinates.cpp
    IMao-Core/src/Coordinate/IdentifyWorldCoordinates/CoordinateCandidate.cpp
    IMao-Core/src/Diagnostics/Diagnostics.cpp
    IMao-Core/src/Runtime/StructuredLogger.cpp
    IMao-Core/src/util.cpp
)
set_target_properties(IMaoCoordinateRegression PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>
)
target_include_directories(IMaoCoordinateRegression PRIVATE ${CMAKE_SOURCE_DIR}/IMao-Core/src)
target_link_libraries(IMaoCoordinateRegression PRIVATE ${DEPS})

add_custom_command(TARGET ${DEMO_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DIMAO_SOURCE_ASSETS=${CMAKE_SOURCE_DIR}/Assets
            -DIMAO_DESTINATION_ROOT=${CMAKE_SOURCE_DIR}/x64/$<CONFIG>
            -DIMAO_CONFIGURATION=$<CONFIG>
            -P ${CMAKE_SOURCE_DIR}/cmake/StageAssets.cmake
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PADDLE_LIB}/third_party/install/mklml/lib/mklml.dll ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>/mklml.dll
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PADDLE_LIB}/third_party/install/mklml/lib/libiomp5md.dll ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>/libiomp5md.dll
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PADDLE_LIB}/third_party/install/onednn/lib/mkldnn.dll ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>/mkldnn.dll
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PADDLE_LIB}/paddle/lib/paddle_inference.dll ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>/paddle_inference.dll
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PADDLE_LIB}/paddle/lib/common.dll ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>/common.dll
)
