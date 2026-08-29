add_library(DSRRandomizer.Runtime.Core STATIC
  ProtectionBootstrap.cpp
  ProtectionBootstrap.h
  hooks/MinHookCoordinator.cpp
  hooks/MinHookCoordinator.h
  game/GameServiceGuard.cpp
  game/GameServiceGuard.h
  modules/DeferredModuleGate.cpp
  modules/DeferredModuleGate.h
  monitor/HookIntegrityRegistry.cpp
  monitor/HookIntegrityRegistry.h
  monitor/ProtectionMonitor.cpp
  monitor/ProtectionMonitor.h
  profile/PinnedCompatibilityProfile.cpp
  profile/PinnedCompatibilityProfile.h
  network/WinsockHooks.cpp
  network/WinsockHooks.h
  save/SaveHooks.cpp
  save/SaveHooks.h
  save/SaveCallsiteRedirect.cpp
  save/SaveCallsiteRedirect.h
  steam/SteamHooks.cpp
  steam/SteamHooks.h
  steam/SteamPolicy.cpp
  steam/SteamPolicy.h)
target_compile_features(DSRRandomizer.Runtime.Core PUBLIC cxx_std_20)
target_include_directories(DSRRandomizer.Runtime.Core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/native/include
  PRIVATE
    ${CMAKE_BINARY_DIR}/generated)
dsr_configure_target(DSRRandomizer.Runtime.Core)
target_link_libraries(DSRRandomizer.Runtime.Core
  PUBLIC
    minhook
    BCrypt
    Psapi
    Shell32
    Ole32
    Ws2_32)
set_target_properties(DSRRandomizer.Runtime.Core PROPERTIES
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)

add_library(DSRRandomizer.Runtime SHARED
  GuardEntry.cpp)
target_compile_features(DSRRandomizer.Runtime PRIVATE cxx_std_20)
target_link_libraries(DSRRandomizer.Runtime
  PRIVATE
    DSRRandomizer.Runtime.Core
    minhook)
target_include_directories(DSRRandomizer.Runtime
  PRIVATE
    ${CMAKE_SOURCE_DIR}/native/include)
dsr_configure_target(DSRRandomizer.Runtime)
set_target_properties(DSRRandomizer.Runtime PROPERTIES
  CXX_EXTENSIONS OFF
  OUTPUT_NAME "DSRRandomizer.Runtime")

add_library(DSRRandomizer.RmmBridge.Core STATIC
  bridge/RmmBridgeConfiguration.cpp
  bridge/RmmBridgeConfiguration.h)
target_compile_features(DSRRandomizer.RmmBridge.Core PUBLIC cxx_std_20)
target_include_directories(DSRRandomizer.RmmBridge.Core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/native/include)
dsr_configure_target(DSRRandomizer.RmmBridge.Core)
set_target_properties(DSRRandomizer.RmmBridge.Core PROPERTIES
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)
