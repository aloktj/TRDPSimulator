cmake_minimum_required(VERSION 3.16)
project(TRDPSimulator VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

option(TRDPSimulator_ENABLE_TRDP "Build with the TCNopen TRDP stack" ON)
option(TRDPSimulator_BUILD_ALL_TRDP_VERSIONS "Build simulator binaries for every supported TRDP stack" OFF)

set(TRDPSimulator_SUPPORTED_TRDP_VERSIONS "3.0.0.0;2.1.0.0;2.0.3.0;1.4.2.0" CACHE STRING
    "TRDP stack versions that can be targeted. The first entry is considered the latest.")
set(TRDPSimulator_TRDP_VERSION "latest" CACHE STRING
    "TRDP stack version to build against (or 'latest' to use the newest supported version).")
set(TRDP_ROOT "" CACHE PATH "Override the TRDP stack root directory for single-version builds.")
set(TRDPSimulator_TRDP_CONFIG "auto" CACHE STRING
    "TRDP configuration file (e.g. LINUX_X86_64_config) to use when building the stack from sources.")

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

set(TRDP_SIMULATOR_CORE_SOURCES
    src/config.cpp
    src/config_loader.cpp
    src/logger.cpp
    src/simulator.cpp
    src/trdp_md_worker.cpp
    src/trdp_pd_worker.cpp
    src/trdp_stack_adapter_factory.cpp
    src/trdp_stack_adapter_stub.cpp
)

add_library(trdp_simulator_core
    ${TRDP_SIMULATOR_CORE_SOURCES}
    ${TRDP_SIMULATOR_HEADERS}
)

target_include_directories(trdp_simulator_core PUBLIC include)

target_link_libraries(trdp_simulator_core
    PUBLIC
        tinyxml2
)

target_compile_features(trdp_simulator_core PUBLIC cxx_std_17)

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

function(trdp_simulator_select_trdp_config stack_root out_var)
    set(config_spec "${TRDPSimulator_TRDP_CONFIG}")

    if (NOT "${config_spec}" STREQUAL "auto" AND NOT "${config_spec}" STREQUAL "")
        if (IS_ABSOLUTE "${config_spec}")
            if (EXISTS "${config_spec}")
                set(${out_var} "${config_spec}" PARENT_SCOPE)
                return()
            endif()
        else()
            set(candidate "${stack_root}/${config_spec}")
            if (EXISTS "${candidate}")
                set(${out_var} "${candidate}" PARENT_SCOPE)
                return()
            endif()
            set(candidate "${stack_root}/config/${config_spec}")
            if (EXISTS "${candidate}")
                set(${out_var} "${candidate}" PARENT_SCOPE)
                return()
            endif()
        endif()
        message(WARNING "Requested TRDP configuration '${config_spec}' not found for stack at ${stack_root}.")
    endif()

    set(candidates)
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|aarch64)")
            list(APPEND candidates "config/RASPIAN_config" "config/LINUX_config" "config/POSIX_X86_config")
        else()
            list(APPEND candidates "config/LINUX_X86_64_config" "config/LINUX_config" "config/POSIX_X86_config")
        endif()
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        list(APPEND candidates "config/OSX_X86_64_config" "config/OSX_64_TSN_config")
    elseif (WIN32)
        list(APPEND candidates "config/WINDOWS_config" "config/WINDOWS_SIM_config" "config/POSIX_X86_config")
    else()
        list(APPEND candidates "config/POSIX_X86_config")
    endif()
    list(APPEND candidates "config/LINUX_config" "config/LINUX_X86_64_config" "config/POSIX_X86_config")
    list(REMOVE_DUPLICATES candidates)

    foreach(candidate IN LISTS candidates)
        set(candidate_path "${stack_root}/${candidate}")
        if (EXISTS "${candidate_path}")
            set(${out_var} "${candidate_path}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    set(${out_var} "" PARENT_SCOPE)
endfunction()

function(trdp_simulator_parse_trdp_config stack_root version_token out_config_path out_target_vos out_target_os out_compile_options out_compile_definitions out_private_includes out_link_options out_link_libraries out_link_directories)
    trdp_simulator_select_trdp_config("${stack_root}" config_path)

    set(target_vos "posix")
    set(target_os "")
    set(compile_options)
    set(compile_definitions)
    set(private_includes)
    set(link_options)
    set(link_libraries)
    set(link_directories)

    if (NOT "${config_path}" STREQUAL "")
        file(READ "${config_path}" config_content)

        if (config_content MATCHES "TARGET_VOS[ \t]*=[ \t]*([^\n\r#]+)")
            set(target_vos "${CMAKE_MATCH_1}")
            string(STRIP target_vos "${target_vos}")
            string(REGEX REPLACE "\r" "" target_vos "${target_vos}")
        endif()

        if (config_content MATCHES "TARGET_OS[ \t]*=[ \t]*([^\n\r#]+)")
            set(target_os "${CMAKE_MATCH_1}")
            string(STRIP target_os "${target_os}")
            string(REGEX REPLACE "\r" "" target_os "${target_os}")
        endif()

        string(REGEX MATCHALL "CFLAGS[ \t]*\\+=[^\n\r]+" cflags_matches "${config_content}")
        foreach(line IN LISTS cflags_matches)
            string(REGEX REPLACE "#.*$" "" line "${line}")
            string(REGEX REPLACE "\r" "" line "${line}")
            string(REGEX REPLACE "^CFLAGS[ \t]*\\+=" "" line "${line}")
            string(STRIP line "${line}")
            if (line STREQUAL "")
                continue()
            endif()
            separate_arguments(tokens UNIX_COMMAND "${line}")
            foreach(token IN LISTS tokens)
                if (token STREQUAL "")
                    continue()
                endif()
                if (token MATCHES "^-D(.+)")
                    list(APPEND compile_definitions "${CMAKE_MATCH_1}")
                elseif (token MATCHES "^-I(.+)")
                    list(APPEND private_includes "${CMAKE_MATCH_1}")
                else()
                    list(APPEND compile_options "${token}")
                endif()
            endforeach()
        endforeach()

        string(REGEX MATCHALL "INCPATH[ \t]*\\+=[^\n\r]+" incpath_matches "${config_content}")
        foreach(line IN LISTS incpath_matches)
            string(REGEX REPLACE "#.*$" "" line "${line}")
            string(REGEX REPLACE "\r" "" line "${line}")
            string(REGEX REPLACE "^INCPATH[ \t]*\\+=" "" line "${line}")
            string(STRIP line "${line}")
            if (line STREQUAL "")
                continue()
            endif()
            separate_arguments(tokens UNIX_COMMAND "${line}")
            foreach(token IN LISTS tokens)
                if (token STREQUAL "")
                    continue()
                endif()
                if (token MATCHES "^-I(.+)")
                    list(APPEND private_includes "${CMAKE_MATCH_1}")
                endif()
            endforeach()
        endforeach()

        string(REGEX MATCHALL "LDFLAGS[ \t]*\\+=[^\n\r]+" ldflags_matches "${config_content}")
        foreach(line IN LISTS ldflags_matches)
            string(REGEX REPLACE "#.*$" "" line "${line}")
            string(REGEX REPLACE "\r" "" line "${line}")
            string(REGEX REPLACE "^LDFLAGS[ \t]*\\+=" "" line "${line}")
            string(STRIP line "${line}")
            if (line STREQUAL "")
                continue()
            endif()
            separate_arguments(tokens UNIX_COMMAND "${line}")
            foreach(token IN LISTS tokens)
                if (token STREQUAL "")
                    continue()
                endif()
                if (token MATCHES "^-l(.+)")
                    list(APPEND link_libraries "${CMAKE_MATCH_1}")
                elseif (token MATCHES "^-L(.+)")
                    list(APPEND link_directories "${CMAKE_MATCH_1}")
                else()
                    list(APPEND link_options "${token}")
                endif()
            endforeach()
        endforeach()

        foreach(flag_name IN ITEMS MD_SUPPORT TSN_SUPPORT SOA_SUPPORT PD_UNICAST_SUPPORT HIGH_PERF_INDEXED HIGH_PERF_BASE2)
            if (config_content MATCHES "${flag_name}[ \t]*=[ \t]*([^\n\r#]+)")
                set(flag_value "${CMAKE_MATCH_1}")
                string(STRIP flag_value "${flag_value}")
                string(REGEX REPLACE "\r" "" flag_value "${flag_value}")
                if (NOT flag_value STREQUAL "")
                    list(APPEND compile_definitions "${flag_name}=${flag_value}")
                endif()
            endif()
        endforeach()
    endif()

    if (NOT "${target_os}" STREQUAL "")
        list(APPEND compile_definitions "${target_os}")
    endif()

    set(resolved_private_includes)
    foreach(include_dir IN LISTS private_includes)
        if (IS_ABSOLUTE "${include_dir}")
            list(APPEND resolved_private_includes "${include_dir}")
        else()
            list(APPEND resolved_private_includes "${stack_root}/${include_dir}")
        endif()
    endforeach()
    set(private_includes "${resolved_private_includes}")

    set(resolved_link_directories)
    foreach(link_dir IN LISTS link_directories)
        if (IS_ABSOLUTE "${link_dir}")
            list(APPEND resolved_link_directories "${link_dir}")
        else()
            list(APPEND resolved_link_directories "${stack_root}/${link_dir}")
        endif()
    endforeach()
    set(link_directories "${resolved_link_directories}")

    set(uuid_requested FALSE)
    foreach(definition IN LISTS compile_definitions)
        if (definition MATCHES "^HAS_UUID(=.*)?$")
            set(uuid_requested TRUE)
            break()
        endif()
    endforeach()

    if (uuid_requested)
        set(uuid_header_found FALSE)
        foreach(include_dir IN LISTS private_includes)
            if (EXISTS "${include_dir}/uuid/uuid.h")
                set(uuid_header_found TRUE)
                break()
            endif()
        endforeach()
        if (NOT uuid_header_found)
            string(REGEX REPLACE "[^0-9A-Za-z]" "_" uuid_token "${version_token}")
            set(uuid_var "TRDPSIMULATOR_UUID_INCLUDE_${uuid_token}")
            if (NOT DEFINED ${uuid_var})
                find_path(${uuid_var} "uuid/uuid.h")
            endif()
            if (DEFINED ${uuid_var} AND NOT "${${uuid_var}}" STREQUAL "")
                list(APPEND private_includes "${${uuid_var}}")
                set(uuid_header_found TRUE)
            endif()
        endif()
        if (NOT uuid_header_found)
            message(WARNING
                "TRDP configuration ${config_path} requests HAS_UUID but uuid/uuid.h was not found."
                " The build may fail unless the libuuid development headers are installed.")
        endif()
    endif()

    foreach(var_name IN ITEMS compile_options compile_definitions private_includes link_options link_libraries link_directories)
        list(REMOVE_DUPLICATES ${var_name})
    endforeach()

    set(${out_config_path} "${config_path}" PARENT_SCOPE)
    set(${out_target_vos} "${target_vos}" PARENT_SCOPE)
    set(${out_target_os} "${target_os}" PARENT_SCOPE)
    set(${out_compile_options} "${compile_options}" PARENT_SCOPE)
    set(${out_compile_definitions} "${compile_definitions}" PARENT_SCOPE)
    set(${out_private_includes} "${private_includes}" PARENT_SCOPE)
    set(${out_link_options} "${link_options}" PARENT_SCOPE)
    set(${out_link_libraries} "${link_libraries}" PARENT_SCOPE)
    set(${out_link_directories} "${link_directories}" PARENT_SCOPE)
endfunction()

function(trdp_simulator_build_stack_from_sources stack_target stack_root version version_token out_success)
    trdp_simulator_parse_trdp_config("${stack_root}" "${version_token}" config_path target_vos target_os compile_options compile_definitions private_includes link_options link_libraries link_directories)

    set(fallback_used FALSE)
    if (config_path STREQUAL "")
        if (version STREQUAL "3.0.0.0")
            set(target_vos "posix")
            list(APPEND compile_options "-D_GNU_SOURCE")
            list(APPEND compile_definitions "POSIX" "L_ENDIAN" "HAS_UUID" "MD_SUPPORT=1")
            if (target_os STREQUAL "")
                set(target_os "LINUX")
            endif()
            list(APPEND compile_definitions "${target_os}")
            list(APPEND link_libraries "rt" "pthread" "uuid")
            set(fallback_used TRUE)
        endif()
        if (NOT fallback_used)
            message(WARNING "No TRDP configuration file found in ${stack_root}; cannot build stack version ${version} from sources.")
            set(${out_success} FALSE PARENT_SCOPE)
            return()
        endif()
        foreach(var_name IN ITEMS compile_options compile_definitions link_libraries)
            if (${var_name})
                list(REMOVE_DUPLICATES ${var_name})
            endif()
        endforeach()
    endif()


    set(vos_dir "${stack_root}/src/vos/${target_vos}")
    if (NOT EXISTS "${vos_dir}")
        message(WARNING "TRDP configuration ${config_path} selects TARGET_VOS='${target_vos}', but ${vos_dir} does not exist.")
        set(${out_success} FALSE PARENT_SCOPE)
        return()
    endif()

    file(GLOB trdp_common_sources
        "${stack_root}/src/common/*.c"
    )
    list(FILTER trdp_common_sources EXCLUDE REGEX "/old/")

    file(GLOB trdp_vos_common_sources
        "${stack_root}/src/vos/common/*.c"
    )

    file(GLOB trdp_vos_specific_sources
        "${vos_dir}/*.c"
    )

    set(trdp_sources ${trdp_common_sources} ${trdp_vos_common_sources} ${trdp_vos_specific_sources})
    list(REMOVE_DUPLICATES trdp_sources)

    set(tsn_enabled FALSE)
    foreach(definition IN LISTS compile_definitions)
        if (definition MATCHES "^TSN_SUPPORT(=1)?$")
            set(tsn_enabled TRUE)
            break()
        endif()
    endforeach()
    if (NOT tsn_enabled)
        list(FILTER trdp_sources EXCLUDE REGEX "/vos_sockTSN\\.c$")
    endif()

    if (trdp_sources STREQUAL "")
        message(WARNING "No TRDP source files discovered in ${stack_root}; cannot build stack version ${version} from sources.")
        set(${out_success} FALSE PARENT_SCOPE)
        return()
    endif()

    add_library(${stack_target} STATIC ${trdp_sources})
    target_compile_features(${stack_target} PRIVATE c_std_99)

    set(public_includes)
    foreach(dir IN ITEMS src/api src/common src/vos/api "src/vos/${target_vos}")
        set(full_path "${stack_root}/${dir}")
        if (EXISTS "${full_path}")
            list(APPEND public_includes "${full_path}")
        endif()
    endforeach()
    if (public_includes)
        list(REMOVE_DUPLICATES public_includes)
        target_include_directories(${stack_target} PUBLIC ${public_includes})
    endif()

    if (compile_options)
        target_compile_options(${stack_target} PRIVATE ${compile_options})
    endif()
    if (compile_definitions)
        target_compile_definitions(${stack_target} PRIVATE ${compile_definitions})
    endif()
    if (private_includes)
        target_include_directories(${stack_target} PRIVATE ${private_includes})
    endif()
    if (link_directories)
        target_link_directories(${stack_target} PRIVATE ${link_directories})
    endif()
    if (link_options)
        target_link_options(${stack_target} PRIVATE ${link_options})
    endif()
    if (link_libraries)
        target_link_libraries(${stack_target} PUBLIC ${link_libraries})
    endif()

    message(STATUS "Building TRDP stack version ${version} from sources at ${stack_root} using ${config_path}")
    set(${out_success} TRUE PARENT_SCOPE)
endfunction()

function(trdp_simulator_import_prebuilt_stack stack_target stack_root version version_token out_success)
    set(${out_success} FALSE PARENT_SCOPE)

    file(GLOB prebuilt_libraries
        "${stack_root}/lib/libtrdp*.a"
        "${stack_root}/lib/libtrdp*.so"
        "${stack_root}/bld/output/*/libtrdp*.a"
        "${stack_root}/bld/output/*/libtrdp*.so"
    )

    if (NOT prebuilt_libraries)
        return()
    endif()

    list(SORT prebuilt_libraries)
    list(GET prebuilt_libraries 0 library_path)

    if (NOT EXISTS "${library_path}")
        return()
    endif()

    trdp_simulator_parse_trdp_config("${stack_root}" "${version_token}" config_path target_vos target_os compile_options compile_definitions private_includes link_options link_libraries link_directories)

    add_library(${stack_target} STATIC IMPORTED)
    set_target_properties(${stack_target} PROPERTIES IMPORTED_LOCATION "${library_path}")

    set(public_includes)
    foreach(dir IN ITEMS include src/api src/common src/vos/api "src/vos/${target_vos}")
        set(full_path "${stack_root}/${dir}")
        if (EXISTS "${full_path}")
            list(APPEND public_includes "${full_path}")
        endif()
    endforeach()
    if (public_includes)
        list(REMOVE_DUPLICATES public_includes)
        set_target_properties(${stack_target} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${public_includes}")
    endif()

    if (link_directories)
        set_target_properties(${stack_target} PROPERTIES INTERFACE_LINK_DIRECTORIES "${link_directories}")
    endif()
    if (link_options)
        set_target_properties(${stack_target} PROPERTIES INTERFACE_LINK_OPTIONS "${link_options}")
    endif()
    if (link_libraries)
        set_target_properties(${stack_target} PROPERTIES INTERFACE_LINK_LIBRARIES "${link_libraries}")
    endif()

    message(STATUS "Using prebuilt TRDP library for version ${version}: ${library_path}")
    set(${out_success} TRUE PARENT_SCOPE)
endfunction()

function(trdp_simulator_get_stack_target version stack_root out_target out_found)
    string(REGEX REPLACE "[^0-9A-Za-z]" "_" version_token "${version}")
    set(stack_target "trdp_stack_${version_token}")

    if (TARGET ${stack_target})
        set(${out_target} "${stack_target}" PARENT_SCOPE)
        set(${out_found} TRUE PARENT_SCOPE)
        return()
    endif()

    if (NOT EXISTS "${stack_root}")
        set(${out_target} "" PARENT_SCOPE)
        set(${out_found} FALSE PARENT_SCOPE)
        return()
    endif()

    trdp_simulator_import_prebuilt_stack(${stack_target} "${stack_root}" "${version}" "${version_token}" prebuilt_success)
    if (prebuilt_success)
        set(${out_target} "${stack_target}" PARENT_SCOPE)
        set(${out_found} TRUE PARENT_SCOPE)
        return()
    endif()

    trdp_simulator_build_stack_from_sources(${stack_target} "${stack_root}" "${version}" "${version_token}" build_success)
    if (build_success)
        set(${out_target} "${stack_target}" PARENT_SCOPE)
        set(${out_found} TRUE PARENT_SCOPE)
        return()
    endif()

    set(${out_target} "" PARENT_SCOPE)
    set(${out_found} FALSE PARENT_SCOPE)
endfunction()

function(trdp_simulator_add_target target_name version)
    cmake_parse_arguments(TRDP_SIM "" "OUTPUT_NAME" "SOURCES" ${ARGN})

    set(target_sources ${TRDP_SIM_SOURCES})
    if (NOT target_sources)
        set(target_sources src/main.cpp)
    endif()

    set(use_real_stack OFF)
    set(trdp_stack_target "")

    if (TRDPSimulator_ENABLE_TRDP)
        trdp_simulator_resolve_stack_root("${version}" stack_root)
        if (NOT "${stack_root}" STREQUAL "")
            trdp_simulator_get_stack_target("${version}" "${stack_root}" stack_target stack_available)
            if (stack_available)
                set(use_real_stack ON)
                set(trdp_stack_target "${stack_target}")
                message(STATUS "Configuring ${target_name} with TRDP stack version ${version} at ${stack_root}")
            else()
                message(WARNING "TRDP stack version ${version} is unavailable at ${stack_root}."
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
        set(stack_adapter_source "")
    endif()

    add_executable(${target_name}
        ${target_sources}
    )

    if (stack_adapter_source)
        target_sources(${target_name} PRIVATE ${stack_adapter_source})
    endif()

    target_link_libraries(${target_name} PRIVATE trdp_simulator_core)
    target_compile_definitions(${target_name} PRIVATE TRDP_STACK_VERSION="${version}")

    if (use_real_stack)
        target_compile_definitions(${target_name} PRIVATE TRDPSIM_WITH_TRDP)
        if (trdp_stack_target)
            target_link_libraries(${target_name} PRIVATE ${trdp_stack_target})
        endif()
    endif()

    if (TRDP_SIM_OUTPUT_NAME)
        set_target_properties(${target_name} PROPERTIES OUTPUT_NAME "${TRDP_SIM_OUTPUT_NAME}")
    endif()

    install(TARGETS ${target_name} RUNTIME DESTINATION bin)
endfunction()

list(GET TRDPSimulator_SUPPORTED_TRDP_VERSIONS 0 TRDPSimulator_LATEST_TRDP_VERSION)

if (TRDPSimulator_BUILD_ALL_TRDP_VERSIONS)
    trdp_simulator_add_target(trdp-simulator "${TRDPSimulator_LATEST_TRDP_VERSION}")
    trdp_simulator_add_target(trdp-simulator-web "${TRDPSimulator_LATEST_TRDP_VERSION}"
        SOURCES
            src/web_main.cpp
            src/web_application.cpp
    )
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
    trdp_simulator_add_target(trdp-simulator-web "${requested_version}"
        SOURCES
            src/web_main.cpp
            src/web_application.cpp
    )
endif()

if (BUILD_TESTING)
    add_executable(trdp-simulator-tests
        tests/payload_tests.cpp
    )

    target_link_libraries(trdp-simulator-tests PRIVATE trdp_simulator_core)

    add_test(NAME payload_tests COMMAND trdp-simulator-tests)
endif()

install(FILES docs/configuration.example.xml DESTINATION share/trdp-simulator)
install(FILES README.md DESTINATION share/doc/trdp-simulator)
