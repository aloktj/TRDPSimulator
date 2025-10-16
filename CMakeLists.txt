cmake_minimum_required(VERSION 3.16)
project(TRDPSimulator VERSION 0.1.0 LANGUAGES CXX)

option(TRDPSimulator_ENABLE_TRDP "Build with the TCNopen TRDP stack" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Modules")

if (TRDPSimulator_ENABLE_TRDP)
    find_package(TRDP QUIET)
endif()

if (TRDP_FOUND)
    message(STATUS "Building with TRDP support")
    add_compile_definitions(TRDPSIM_WITH_TRDP)
else()
    message(WARNING "TCNopen TRDP stack not found. Building simulator with stub adapter."
                   " Real PD/MD traffic will be disabled.")
endif()

add_library(tinyxml2 STATIC
    third_party/tinyxml2/tinyxml2.cpp
)

target_include_directories(tinyxml2 PUBLIC third_party/tinyxml2)

file(GLOB TRDP_SIMULATOR_HEADERS CONFIGURE_DEPENDS
    include/trdp_simulator/*.hpp
)

set(TRDP_SIMULATOR_SOURCES
    src/main.cpp
    src/config.cpp
    src/config_loader.cpp
    src/simulator.cpp
    src/logger.cpp
    src/trdp_stack_adapter_factory.cpp
    src/trdp_pd_worker.cpp
    src/trdp_md_worker.cpp
)

if (TRDP_FOUND)
    list(APPEND TRDP_SIMULATOR_SOURCES src/trdp_stack_adapter_real.cpp)
else()
    list(APPEND TRDP_SIMULATOR_SOURCES src/trdp_stack_adapter_stub.cpp)
endif()

add_executable(trdp-simulator ${TRDP_SIMULATOR_SOURCES} ${TRDP_SIMULATOR_HEADERS})

target_include_directories(trdp-simulator
    PRIVATE
        include
)

if (TRDP_FOUND)
    target_include_directories(trdp-simulator PRIVATE ${TRDP_INCLUDE_DIRS})
    target_link_libraries(trdp-simulator PRIVATE ${TRDP_LIBRARIES})
endif()

target_link_libraries(trdp-simulator PRIVATE tinyxml2)

target_compile_features(trdp-simulator PRIVATE cxx_std_17)

install(TARGETS trdp-simulator RUNTIME DESTINATION bin)
install(FILES docs/configuration.example.xml DESTINATION share/trdp-simulator)
install(FILES README.md DESTINATION share/doc/trdp-simulator)
