cmake_minimum_required(VERSION 3.24)
project(PagePilot VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(PAGEPILOT_SANITIZE "Enable address and undefined behavior sanitizers" OFF)
option(PAGEPILOT_BUILD_FUZZER "Build bounded protocol fuzz target with LLVM libFuzzer" OFF)
find_package(nlohmann_json 3.12 REQUIRED)
find_package(Boost 1.85 CONFIG REQUIRED)
find_package(Threads REQUIRED)
function(pagepilot_require_cancellation_support)
 include(CheckCXXSourceCompiles)
 include(CMakePushCheckState)
 cmake_push_check_state(RESET)
 set(CMAKE_REQUIRED_LIBRARIES Threads::Threads)
 # This must link an executable even when a parent toolchain uses static
 # try-compiles. A compiler's -std=c++20 flag alone does not prove library support.
 set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
 check_cxx_source_compiles([[
  #include <stop_token>
  int main() {
    std::stop_source source;
    std::stop_token token = source.get_token();
    if (!token.stop_possible()) return 1;
    source.request_stop();
    return token.stop_requested() ? 0 : 1;
  }
 ]] PAGEPILOT_HAS_CXX20_STOP_TOKEN)
 cmake_pop_check_state()
 if(NOT PAGEPILOT_HAS_CXX20_STOP_TOKEN)
  message(FATAL_ERROR
   "PagePilot requires a compiler and C++ standard library/SDK that compile "
   "and link std::stop_source and std::stop_token. The selected toolchain does "
   "not provide that C++20 cancellation support. Upgrade the toolchain and its "
   "standard library/SDK (on macOS, select a newer Xcode or a compatible LLVM "
   "installation), then configure a fresh build directory with "
   "-DCMAKE_CXX_COMPILER=/absolute/path/to/clang++. Enabling -std=c++20 alone "
   "is insufficient. See the CMake configure log for the failed probe.")
 endif()
endfunction()
pagepilot_require_cancellation_support()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS resources/tool_catalog.json resources/dom_agent.js resources/legacy_value.js resources/key_layout.json resources/editing_commands.json)
file(READ resources/tool_catalog.json PILOT_CATALOG)
file(READ resources/dom_agent.js PILOT_DOM)
file(READ resources/legacy_value.js PILOT_LEGACY_VALUE)
file(READ resources/key_layout.json PILOT_KEYS)
file(READ resources/editing_commands.json PILOT_EDITING)
configure_file(cmake/embedded_resources.hpp.in generated/embedded_resources.hpp @ONLY)
add_library(pagepilot
  src/browser/browser_session.cpp src/browser/frame_context.cpp
  src/browser/frame_geometry.cpp src/browser/input_observer.cpp
  src/browser/keyboard_state.cpp src/browser/page_services.cpp
  src/browser/pointer_state.cpp
  src/core/cancellation.cpp src/core/json_contract.cpp src/core/path_guard.cpp
  src/core/png_image.cpp
  src/mcp/request_pump.cpp src/mcp/stdio_transport.cpp
  src/tools/click_tool.cpp src/tools/dom_tools.cpp src/tools/file_tools.cpp
  src/tools/keyboard_plan.cpp src/tools/step_sequence.cpp
  src/tools/tool_catalog.cpp src/tools/tool_runtime.cpp
  src/wire/cdp_channel.cpp src/wire/cdp_message.cpp)
target_include_directories(pagepilot PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include> PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_features(pagepilot PUBLIC cxx_std_20)
target_link_libraries(pagepilot PUBLIC nlohmann_json::nlohmann_json PRIVATE Boost::headers Threads::Threads)
target_compile_options(pagepilot PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
if(PAGEPILOT_SANITIZE)
 target_compile_options(pagepilot PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
 target_link_options(pagepilot PUBLIC -fsanitize=address,undefined)
endif()
add_executable(page-pilot app/cli_main.cpp)
target_link_libraries(page-pilot PRIVATE pagepilot)
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/PagePilotConfig.cmake.in
 ${CMAKE_CURRENT_BINARY_DIR}/PagePilotConfig.cmake
 INSTALL_DESTINATION lib/cmake/PagePilot)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/PagePilotConfigVersion.cmake
 COMPATIBILITY SameMajorVersion)
install(TARGETS page-pilot pagepilot EXPORT PagePilotTargets
 RUNTIME DESTINATION bin ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT PagePilotTargets NAMESPACE PagePilot:: DESTINATION lib/cmake/PagePilot)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/PagePilotConfig.cmake
 ${CMAKE_CURRENT_BINARY_DIR}/PagePilotConfigVersion.cmake DESTINATION lib/cmake/PagePilot)
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md dependencies.lock.json DESTINATION share/doc/PagePilot)
install(DIRECTORY docs licenses validation DESTINATION share/doc/PagePilot)
install(DIRECTORY examples DESTINATION share/PagePilot)
if(PAGEPILOT_BUILD_FUZZER)
 if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Protocol fuzzing requires a Clang installation with libFuzzer")
 endif()
 add_executable(pilot-protocol-fuzz tests/fuzz/protocol_fuzz.cpp src/core/json_contract.cpp src/core/cancellation.cpp src/tools/tool_catalog.cpp src/mcp/stdio_transport.cpp src/wire/cdp_message.cpp)
 target_include_directories(pilot-protocol-fuzz PRIVATE include ${CMAKE_CURRENT_BINARY_DIR}/generated)
 target_link_libraries(pilot-protocol-fuzz PRIVATE nlohmann_json::nlohmann_json)
 target_compile_options(pilot-protocol-fuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
 target_link_options(pilot-protocol-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
include(CTest)
if(BUILD_TESTING)
 add_executable(pilot-foundation-tests tests/unit/foundation.cpp)
 target_link_libraries(pilot-foundation-tests PRIVATE pagepilot)
 add_test(NAME foundation COMMAND pilot-foundation-tests)
 add_executable(pilot-mcp-tests tests/unit/mcp_contract.cpp)
 target_link_libraries(pilot-mcp-tests PRIVATE pagepilot)
 add_test(NAME mcp-contract COMMAND pilot-mcp-tests)
 add_executable(pilot-file-contract-tests tests/unit/file_contract.cpp)
 target_link_libraries(pilot-file-contract-tests PRIVATE pagepilot)
 add_test(NAME file-contract COMMAND pilot-file-contract-tests)
 add_executable(pilot-workflow-contract-tests tests/unit/workflow_contract.cpp)
 target_link_libraries(pilot-workflow-contract-tests PRIVATE pagepilot)
 add_test(NAME workflow-contract COMMAND pilot-workflow-contract-tests)
 add_executable(pilot-cancellation-contract-tests tests/unit/cancellation_contract.cpp)
 target_link_libraries(pilot-cancellation-contract-tests PRIVATE pagepilot Threads::Threads)
 add_test(NAME cancellation-contract COMMAND pilot-cancellation-contract-tests)
 set_tests_properties(cancellation-contract PROPERTIES TIMEOUT 20)
 add_executable(pilot-browser-tests tests/integration/browser_live.cpp)
 target_link_libraries(pilot-browser-tests PRIVATE pagepilot)
 add_executable(pilot-wire-tests tests/integration/wire_live.cpp)
 target_link_libraries(pilot-wire-tests PRIVATE pagepilot)
 add_executable(pilot-wire-fault-tests tests/integration/wire_fault.cpp)
 target_link_libraries(pilot-wire-fault-tests PRIVATE pagepilot Threads::Threads)
 add_executable(pilot-dom-tests tests/integration/dom_live.cpp)
 target_link_libraries(pilot-dom-tests PRIVATE pagepilot)
 add_executable(pilot-keyboard-tests tests/integration/keyboard_live.cpp)
 target_link_libraries(pilot-keyboard-tests PRIVATE pagepilot)
 add_executable(pilot-keyboard-fault-tests tests/integration/keyboard_fault.cpp)
 target_link_libraries(pilot-keyboard-fault-tests PRIVATE pagepilot Threads::Threads)
 add_executable(pilot-input-target-tests tests/integration/input_targets_live.cpp)
 target_link_libraries(pilot-input-target-tests PRIVATE pagepilot)
 add_executable(pilot-condition-tests tests/integration/condition_live.cpp)
 target_link_libraries(pilot-condition-tests PRIVATE pagepilot)
 add_executable(pilot-click-tests tests/integration/click_live.cpp)
 target_link_libraries(pilot-click-tests PRIVATE pagepilot)
 add_executable(pilot-frame-tests tests/integration/frame_live.cpp)
 target_link_libraries(pilot-frame-tests PRIVATE pagepilot)
 add_executable(pilot-services-tests tests/integration/services_live.cpp)
 target_link_libraries(pilot-services-tests PRIVATE pagepilot)
 add_executable(pilot-cookie-context-tests tests/integration/cookie_context_live.cpp)
 target_link_libraries(pilot-cookie-context-tests PRIVATE pagepilot)
 add_executable(pilot-pointer-tests tests/integration/pointer_live.cpp)
 target_link_libraries(pilot-pointer-tests PRIVATE pagepilot)
 add_executable(pilot-files-tests tests/integration/files_live.cpp)
 target_link_libraries(pilot-files-tests PRIVATE pagepilot)
 add_executable(pilot-workflow-tests tests/integration/workflow_live.cpp)
 target_link_libraries(pilot-workflow-tests PRIVATE pagepilot)
 add_executable(pilot-navigation-tests tests/integration/navigation_live.cpp)
 target_link_libraries(pilot-navigation-tests PRIVATE pagepilot)
 add_executable(pilot-recovery-tests tests/integration/recovery_live.cpp)
 target_link_libraries(pilot-recovery-tests PRIVATE pagepilot Threads::Threads)
 add_executable(pilot-target-affinity-tests tests/integration/target_affinity_live.cpp)
 target_link_libraries(pilot-target-affinity-tests PRIVATE pagepilot Threads::Threads)
endif()
