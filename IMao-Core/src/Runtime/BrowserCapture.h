#pragma once
#include "GameCaptureTarget.h"
#include <nlohmann/json.hpp>
namespace BrowserCapture {
GameCaptureTarget::Selection Parse(const nlohmann::json& value);
nlohmann::json Preview(const nlohmann::json& request);
}
