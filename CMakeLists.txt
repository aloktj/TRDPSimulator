cmake_minimum_required(VERSION 3.16)
project(TRDPSimulator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TRDPSimulator_ENABLE_TRDP "Build with the TCNopen TRDP stack" ON)
option(TRDPSimulator_BUILD_ALL_TRDP_VERSIONS "Build simulator binaries for every supported TRDP stack" OFF)

set(TRDPSimulator_SUPPORTED_TRDP_VERSIONS "3.0.0.0;2.1.0.0;2.0.3.0;1.4.2.0" CACHE STRING
    "TRDP stack versions that can be targeted. The first entry is considered the latest.")
set(TRDPSimulator_TRDP_VERSION "latest" CACHE STRING
    "TRDP stack version to build against (or 'latest' to use the newest supported version).")
set(TRDP_ROOT "" CACHE PATH "Override the TRDP stack root directory for single-version builds.")

foreach(version IN LISTS TRDPSimulator_SUPPORTED_TRDP_VERSIONS)
    string(REGEX REPLACE "[^0-9A-Za-z]" "_" version_token "${version}")
    set(var_name "TRDP_${version_token}_ROOT")
    if (NOT DEFINED ${var_name})
        set(${var_name} "" CACHE PATH "Override the TRDP stack root directory for TRDP version ${version}.")
    endif()
endforeach()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Modules")

add_library(tinyxml2 STATIC
    third_party/tinyxml2/tinyxml2.cpp
)

target_include_directories(tinyxml2 PUBLIC third_party/tinyxml2)

file(GLOB TRDP_SIMULATOR_HEADERS CONFIGURE_DEPENDS
    include/trdp_simulator/*.hpp
)

set(TRDP_SIMULATOR_COMMON_SOURCES
    src/main.cpp
    src/config.cpp
    src/config_loader.cpp
    src/simulator.cpp
    src/logger.cpp
    src/trdp_stack_adapter_factory.cpp
    src/trdp_pd_worker.cpp
    src/trdp_md_worker.cpp
)

function(trdp_simulator_resolve_stack_root version out_var)
    string(REGEX REPLACE "[^0-9A-Za-z]" "_" version_token "${version}")
    set(version_override_var "TRDP_${version_token}_ROOT")

    set(candidate_root "")
    if (DEFINED ${version_override_var} AND NOT "${${version_override_var}}" STREQUAL "")
        set(candidate_root "${${version_override_var}}")
    elseif (NOT TRDPSimulator_BUILD_ALL_TRDP_VERSIONS AND NOT "${TRDP_ROOT}" STREQUAL "")
        set(candidate_root "${TRDP_ROOT}")
    else()
        set(candidate_root "${CMAKE_CURRENT_SOURCE_DIR}/third_party/trdp/${version}")
    endif()

    if (EXISTS "${candidate_root}")
        set(${out_var} "${candidate_root}" PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()

function(trdp_simulator_add_target target_name version)
    cmake_parse_arguments(TRDP_SIM "" "OUTPUT_NAME" "" ${ARGN})

    set(use_real_stack OFF)
    set(trdp_include_dirs "")
    set(trdp_libraries "")

    if (TRDPSimulator_ENABLE_TRDP)
        trdp_simulator_resolve_stack_root("${version}" stack_root)
        if (NOT "${stack_root}" STREQUAL "")
            unset(TRDP_INCLUDE_DIR CACHE)
            unset(TRDP_LIBRARY CACHE)
            unset(TRDP_FOUND CACHE)
            set(TRDP_ROOT "${stack_root}")
            find_package(TRDP QUIET)
            if (TRDP_FOUND)
                set(use_real_stack ON)
                set(trdp_include_dirs ${TRDP_INCLUDE_DIRS})
                set(trdp_libraries ${TRDP_LIBRARIES})
                message(STATUS "Configuring ${target_name} with TRDP stack version ${version} at ${stack_root}")
            else()
                message(WARNING "TRDP stack version ${version} not found at ${stack_root}."
                               " Building ${target_name} with the stub adapter.")
            endif()
        else()
            message(WARNING "No TRDP stack directory configured for version ${version}."
                           " Building ${target_name} with the stub adapter.")
        endif()
    endif()

    if (use_real_stack)
        set(stack_adapter_source src/trdp_stack_adapter_real.cpp)
    else()
        set(stack_adapter_source src/trdp_stack_adapter_stub.cpp)
    endif()

    add_executable(${target_name}
        ${TRDP_SIMULATOR_COMMON_SOURCES}
        ${stack_adapter_source}
        ${TRDP_SIMULATOR_HEADERS}
    )

    target_include_directories(${target_name} PRIVATE include)
    target_link_libraries(${target_name} PRIVATE tinyxml2)
    target_compile_features(${target_name} PRIVATE cxx_std_17)
    target_compile_definitions(${target_name} PRIVATE TRDP_STACK_VERSION="${version}")

    if (use_real_stack)
        target_compile_definitions(${target_name} PRIVATE TRDPSIM_WITH_TRDP)
        target_include_directories(${target_name} PRIVATE ${trdp_include_dirs})
        target_link_libraries(${target_name} PRIVATE ${trdp_libraries})
    endif()

    if (TRDP_SIM_OUTPUT_NAME)
        set_target_properties(${target_name} PROPERTIES OUTPUT_NAME "${TRDP_SIM_OUTPUT_NAME}")
    endif()

    install(TARGETS ${target_name} RUNTIME DESTINATION bin)
endfunction()

list(GET TRDPSimulator_SUPPORTED_TRDP_VERSIONS 0 TRDPSimulator_LATEST_TRDP_VERSION)

if (TRDPSimulator_BUILD_ALL_TRDP_VERSIONS)
    trdp_simulator_add_target(trdp-simulator "${TRDPSimulator_LATEST_TRDP_VERSION}")
    foreach(version IN LISTS TRDPSimulator_SUPPORTED_TRDP_VERSIONS)
        if (version STREQUAL TRDPSimulator_LATEST_TRDP_VERSION)
            continue()
        endif()
        string(REPLACE "." "-" version_suffix "${version}")
        set(extra_target "trdp-simulator-${version_suffix}")
        trdp_simulator_add_target(${extra_target} "${version}" OUTPUT_NAME "trdp-simulator-${version}")
    endforeach()
else()
    set(requested_version "${TRDPSimulator_TRDP_VERSION}")
    if (requested_version STREQUAL "latest")
        set(requested_version "${TRDPSimulator_LATEST_TRDP_VERSION}")
    endif()

    list(FIND TRDPSimulator_SUPPORTED_TRDP_VERSIONS "${requested_version}" version_index)
    if (version_index EQUAL -1)
        message(FATAL_ERROR "TRDP stack version ${requested_version} is not listed in"
                           " TRDPSimulator_SUPPORTED_TRDP_VERSIONS (${TRDPSimulator_SUPPORTED_TRDP_VERSIONS}).")
    endif()

    trdp_simulator_add_target(trdp-simulator "${requested_version}")
endif()

install(FILES docs/configuration.example.xml DESTINATION share/trdp-simulator)
install(FILES README.md DESTINATION share/doc/trdp-simulator)
