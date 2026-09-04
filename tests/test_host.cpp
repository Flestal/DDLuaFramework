#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

int wmain(int argc, wchar_t** argv) {
  if (argc != 3 && argc != 4) {
    std::wcerr << L"usage: ddlua_test_host <proxy.dll> <success|error|syntax> [dependent.dll]\n";
    return 2;
  }
  const fs::path dll_path = fs::absolute(argv[1]);
  const fs::path log_path = dll_path.parent_path() / L"ddlua" / L"logs" / L"ddlua.log";
  DeleteFileW(log_path.c_str());

  HMODULE proxy = LoadLibraryW(dll_path.c_str());
  if (!proxy) {
    std::wcerr << L"LoadLibrary failed: " << GetLastError() << L"\n";
    return 3;
  }
  using GetTime = DWORD(WINAPI*)();
  using GetState = int(WINAPI*)();
  using TestRenderTick = void(WINAPI*)();
  using TestEffectEvent = void(WINAPI*)();
  using TestCorridorEvent = BOOL(WINAPI*)();
  auto get_time = reinterpret_cast<GetTime>(GetProcAddress(proxy, "timeGetTime"));
  auto get_state = reinterpret_cast<GetState>(GetProcAddress(proxy, "DDLua_GetInitializationState"));
  auto test_render_tick = reinterpret_cast<TestRenderTick>(GetProcAddress(proxy, "DDLua_TestRenderTick"));
  auto test_effect_event = reinterpret_cast<TestEffectEvent>(GetProcAddress(proxy, "DDLua_TestEffectEvent"));
  auto test_corridor_event = reinterpret_cast<TestCorridorEvent>(
      GetProcAddress(proxy, "DDLua_TestCorridorEvent"));
  if (!get_time || !get_state || !test_render_tick || !test_effect_event ||
      !test_corridor_event) {
    std::cerr << "required exports missing\n";
    FreeLibrary(proxy);
    return 4;
  }
  const char* sdl_required[] = {
      "waveInAddBuffer", "waveInClose", "waveInGetDevCapsW", "waveInGetNumDevs", "waveInOpen",
      "waveInPrepareHeader", "waveInReset", "waveInStart", "waveInUnprepareHeader", "waveOutClose",
      "waveOutGetDevCapsW", "waveOutGetErrorTextW", "waveOutGetNumDevs", "waveOutOpen",
      "waveOutPrepareHeader", "waveOutReset", "waveOutUnprepareHeader", "waveOutWrite"};
  for (const char* symbol : sdl_required) {
    if (!GetProcAddress(proxy, symbol)) {
      std::cerr << "SDL2-required export missing: " << symbol << "\n";
      FreeLibrary(proxy);
      return 7;
    }
  }
  const char* runtime_required[] = {
      "timeGetDevCaps", "mixerClose", "mixerGetControlDetailsA", "mixerGetDevCapsA", "mixerGetID",
      "mixerGetLineControlsA", "mixerGetLineInfoA", "mixerOpen", "mixerSetControlDetails",
      "waveInMessage", "waveOutMessage"};
  for (const char* symbol : runtime_required) {
    if (!GetProcAddress(proxy, symbol)) {
      std::cerr << "runtime-required export missing: " << symbol << "\n";
      FreeLibrary(proxy);
      return 9;
    }
  }
  const DWORD before = get_time();
  Sleep(2);
  const DWORD after = get_time();
  if (after < before) {
    std::cerr << "forwarded timer moved backwards\n";
    FreeLibrary(proxy);
    return 5;
  }
  HMODULE dependent = nullptr;
  if (argc == 4) {
    dependent = LoadLibraryW(fs::absolute(argv[3]).c_str());
    if (!dependent) {
      std::wcerr << L"dependent DLL load failed: " << GetLastError() << L"\n";
      FreeLibrary(proxy);
      return 8;
    }
  }
  for (int i = 0; i < 100 && get_state() == 1; ++i) Sleep(25);
  const int state = get_state();
  const std::wstring mode = argv[2];
  const bool expect_loaded = mode == L"success" || mode == L"callback";
  const int expected_state = expect_loaded ? 2 : 3;
  if (expect_loaded) {
    test_render_tick();
    test_effect_event();
    const BOOL corridor_ok = test_corridor_event();
    if (mode == L"success" && !corridor_ok) {
      std::cerr << "corridor callback modifiers were not aggregated\n";
      if (dependent) FreeLibrary(dependent);
      FreeLibrary(proxy);
      return 10;
    }
  }

  std::ifstream log(log_path, std::ios::binary);
  const std::string contents((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
  const bool expected_text = mode == L"success" ? contents.find("Lua framework loaded") != std::string::npos
      : mode == L"callback" ? contents.find("callback isolation marker") != std::string::npos
      : mode == L"syntax" ? contents.find("Lua load failed") != std::string::npos
                          : contents.find("intentional runtime error marker") != std::string::npos;
  if (dependent) FreeLibrary(dependent);
  FreeLibrary(proxy);
  const bool callback_text = mode == L"success" ? contents.find("render_tick callback active") != std::string::npos
      : mode != L"callback" || contents.find("Failing callback disabled") != std::string::npos;
  const bool discovery_text = mode != L"success" || contents.find("base mods lua folder discovered") != std::string::npos;
  const bool effect_text = mode != L"success" ||
      (contents.find("test effect before_apply") != std::string::npos &&
       contents.find("test effect after_apply") != std::string::npos);
  const bool corridor_text = mode != L"success" ||
      contents.find("test corridor callback\tbattle\t0.125\t0.0\tfalse") !=
          std::string::npos;
  if (state != expected_state || !expected_text || !callback_text || !discovery_text ||
      !effect_text || !corridor_text) {
    std::cerr << "state=" << state << " expected=" << expected_state << " log:\n" << contents;
    return 6;
  }
  std::cout << "proxy forwarding and Lua " << (expect_loaded ? "loaded callback" : "load error isolation") << " verified\n";
  return 0;
}
