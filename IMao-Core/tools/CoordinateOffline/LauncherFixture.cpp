#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

// Test-only process: exercises the real PowerShell launcher without hotkeys or capture.
int wmain(int argc, wchar_t** argv) {
    std::filesystem::path output;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::wstring(argv[i]) == L"--output") output = argv[i + 1];
    }
    if (argc != 11 || output.empty()) { std::cerr << "bad argument boundaries\n"; return 2; }
    std::filesystem::create_directories(output);
    std::ofstream(output / "fixture-arguments-ok.txt") << "ok";
    const auto name = L"Local\\IMaoCoordinateSamplesStop-" + std::to_wstring(GetCurrentProcessId());
    const auto event = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
    if (!event) return 3;
    std::cout << "Ready (paused).\n" << std::flush;
    const auto wait = WaitForSingleObject(event, 30000);
    CloseHandle(event);
    if (wait != WAIT_OBJECT_0) { std::cerr << "fixture timeout\n"; return 4; }
    if (GetEnvironmentVariableW(L"IMAO_FIXTURE_FAIL", nullptr, 0)) {
        std::cerr << "Capture failed: intentional fixture failure\n";
        return 1;
    }
    std::cout << "Finished: 0 samples, fixture\n";
    return 0;
}
