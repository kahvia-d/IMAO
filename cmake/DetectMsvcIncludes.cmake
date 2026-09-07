# CMake can misdecode localized MSVC /showIncludes output when launched by a
# desktop host with a different console encoding. Probe raw bytes and normalize
# only the dependency prefix through a tiny native compiler launcher.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/../tools/MsvcOutput/main.cpp")
    set(_imao_probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/IMaoShowIncludes")
    file(MAKE_DIRECTORY "${_imao_probe_dir}")
    file(WRITE "${_imao_probe_dir}/probe.h" "// Dependency prefix probe\n")
    file(WRITE "${_imao_probe_dir}/probe.cpp" "#include \"probe.h\"\n")
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" /nologo /source-charset:utf-8 /execution-charset:utf-8 /showIncludes /EP /TP probe.cpp
        WORKING_DIRECTORY "${_imao_probe_dir}"
        OUTPUT_VARIABLE _imao_probe_output
        ERROR_VARIABLE _imao_probe_error
        RESULT_VARIABLE _imao_probe_result
        ENCODING NONE)
    string(REGEX MATCH "([^\r\n]*: +)[^\r\n]*probe\\.h" _imao_probe_match "${_imao_probe_output}\n${_imao_probe_error}")
    if(NOT _imao_probe_result EQUAL 0 OR NOT _imao_probe_match)
        message(FATAL_ERROR "Unable to detect MSVC dependency prefix: ${_imao_probe_error}")
    endif()
    string(HEX "${CMAKE_MATCH_1}" _imao_prefix_hex)
    set(_imao_launcher "${_imao_probe_dir}/MsvcOutput.exe")
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" /nologo /EHsc /std:c++17 /O2 /MT
            "${CMAKE_CURRENT_LIST_DIR}/../tools/MsvcOutput/main.cpp"
            "/Fo${_imao_probe_dir}/MsvcOutput.obj" "/Fe${_imao_launcher}"
        WORKING_DIRECTORY "${_imao_probe_dir}"
        OUTPUT_VARIABLE _imao_launcher_output ERROR_VARIABLE _imao_launcher_error
        RESULT_VARIABLE _imao_launcher_result)
    if(NOT _imao_launcher_result EQUAL 0)
        message(FATAL_ERROR "Unable to build MSVC output adapter: ${_imao_launcher_output} ${_imao_launcher_error}")
    endif()
    set(CMAKE_CXX_COMPILER_LAUNCHER "${_imao_launcher};${_imao_prefix_hex}")
    set(CMAKE_C_COMPILER_LAUNCHER "${_imao_launcher};${_imao_prefix_hex}")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "Note: including file: ")
endif()
