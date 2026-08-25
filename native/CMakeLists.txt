include(${CMAKE_SOURCE_DIR}/cmake/MinHook.cmake)

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
