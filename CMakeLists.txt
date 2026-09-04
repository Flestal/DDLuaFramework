cmake_minimum_required(VERSION 3.24)
project(DDLuaFramework VERSION 0.6.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "DDLuaFramework milestone 1 supports only 64-bit Windows")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(LUA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua-5.4.9/src")
file(GLOB LUA_SOURCES CONFIGURE_DEPENDS "${LUA_DIR}/*.c")
list(REMOVE_ITEM LUA_SOURCES "${LUA_DIR}/lua.c" "${LUA_DIR}/luac.c")

add_library(lua54 STATIC ${LUA_SOURCES})
target_include_directories(lua54 PUBLIC "${LUA_DIR}")
set_target_properties(lua54 PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(ddlua_core STATIC
  src/core.cpp
  src/core.hpp
  src/game_bridge.cpp
  src/game_bridge.hpp
  src/render_hook.cpp
  src/render_hook.hpp
  src/tracer.cpp
  src/tracer.hpp
)
target_include_directories(ddlua_core PUBLIC src)
target_link_libraries(ddlua_core PUBLIC lua54 bcrypt)
target_compile_definitions(ddlua_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_library(winmm_proxy SHARED
  src/winmm_proxy.cpp
  src/winmm_proxy.def
)
target_link_libraries(winmm_proxy PRIVATE ddlua_core)
target_compile_definitions(winmm_proxy PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(winmm_proxy PROPERTIES OUTPUT_NAME winmm PREFIX "")

add_executable(ddlua_test_host tests/test_host.cpp)
target_compile_definitions(ddlua_test_host PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(ddlua_search_order_host tests/search_order_host.cpp)
target_link_libraries(ddlua_search_order_host PRIVATE winmm_proxy)
target_compile_definitions(ddlua_search_order_host PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

set(STAGE_DIR "${CMAKE_BINARY_DIR}/stage")
add_custom_command(TARGET winmm_proxy POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${STAGE_DIR}/good/ddlua/config"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${STAGE_DIR}/bad/ddlua/config"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${STAGE_DIR}/syntax/ddlua/config"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${STAGE_DIR}/callback/ddlua/config"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${STAGE_DIR}/good/mods/test_mod/lua"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${STAGE_DIR}/good/mods/FlestalTrinkets/lua"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:winmm_proxy>" "${STAGE_DIR}/good/winmm.dll"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:winmm_proxy>" "${STAGE_DIR}/bad/winmm.dll"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:winmm_proxy>" "${STAGE_DIR}/syntax/winmm.dll"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:winmm_proxy>" "${STAGE_DIR}/callback/winmm.dll"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/runtime/init.lua" "${STAGE_DIR}/good/ddlua/init.lua"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_error.lua" "${STAGE_DIR}/bad/ddlua/init.lua"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/tests/syntax_error.lua" "${STAGE_DIR}/syntax/ddlua/init.lua"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/tests/callback_error.lua" "${STAGE_DIR}/callback/ddlua/init.lua"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/runtime/config/supported_builds.ini" "${STAGE_DIR}/good/ddlua/config/supported_builds.ini"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/runtime/config/supported_builds.ini" "${STAGE_DIR}/bad/ddlua/config/supported_builds.ini"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/runtime/config/supported_builds.ini" "${STAGE_DIR}/syntax/ddlua/config/supported_builds.ini"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/runtime/config/supported_builds.ini" "${STAGE_DIR}/callback/ddlua/config/supported_builds.ini"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/tests/base_mod.lua" "${STAGE_DIR}/good/mods/test_mod/lua/init.lua"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../FlestalTrinkets/lua/init.lua" "${STAGE_DIR}/good/mods/FlestalTrinkets/lua/init.lua"
)

add_custom_command(TARGET ddlua_search_order_host POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${STAGE_DIR}/search/ddlua/config"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ddlua_search_order_host>" "${STAGE_DIR}/search/ddlua_search_order_host.exe"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:winmm_proxy>" "${STAGE_DIR}/search/winmm.dll"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/runtime/init.lua" "${STAGE_DIR}/search/ddlua/init.lua"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/runtime/config/supported_builds.ini" "${STAGE_DIR}/search/ddlua/config/supported_builds.ini"
)

enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME proxy_and_lua_success COMMAND ddlua_test_host "${STAGE_DIR}/good/winmm.dll" success)
add_test(NAME lua_error_isolation COMMAND ddlua_test_host "${STAGE_DIR}/bad/winmm.dll" error)
add_test(NAME lua_syntax_error_isolation COMMAND ddlua_test_host "${STAGE_DIR}/syntax/winmm.dll" syntax)
add_test(NAME lua_callback_error_isolation COMMAND ddlua_test_host "${STAGE_DIR}/callback/winmm.dll" callback)
add_test(NAME application_directory_proxy_selection COMMAND "${STAGE_DIR}/search/ddlua_search_order_host.exe")
add_test(NAME native_trace_decoder COMMAND ${Python3_EXECUTABLE}
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_decode_native_trace.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/decode_native_trace.py")
set_tests_properties(proxy_and_lua_success lua_error_isolation lua_syntax_error_isolation lua_callback_error_isolation application_directory_proxy_selection native_trace_decoder PROPERTIES TIMEOUT 10 RUN_SERIAL TRUE)

install(TARGETS winmm_proxy RUNTIME DESTINATION .)
install(DIRECTORY runtime/ DESTINATION ddlua)
