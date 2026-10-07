#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

// A path is not text. Windows narrow file APIs read and write the process code page, so path.string()
// is the right call immediately before a file is opened, and converting those call sites to UTF-8
// would break them - a Chinese directory would stop opening its own files.
//
// Every string that leaves this process as JSON - a log record, a crash report, an IPC response - has
// to be UTF-8 instead. A Chinese installation directory turned that difference into a fatal error: the
// warning "feature binary cannot be opened: <path>" carried GBK bytes, nlohmann refused to serialize
// them with type_error.316, and the exception escaped the logger, so one diagnostic line disabled the
// whole resource set. Convert at the boundary with Utf8Text and keep the serializer lossy as well,
// because a diagnostic must never fail over the bytes it is reporting on.
//
// AutoRoute::Utf8Path/Utf8Text are the route bundle's own pair for the one place a path arrives from
// the interface as UTF-8; they are deliberately left alone so a bug fix does not touch route storage.
inline std::string Utf8Text(const std::filesystem::path& value) {
    const auto text = value.u8string();
    return { text.begin(), text.end() };
}

inline std::string DumpJsonText(const nlohmann::json& value, int indent = -1) {
    return value.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}
