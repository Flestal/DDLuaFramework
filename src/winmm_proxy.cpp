#include "core.hpp"

#define _WINMM_
#include <windows.h>
#include <mmsystem.h>

namespace {

HMODULE g_proxy_module = nullptr;
INIT_ONCE g_winmm_once = INIT_ONCE_STATIC_INIT;
HMODULE g_real_winmm = nullptr;

using TimeBeginPeriod = decltype(&::timeBeginPeriod);
using TimeEndPeriod = decltype(&::timeEndPeriod);
using TimeGetTime = DWORD(WINAPI*)();
TimeBeginPeriod g_time_begin_period = nullptr;
TimeEndPeriod g_time_end_period = nullptr;
TimeGetTime g_time_get_time = nullptr;

#define DECLARE_WINMM_PROC(name) decltype(&::name) g_##name = nullptr
DECLARE_WINMM_PROC(timeGetDevCaps);
DECLARE_WINMM_PROC(mixerClose);
DECLARE_WINMM_PROC(mixerGetControlDetailsA);
DECLARE_WINMM_PROC(mixerGetDevCapsA);
DECLARE_WINMM_PROC(mixerGetID);
DECLARE_WINMM_PROC(mixerGetLineControlsA);
DECLARE_WINMM_PROC(mixerGetLineInfoA);
DECLARE_WINMM_PROC(mixerOpen);
DECLARE_WINMM_PROC(mixerSetControlDetails);
DECLARE_WINMM_PROC(waveInClose);
DECLARE_WINMM_PROC(waveInOpen);
DECLARE_WINMM_PROC(waveInGetDevCapsW);
DECLARE_WINMM_PROC(waveInGetNumDevs);
DECLARE_WINMM_PROC(waveInPrepareHeader);
DECLARE_WINMM_PROC(waveInUnprepareHeader);
DECLARE_WINMM_PROC(waveInAddBuffer);
DECLARE_WINMM_PROC(waveInStart);
DECLARE_WINMM_PROC(waveInReset);
DECLARE_WINMM_PROC(waveInMessage);
DECLARE_WINMM_PROC(waveOutClose);
DECLARE_WINMM_PROC(waveOutOpen);
DECLARE_WINMM_PROC(waveOutGetDevCapsW);
DECLARE_WINMM_PROC(waveOutGetErrorTextW);
DECLARE_WINMM_PROC(waveOutGetNumDevs);
DECLARE_WINMM_PROC(waveOutPrepareHeader);
DECLARE_WINMM_PROC(waveOutUnprepareHeader);
DECLARE_WINMM_PROC(waveOutReset);
DECLARE_WINMM_PROC(waveOutWrite);
DECLARE_WINMM_PROC(waveOutMessage);
#undef DECLARE_WINMM_PROC

BOOL CALLBACK load_real_winmm(PINIT_ONCE, PVOID, PVOID*) {
  wchar_t system_directory[MAX_PATH]{};
  const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
  if (!length || length >= MAX_PATH - 10) return FALSE;
  wcscat_s(system_directory, L"\\winmm.dll");
  g_real_winmm = LoadLibraryExW(system_directory, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!g_real_winmm) return FALSE;
  g_time_begin_period = reinterpret_cast<TimeBeginPeriod>(GetProcAddress(g_real_winmm, "timeBeginPeriod"));
  g_time_end_period = reinterpret_cast<TimeEndPeriod>(GetProcAddress(g_real_winmm, "timeEndPeriod"));
  g_time_get_time = reinterpret_cast<TimeGetTime>(GetProcAddress(g_real_winmm, "timeGetTime"));
#define RESOLVE_WINMM_PROC(name) g_##name = reinterpret_cast<decltype(g_##name)>(GetProcAddress(g_real_winmm, #name))
  RESOLVE_WINMM_PROC(timeGetDevCaps);
  RESOLVE_WINMM_PROC(mixerClose);
  RESOLVE_WINMM_PROC(mixerGetControlDetailsA);
  RESOLVE_WINMM_PROC(mixerGetDevCapsA);
  RESOLVE_WINMM_PROC(mixerGetID);
  RESOLVE_WINMM_PROC(mixerGetLineControlsA);
  RESOLVE_WINMM_PROC(mixerGetLineInfoA);
  RESOLVE_WINMM_PROC(mixerOpen);
  RESOLVE_WINMM_PROC(mixerSetControlDetails);
  RESOLVE_WINMM_PROC(waveInClose);
  RESOLVE_WINMM_PROC(waveInOpen);
  RESOLVE_WINMM_PROC(waveInGetDevCapsW);
  RESOLVE_WINMM_PROC(waveInGetNumDevs);
  RESOLVE_WINMM_PROC(waveInPrepareHeader);
  RESOLVE_WINMM_PROC(waveInUnprepareHeader);
  RESOLVE_WINMM_PROC(waveInAddBuffer);
  RESOLVE_WINMM_PROC(waveInStart);
  RESOLVE_WINMM_PROC(waveInReset);
  RESOLVE_WINMM_PROC(waveInMessage);
  RESOLVE_WINMM_PROC(waveOutClose);
  RESOLVE_WINMM_PROC(waveOutOpen);
  RESOLVE_WINMM_PROC(waveOutGetDevCapsW);
  RESOLVE_WINMM_PROC(waveOutGetErrorTextW);
  RESOLVE_WINMM_PROC(waveOutGetNumDevs);
  RESOLVE_WINMM_PROC(waveOutPrepareHeader);
  RESOLVE_WINMM_PROC(waveOutUnprepareHeader);
  RESOLVE_WINMM_PROC(waveOutReset);
  RESOLVE_WINMM_PROC(waveOutWrite);
  RESOLVE_WINMM_PROC(waveOutMessage);
#undef RESOLVE_WINMM_PROC
  return g_time_begin_period && g_time_end_period && g_time_get_time && g_timeGetDevCaps &&
      g_mixerClose && g_mixerGetControlDetailsA && g_mixerGetDevCapsA && g_mixerGetID &&
      g_mixerGetLineControlsA && g_mixerGetLineInfoA && g_mixerOpen && g_mixerSetControlDetails &&
      g_waveInClose && g_waveInOpen && g_waveInGetDevCapsW && g_waveInGetNumDevs &&
      g_waveInPrepareHeader && g_waveInUnprepareHeader && g_waveInAddBuffer && g_waveInStart &&
      g_waveInReset && g_waveInMessage && g_waveOutClose && g_waveOutOpen && g_waveOutGetDevCapsW &&
      g_waveOutGetErrorTextW && g_waveOutGetNumDevs && g_waveOutPrepareHeader &&
      g_waveOutUnprepareHeader && g_waveOutReset && g_waveOutWrite && g_waveOutMessage;
}

bool ready() {
  PVOID ignored = nullptr;
  const bool loaded = InitOnceExecuteOnce(&g_winmm_once, load_real_winmm, nullptr, &ignored) != FALSE;
  if (loaded) ddlua::start_async(g_proxy_module);
  return loaded;
}

} // namespace

extern "C" MMRESULT WINAPI timeBeginPeriod(UINT period) {
  return ready() ? g_time_begin_period(period) : TIMERR_NOCANDO;
}

extern "C" MMRESULT WINAPI timeEndPeriod(UINT period) {
  return ready() ? g_time_end_period(period) : TIMERR_NOCANDO;
}

extern "C" DWORD WINAPI timeGetTime() {
  return ready() ? g_time_get_time() : GetTickCount();
}

extern "C" MMRESULT WINAPI timeGetDevCaps(LPTIMECAPS caps, UINT caps_size) {
  return ready() ? g_timeGetDevCaps(caps, caps_size) : TIMERR_NOCANDO;
}

#define FORWARD_MMRESULT_1(name, t1, a1) \
  extern "C" MMRESULT WINAPI name(t1 a1) { return ready() ? g_##name(a1) : MMSYSERR_NODRIVER; }
#define FORWARD_MMRESULT_2(name, t1, a1, t2, a2) \
  extern "C" MMRESULT WINAPI name(t1 a1, t2 a2) { return ready() ? g_##name(a1, a2) : MMSYSERR_NODRIVER; }
#define FORWARD_MMRESULT_3(name, t1, a1, t2, a2, t3, a3) \
  extern "C" MMRESULT WINAPI name(t1 a1, t2 a2, t3 a3) { return ready() ? g_##name(a1, a2, a3) : MMSYSERR_NODRIVER; }
#define FORWARD_MMRESULT_4(name, t1, a1, t2, a2, t3, a3, t4, a4) \
  extern "C" MMRESULT WINAPI name(t1 a1, t2 a2, t3 a3, t4 a4) { return ready() ? g_##name(a1, a2, a3, a4) : MMSYSERR_NODRIVER; }

FORWARD_MMRESULT_1(mixerClose, HMIXER, handle)
FORWARD_MMRESULT_3(mixerGetControlDetailsA, HMIXEROBJ, object, LPMIXERCONTROLDETAILS, details, DWORD, flags)
FORWARD_MMRESULT_3(mixerGetDevCapsA, UINT_PTR, device_id, LPMIXERCAPSA, caps, UINT, caps_size)
FORWARD_MMRESULT_3(mixerGetID, HMIXEROBJ, object, UINT*, mixer_id, DWORD, flags)
FORWARD_MMRESULT_3(mixerGetLineControlsA, HMIXEROBJ, object, LPMIXERLINECONTROLSA, controls, DWORD, flags)
FORWARD_MMRESULT_3(mixerGetLineInfoA, HMIXEROBJ, object, LPMIXERLINEA, line, DWORD, flags)
extern "C" MMRESULT WINAPI mixerOpen(LPHMIXER handle, UINT device_id, DWORD_PTR callback,
    DWORD_PTR instance, DWORD flags) {
  return ready() ? g_mixerOpen(handle, device_id, callback, instance, flags) : MMSYSERR_NODRIVER;
}
FORWARD_MMRESULT_3(mixerSetControlDetails, HMIXEROBJ, object, LPMIXERCONTROLDETAILS, details, DWORD, flags)

FORWARD_MMRESULT_1(waveInClose, HWAVEIN, handle)
FORWARD_MMRESULT_3(waveInGetDevCapsW, UINT_PTR, device_id, LPWAVEINCAPSW, caps, UINT, caps_size)
extern "C" UINT WINAPI waveInGetNumDevs() { return ready() ? g_waveInGetNumDevs() : 0; }
extern "C" MMRESULT WINAPI waveInOpen(LPHWAVEIN handle, UINT device_id, LPCWAVEFORMATEX format,
    DWORD_PTR callback, DWORD_PTR instance, DWORD flags) {
  return ready() ? g_waveInOpen(handle, device_id, format, callback, instance, flags) : MMSYSERR_NODRIVER;
}
FORWARD_MMRESULT_3(waveInPrepareHeader, HWAVEIN, handle, LPWAVEHDR, header, UINT, header_size)
FORWARD_MMRESULT_3(waveInUnprepareHeader, HWAVEIN, handle, LPWAVEHDR, header, UINT, header_size)
FORWARD_MMRESULT_3(waveInAddBuffer, HWAVEIN, handle, LPWAVEHDR, header, UINT, header_size)
FORWARD_MMRESULT_1(waveInStart, HWAVEIN, handle)
FORWARD_MMRESULT_1(waveInReset, HWAVEIN, handle)
FORWARD_MMRESULT_4(waveInMessage, HWAVEIN, handle, UINT, message, DWORD_PTR, param1, DWORD_PTR, param2)
FORWARD_MMRESULT_1(waveOutClose, HWAVEOUT, handle)
FORWARD_MMRESULT_3(waveOutGetDevCapsW, UINT_PTR, device_id, LPWAVEOUTCAPSW, caps, UINT, caps_size)
FORWARD_MMRESULT_3(waveOutGetErrorTextW, MMRESULT, error, LPWSTR, text, UINT, text_size)
extern "C" UINT WINAPI waveOutGetNumDevs() { return ready() ? g_waveOutGetNumDevs() : 0; }
extern "C" MMRESULT WINAPI waveOutOpen(LPHWAVEOUT handle, UINT device_id, LPCWAVEFORMATEX format,
    DWORD_PTR callback, DWORD_PTR instance, DWORD flags) {
  return ready() ? g_waveOutOpen(handle, device_id, format, callback, instance, flags) : MMSYSERR_NODRIVER;
}
FORWARD_MMRESULT_3(waveOutPrepareHeader, HWAVEOUT, handle, LPWAVEHDR, header, UINT, header_size)
FORWARD_MMRESULT_3(waveOutUnprepareHeader, HWAVEOUT, handle, LPWAVEHDR, header, UINT, header_size)
FORWARD_MMRESULT_1(waveOutReset, HWAVEOUT, handle)
FORWARD_MMRESULT_3(waveOutWrite, HWAVEOUT, handle, LPWAVEHDR, header, UINT, header_size)
FORWARD_MMRESULT_4(waveOutMessage, HWAVEOUT, handle, UINT, message, DWORD_PTR, param1, DWORD_PTR, param2)

#undef FORWARD_MMRESULT_1
#undef FORWARD_MMRESULT_2
#undef FORWARD_MMRESULT_3
#undef FORWARD_MMRESULT_4

extern "C" int WINAPI DDLua_GetInitializationState() { return ddlua::initialization_state(); }
extern "C" void WINAPI DDLua_TestRenderTick() { ddlua::test_render_tick(); }
extern "C" void WINAPI DDLua_TestEffectEvent() { ddlua::test_effect_event(); }
extern "C" BOOL WINAPI DDLua_TestCorridorEvent() {
  return ddlua::test_corridor_event() ? TRUE : FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_proxy_module = instance;
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}
