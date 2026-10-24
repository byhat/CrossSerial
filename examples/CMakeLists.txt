# ---- console examples (desktop only; they cannot run or be deployed
# as standalone executables on Android anyway) -------------------------------

if(NOT ANDROID)
    add_executable(cs_list_ports list_ports.cpp)
    target_link_libraries(cs_list_ports PRIVATE crossserial)

    add_executable(cs_terminal terminal.cpp)
    target_link_libraries(cs_terminal PRIVATE crossserial)
endif()

# ---- throughput/latency benchmark (POSIX desktop; Qt part optional) --------

if(UNIX AND NOT ANDROID)
    find_package(Qt6 COMPONENTS Core SerialPort QUIET)
    add_executable(cs_bench bench.cpp)
    target_link_libraries(cs_bench PRIVATE crossserial)
    target_include_directories(cs_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../testutil)
    if(Qt6_FOUND)
        target_link_libraries(cs_bench PRIVATE Qt6::Core Qt6::SerialPort)
        target_compile_definitions(cs_bench PRIVATE CS_BENCH_HAVE_QT)
        message(STATUS "cs_bench: Qt6 found, QSerialPort comparison enabled")
    else()
        message(STATUS "cs_bench: Qt6 not found, QSerialPort comparison disabled")
    endif()
endif()

# ---- Qt QML terminal example (desktop + Android, optional) ------------------

find_package(Qt6 COMPONENTS Quick Qml QUIET)
if(Qt6_FOUND)
    add_subdirectory(qt_qml_terminal)
endif()
