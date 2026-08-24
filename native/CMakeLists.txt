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

add_executable(DSRRandomizer.SuspendedFixture
  fixtures/SuspendedFixture.cpp)
target_compile_features(DSRRandomizer.SuspendedFixture PRIVATE cxx_std_20)
dsr_configure_target(DSRRandomizer.SuspendedFixture)
set_target_properties(DSRRandomizer.SuspendedFixture PROPERTIES
  CXX_EXTENSIONS OFF)

add_library(DSRRandomizer.StalledRuntime SHARED
  fixtures/StalledGuard.cpp)
target_compile_features(DSRRandomizer.StalledRuntime PRIVATE cxx_std_20)
dsr_configure_target(DSRRandomizer.StalledRuntime)
set_target_properties(DSRRandomizer.StalledRuntime PROPERTIES
  CXX_EXTENSIONS OFF
  OUTPUT_NAME "DSRRandomizer.StalledRuntime")

add_executable(ProtectionBootstrapTests
  tests/ProtectionBootstrapTests.cpp)
target_compile_features(ProtectionBootstrapTests PRIVATE cxx_std_20)
target_link_libraries(ProtectionBootstrapTests PRIVATE DSRRandomizer.Runtime.Core)
dsr_configure_target(ProtectionBootstrapTests)
set_target_properties(ProtectionBootstrapTests PROPERTIES
  CXX_EXTENSIONS OFF)

add_test(NAME ProtectionBootstrapTests COMMAND ProtectionBootstrapTests)

add_executable(SuspendedFixtureTests
  tests/SuspendedFixtureTests.cpp)
target_compile_features(SuspendedFixtureTests PRIVATE cxx_std_20)
dsr_configure_target(SuspendedFixtureTests)
set_target_properties(SuspendedFixtureTests PROPERTIES
  CXX_EXTENSIONS OFF)

add_test(
  NAME SuspendedFixtureTests
  COMMAND SuspendedFixtureTests $<TARGET_FILE:DSRRandomizer.SuspendedFixture>)
