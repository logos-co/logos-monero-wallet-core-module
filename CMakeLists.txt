cmake_minimum_required(VERSION 3.14)
project(MoneroWalletCoreModulePlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

logos_module(
    NAME monero_wallet_core_module
    SOURCES
        src/monero_wallet_core_impl.h
        src/monero_wallet_core_impl.cpp
        src/wallet_runtime.h
        src/wallet_runtime.cpp
        src/address_check.h
        src/address_check.cpp
    EXTERNAL_LIBS
        monero_wallet2_api_c
    INCLUDE_DIRS
        lib
)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" _mw_metadata_json)
string(JSON MONERO_WALLET_CORE_VERSION GET "${_mw_metadata_json}" version)
target_compile_definitions(monero_wallet_core_module_module_plugin
    PRIVATE MONERO_WALLET_CORE_VERSION="${MONERO_WALLET_CORE_VERSION}"
            MONERO_C_VERSION="0.18.4.6-RC2")

if(NOT APPLE AND NOT WIN32)
    target_link_libraries(monero_wallet_core_module_module_plugin PRIVATE stdc++ m pthread)
endif()
