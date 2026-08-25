include(${CMAKE_SOURCE_DIR}/cmake/MinHook.cmake)

file(READ "${CMAKE_SOURCE_DIR}/config/compatibility-profiles.json" profile_json)
string(JSON profile_count LENGTH "${profile_json}" profiles)
if(NOT profile_count EQUAL 1)
  message(FATAL_ERROR "The native guard requires exactly one release-pinned profile")
endif()
string(JSON profile_schema GET "${profile_json}" schemaVersion)
if(NOT profile_schema EQUAL 1)
  message(FATAL_ERROR "The native guard supports compatibility profile schema v1 only")
endif()
string(JSON PROFILE_EXECUTABLE_MODULE GET
  "${profile_json}" profiles 0 executableModule)
string(JSON PROFILE_MACHINE GET
  "${profile_json}" profiles 0 executable machine)
string(JSON PROFILE_TIMESTAMP GET
  "${profile_json}" profiles 0 executable peTimestamp)
string(JSON PROFILE_IMAGE_SIZE GET
  "${profile_json}" profiles 0 executable sizeOfImage)
string(JSON PROFILE_TARGET_COUNT LENGTH
  "${profile_json}" profiles 0 gameServiceTargets)
set(PROFILE_TARGETS "")
if(PROFILE_TARGET_COUNT GREATER 0)
  math(EXPR profile_target_last "${PROFILE_TARGET_COUNT} - 1")
  foreach(profile_target_index RANGE 0 ${profile_target_last})
    string(JSON target_module GET "${profile_json}"
      profiles 0 gameServiceTargets ${profile_target_index} module)
    string(JSON target_rva GET "${profile_json}"
      profiles 0 gameServiceTargets ${profile_target_index} rva)
    string(JSON target_fingerprint GET "${profile_json}"
      profiles 0 gameServiceTargets ${profile_target_index} fingerprintSha256)
    string(JSON target_length GET "${profile_json}"
      profiles 0 gameServiceTargets ${profile_target_index} patchLength)
    string(JSON target_action GET "${profile_json}"
      profiles 0 gameServiceTargets ${profile_target_index} action)
    string(LENGTH "${target_fingerprint}" target_fingerprint_length)
    if(NOT target_fingerprint_length EQUAL 64
        OR target_fingerprint MATCHES "[^0-9A-Fa-f]")
      message(FATAL_ERROR "A game-service fingerprint is not SHA-256")
    endif()
    if(target_action STREQUAL "ForceOffline")
      set(target_action_cpp "InternalTargetAction::ForceOffline")
    elseif(target_action STREQUAL "DenyCall")
      set(target_action_cpp "InternalTargetAction::DenyCall")
    else()
      message(FATAL_ERROR "A game-service target action is unsupported")
    endif()
    set(target_fingerprint_cpp "")
    foreach(profile_byte_index RANGE 0 31)
      math(EXPR profile_hex_index "${profile_byte_index} * 2")
      string(SUBSTRING "${target_fingerprint}" ${profile_hex_index} 2 target_byte)
      string(APPEND target_fingerprint_cpp "0x${target_byte}")
      if(NOT profile_byte_index EQUAL 31)
        string(APPEND target_fingerprint_cpp ", ")
      endif()
    endforeach()
    string(APPEND PROFILE_TARGETS
      "    Target{L\"${target_module}\", ${target_rva}ULL, {${target_fingerprint_cpp}}, ${target_length}ULL, ${target_action_cpp}},\n")
  endforeach()
endif()
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/game")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/runtime/game/CompatibilityProfile.generated.h.in"
  "${CMAKE_BINARY_DIR}/generated/game/CompatibilityProfile.generated.h"
  @ONLY)

function(dsr_configure_target target_name)
  target_compile_definitions(${target_name}
    PRIVATE
      UNICODE
      _UNICODE
      WIN32_LEAN_AND_MEAN
      NOMINMAX)
  if(MSVC)
    target_compile_options(${target_name}
      PRIVATE
        /W4
        /WX
        /permissive-
        /utf-8)
  endif()
endfunction()

add_subdirectory(runtime)
target_sources(DSRRandomizer.Runtime.Core
  PRIVATE
    runtime/network/NetworkPolicy.cpp
    runtime/save/SavePathPolicy.cpp)

add_executable(DSRRandomizer.SuspendedFixture
  fixtures/SuspendedFixture.cpp)
target_compile_features(DSRRandomizer.SuspendedFixture PRIVATE cxx_std_20)
dsr_configure_target(DSRRandomizer.SuspendedFixture)
set_target_properties(DSRRandomizer.SuspendedFixture PROPERTIES
  CXX_EXTENSIONS OFF)

add_executable(DSRRandomizer.SaveClientFixture
  fixtures/SaveClientFixture.cpp)
target_compile_features(DSRRandomizer.SaveClientFixture PRIVATE cxx_std_20)
target_link_libraries(DSRRandomizer.SaveClientFixture PRIVATE Shell32 Ole32)
target_include_directories(DSRRandomizer.SaveClientFixture
  PRIVATE
    ${CMAKE_SOURCE_DIR}/native/include)
dsr_configure_target(DSRRandomizer.SaveClientFixture)
set_target_properties(DSRRandomizer.SaveClientFixture PROPERTIES
  CXX_EXTENSIONS OFF)

add_executable(DSRRandomizer.NetworkClientFixture
  fixtures/NetworkClientFixture.cpp)
target_compile_features(DSRRandomizer.NetworkClientFixture PRIVATE cxx_std_20)
target_link_libraries(DSRRandomizer.NetworkClientFixture PRIVATE Ws2_32)
target_include_directories(DSRRandomizer.NetworkClientFixture
  PRIVATE
    ${CMAKE_SOURCE_DIR}/native/include
    ${CMAKE_SOURCE_DIR}/native/runtime)
dsr_configure_target(DSRRandomizer.NetworkClientFixture)
set_target_properties(DSRRandomizer.NetworkClientFixture PROPERTIES
  CXX_EXTENSIONS OFF)

add_library(DSRRandomizer.StalledRuntime SHARED
  fixtures/StalledGuard.cpp)
target_compile_features(DSRRandomizer.StalledRuntime PRIVATE cxx_std_20)
dsr_configure_target(DSRRandomizer.StalledRuntime)
set_target_properties(DSRRandomizer.StalledRuntime PROPERTIES
  CXX_EXTENSIONS OFF
  OUTPUT_NAME "DSRRandomizer.StalledRuntime")

add_library(DSRRandomizer.FakeSteamApi SHARED
  fixtures/FakeSteamApi.cpp)
target_compile_features(DSRRandomizer.FakeSteamApi PRIVATE cxx_std_20)
target_include_directories(DSRRandomizer.FakeSteamApi
  PRIVATE
    ${CMAKE_SOURCE_DIR}/native/runtime)
dsr_configure_target(DSRRandomizer.FakeSteamApi)
set_target_properties(DSRRandomizer.FakeSteamApi PROPERTIES
  CXX_EXTENSIONS OFF
  OUTPUT_NAME "DSRRandomizer.FakeSteamApi")

add_library(DSRRandomizer.NestedSteamLoader SHARED
  fixtures/NestedSteamLoader.cpp)
target_compile_features(DSRRandomizer.NestedSteamLoader PRIVATE cxx_std_20)
dsr_configure_target(DSRRandomizer.NestedSteamLoader)
set_target_properties(DSRRandomizer.NestedSteamLoader PROPERTIES
  CXX_EXTENSIONS OFF
  OUTPUT_NAME "DSRRandomizer.NestedSteamLoader")

add_library(DSRRandomizer.NestedSteamBridge SHARED
  fixtures/NestedSteamBridge.cpp)
target_compile_features(DSRRandomizer.NestedSteamBridge PRIVATE cxx_std_20)
dsr_configure_target(DSRRandomizer.NestedSteamBridge)
set_target_properties(DSRRandomizer.NestedSteamBridge PROPERTIES
  CXX_EXTENSIONS OFF
  OUTPUT_NAME "DSRRandomizer.NestedSteamBridge")

add_library(DSRRandomizer.StaticSteamCarrier SHARED
  fixtures/StaticSteamCarrier.cpp)
target_compile_features(DSRRandomizer.StaticSteamCarrier PRIVATE cxx_std_20)
target_link_libraries(DSRRandomizer.StaticSteamCarrier
  PRIVATE
    DSRRandomizer.FakeSteamApi)
dsr_configure_target(DSRRandomizer.StaticSteamCarrier)
set_target_properties(DSRRandomizer.StaticSteamCarrier PROPERTIES
  CXX_EXTENSIONS OFF
  OUTPUT_NAME "DSRRandomizer.StaticSteamCarrier")

add_library(DSRRandomizer.StaticSteamOuter SHARED
  fixtures/StaticSteamOuter.cpp)
target_compile_features(DSRRandomizer.StaticSteamOuter PRIVATE cxx_std_20)
target_link_libraries(DSRRandomizer.StaticSteamOuter
  PRIVATE
    DSRRandomizer.StaticSteamCarrier)
dsr_configure_target(DSRRandomizer.StaticSteamOuter)
set_target_properties(DSRRandomizer.StaticSteamOuter PROPERTIES
  CXX_EXTENSIONS OFF
  OUTPUT_NAME "DSRRandomizer.StaticSteamOuter")

add_library(DSRRandomizer.DelaySteamCarrier SHARED
  fixtures/DelaySteamCarrier.cpp)
target_compile_features(DSRRandomizer.DelaySteamCarrier PRIVATE cxx_std_20)
target_link_libraries(DSRRandomizer.DelaySteamCarrier
  PRIVATE
    DSRRandomizer.FakeSteamApi
    delayimp)
target_link_options(DSRRandomizer.DelaySteamCarrier
  PRIVATE
    "/DELAYLOAD:DSRRandomizer.FakeSteamApi.dll")
dsr_configure_target(DSRRandomizer.DelaySteamCarrier)
set_target_properties(DSRRandomizer.DelaySteamCarrier PROPERTIES
  CXX_EXTENSIONS OFF
  OUTPUT_NAME "DSRRandomizer.DelaySteamCarrier")

add_library(DSRRandomizer.NativeLoaderCallout SHARED
  fixtures/NativeLoaderCallout.cpp)
target_compile_features(DSRRandomizer.NativeLoaderCallout PRIVATE cxx_std_20)
target_link_libraries(DSRRandomizer.NativeLoaderCallout PRIVATE ntdll)
dsr_configure_target(DSRRandomizer.NativeLoaderCallout)
set_target_properties(DSRRandomizer.NativeLoaderCallout PROPERTIES
  CXX_EXTENSIONS OFF
  OUTPUT_NAME "DSRRandomizer.NativeLoaderCallout")

add_executable(ProtectionBootstrapTests
  tests/ProtectionBootstrapTests.cpp)
target_compile_features(ProtectionBootstrapTests PRIVATE cxx_std_20)
target_link_libraries(ProtectionBootstrapTests PRIVATE DSRRandomizer.Runtime.Core)
dsr_configure_target(ProtectionBootstrapTests)
set_target_properties(ProtectionBootstrapTests PROPERTIES
  CXX_EXTENSIONS OFF)

add_test(NAME ProtectionBootstrapTests COMMAND ProtectionBootstrapTests)

add_executable(SavePathPolicyTests
  tests/SavePathPolicyTests.cpp)
target_compile_features(SavePathPolicyTests PRIVATE cxx_std_20)
target_link_libraries(SavePathPolicyTests PRIVATE DSRRandomizer.Runtime.Core)
dsr_configure_target(SavePathPolicyTests)
set_target_properties(SavePathPolicyTests PROPERTIES
  CXX_EXTENSIONS OFF)

add_test(NAME SavePathPolicyTests COMMAND SavePathPolicyTests)

add_executable(NetworkPolicyTests
  tests/NetworkPolicyTests.cpp)
target_compile_features(NetworkPolicyTests PRIVATE cxx_std_20)
target_link_libraries(NetworkPolicyTests PRIVATE DSRRandomizer.Runtime.Core Ws2_32)
dsr_configure_target(NetworkPolicyTests)
set_target_properties(NetworkPolicyTests PROPERTIES
  CXX_EXTENSIONS OFF)

add_test(NAME NetworkPolicyTests COMMAND NetworkPolicyTests)

add_executable(WinsockHookIntegrationTests
  tests/WinsockHookIntegrationTests.cpp)
target_compile_features(WinsockHookIntegrationTests PRIVATE cxx_std_20)
target_link_libraries(WinsockHookIntegrationTests
  PRIVATE
    DSRRandomizer.Runtime.Core
    Ws2_32)
target_include_directories(WinsockHookIntegrationTests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/native/include)
dsr_configure_target(WinsockHookIntegrationTests)
set_target_properties(WinsockHookIntegrationTests PROPERTIES
  CXX_EXTENSIONS OFF)

add_test(
  NAME WinsockHookIntegrationTests
  COMMAND WinsockHookIntegrationTests
    $<TARGET_FILE:DSRRandomizer.NetworkClientFixture>
    $<TARGET_FILE:DSRRandomizer.Runtime>)

add_executable(SuspendedFixtureTests
  tests/SuspendedFixtureTests.cpp)
target_compile_features(SuspendedFixtureTests PRIVATE cxx_std_20)
dsr_configure_target(SuspendedFixtureTests)
set_target_properties(SuspendedFixtureTests PROPERTIES
  CXX_EXTENSIONS OFF)

add_test(
  NAME SuspendedFixtureTests
  COMMAND SuspendedFixtureTests $<TARGET_FILE:DSRRandomizer.SuspendedFixture>)

add_executable(SaveHookIntegrationTests
  tests/SaveHookIntegrationTests.cpp)
target_compile_features(SaveHookIntegrationTests PRIVATE cxx_std_20)
target_link_libraries(SaveHookIntegrationTests PRIVATE DSRRandomizer.Runtime.Core)
target_include_directories(SaveHookIntegrationTests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/native/include)
dsr_configure_target(SaveHookIntegrationTests)
set_target_properties(SaveHookIntegrationTests PROPERTIES
  CXX_EXTENSIONS OFF)

add_test(
  NAME SaveHookIntegrationTests
  COMMAND SaveHookIntegrationTests
    $<TARGET_FILE:DSRRandomizer.SaveClientFixture>
    $<TARGET_FILE:DSRRandomizer.Runtime>)

add_executable(SteamGuardTests
  tests/SteamGuardTests.cpp)
target_compile_features(SteamGuardTests PRIVATE cxx_std_20)
target_link_libraries(SteamGuardTests
  PRIVATE
    DSRRandomizer.Runtime.Core
    BCrypt)
target_include_directories(SteamGuardTests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/native/runtime)
dsr_configure_target(SteamGuardTests)
set_target_properties(SteamGuardTests PROPERTIES
  CXX_EXTENSIONS OFF)

add_test(
  NAME SteamGuardTests
  COMMAND SteamGuardTests
    $<TARGET_FILE:DSRRandomizer.FakeSteamApi>)

add_executable(SteamGuardHardeningTests
  tests/SteamGuardHardeningTests.cpp)
target_compile_features(SteamGuardHardeningTests PRIVATE cxx_std_20)
target_link_libraries(SteamGuardHardeningTests
  PRIVATE
    DSRRandomizer.Runtime.Core
    BCrypt)
target_include_directories(SteamGuardHardeningTests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/native/runtime)
dsr_configure_target(SteamGuardHardeningTests)
set_target_properties(SteamGuardHardeningTests PROPERTIES
  CXX_EXTENSIONS OFF)
add_dependencies(SteamGuardHardeningTests
  DSRRandomizer.DelaySteamCarrier
  DSRRandomizer.NativeLoaderCallout
  DSRRandomizer.StaticSteamOuter)

add_test(
  NAME SteamGuardHardeningTests
  COMMAND SteamGuardHardeningTests
    $<TARGET_FILE:DSRRandomizer.FakeSteamApi>
    $<TARGET_FILE:DSRRandomizer.NestedSteamLoader>
    $<TARGET_FILE:DSRRandomizer.NestedSteamBridge>
    $<TARGET_FILE:DSRRandomizer.StaticSteamCarrier>
    $<TARGET_FILE:DSRRandomizer.NativeLoaderCallout>)

add_executable(GameServiceGuardTests
  tests/GameServiceGuardTests.cpp)
target_compile_features(GameServiceGuardTests PRIVATE cxx_std_20)
target_link_libraries(GameServiceGuardTests
  PRIVATE
    DSRRandomizer.Runtime.Core
    BCrypt)
target_include_directories(GameServiceGuardTests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/native/runtime)
dsr_configure_target(GameServiceGuardTests)
set_target_properties(GameServiceGuardTests PROPERTIES
  CXX_EXTENSIONS OFF)

add_test(NAME GameServiceGuardTests COMMAND GameServiceGuardTests)
