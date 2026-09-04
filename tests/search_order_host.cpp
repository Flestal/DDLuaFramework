#include <windows.h>
#include <mmsystem.h>

#include <iostream>

extern "C" __declspec(dllimport) int WINAPI DDLua_GetInitializationState();

int main() {
  const DWORD before = timeGetTime();
  Sleep(2);
  const DWORD after = timeGetTime();
  if (after < before) {
    std::cerr << "forwarded timer moved backwards\n";
    return 2;
  }
  for (int i = 0; i < 100 && DDLua_GetInitializationState() == 1; ++i) Sleep(25);
  if (DDLua_GetInitializationState() != 2) {
    std::cerr << "application-directory proxy did not initialize successfully\n";
    return 3;
  }
  std::cout << "application-directory winmm proxy selected and initialized\n";
  return 0;
}

