cmake_minimum_required(VERSION 3.18)
project(network_lab2_npcap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(ethernet_parser STATIC src/ethernet_parser.cpp)
target_include_directories(ethernet_parser PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_executable(ethernet_parser_tests tests/ethernet_parser_tests.cpp)
target_link_libraries(ethernet_parser_tests PRIVATE ethernet_parser)
enable_testing()
add_test(NAME ethernet_parser_tests COMMAND ethernet_parser_tests)

if(WIN32)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "x64 Windows is required")
    endif()

    if(NOT DEFINED NPCAP_SDK)
        set(NPCAP_SDK "$ENV{NPCAP_SDK}")
    endif()
    if(NOT NPCAP_SDK)
        set(NPCAP_SDK "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/npcap-sdk-1.17")
    endif()

    find_path(NPCAP_INCLUDE_DIR pcap.h HINTS "${NPCAP_SDK}/Include" REQUIRED)
    find_library(NPCAP_WPCAP_LIBRARY NAMES wpcap HINTS "${NPCAP_SDK}/Lib/x64" REQUIRED)
    find_library(NPCAP_PACKET_LIBRARY NAMES Packet HINTS "${NPCAP_SDK}/Lib/x64" REQUIRED)

    add_library(capture_core STATIC src/capture_service.cpp)
    target_include_directories(capture_core PUBLIC "${NPCAP_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(capture_core PUBLIC ethernet_parser "${NPCAP_WPCAP_LIBRARY}"
                          "${NPCAP_PACKET_LIBRARY}" Ws2_32)

    add_executable(packet_capture src/main.cpp src/display.cpp)
    target_link_libraries(packet_capture PRIVATE capture_core)

    add_executable(packet_capture_gui WIN32 src/gui_main.cpp)
    target_compile_definitions(packet_capture_gui PRIVATE UNICODE _UNICODE)
    target_link_libraries(packet_capture_gui PRIVATE capture_core User32 Gdi32 Comctl32)

    if(MSVC)
        foreach(target packet_capture packet_capture_gui)
            target_link_options(${target} PRIVATE /DELAYLOAD:wpcap.dll)
            target_link_libraries(${target} PRIVATE delayimp)
        endforeach()
        foreach(target ethernet_parser ethernet_parser_tests capture_core packet_capture packet_capture_gui)
            target_compile_options(${target} PRIVATE /W4 /utf-8 /EHsc)
        endforeach()
    endif()
endif()
