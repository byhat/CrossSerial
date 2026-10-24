cmake_minimum_required(VERSION 3.16)

project(crossserial
    VERSION 1.0.0
    DESCRIPTION "Cross-platform serial port library (Linux/Windows/Android), QSerialPort-like API, no Qt"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

# ---- options ---------------------------------------------------------------

option(CROSSSERIAL_BUILD_SHARED  "Build crossserial as a shared library (otherwise static)" ON)
option(CROSSSERIAL_BUILD_EXAMPLES "Build the example programs" ON)
option(CROSSSERIAL_BUILD_TESTS   "Build the test programs" ON)
set(CROSSSERIAL_ANDROID_MIN_API "21" CACHE STRING "Android min API level for the embedded dex (Android only)")

# ---- target ----------------------------------------------------------------

set(CS_LIB_TYPE STATIC)
if(CROSSSERIAL_BUILD_SHARED)
    set(CS_LIB_TYPE SHARED)
endif()

add_library(crossserial ${CS_LIB_TYPE}
    src/SerialPort.cpp
    src/SerialPortInfo.cpp
)

target_include_directories(crossserial
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(crossserial PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME crossserial
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

if(CROSSSERIAL_BUILD_SHARED)
    target_compile_definitions(crossserial PRIVATE CROSSSERIAL_EXPORTS)
else()
    target_compile_definitions(crossserial PUBLIC CROSSSERIAL_STATIC_DEFINE)
endif()

if(ANDROID)
    # ---- Android: JNI bridge into usb-serial-for-android + embedded dex ------
    target_sources(crossserial PRIVATE src/SerialPortAndroid.cpp)
    target_link_libraries(crossserial PRIVATE log android)

    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/crossserial_dex.cmake)
    crossserial_embed_dex(crossserial
        JAVA_DIR       "${CMAKE_CURRENT_SOURCE_DIR}/android/java"
        USB_SERIAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/usb-serial-for-android"
        GENERATED_DIR  "${CMAKE_CURRENT_BINARY_DIR}/generated"
        MIN_API        "${CROSSSERIAL_ANDROID_MIN_API}")
else()
    # ---- Desktop (Linux/Windows): boost::asio --------------------------------
    # Boost is PUBLIC: the public SerialPort.hpp includes <boost/asio.hpp>
    # (no pimpl by design - see README), so consumers need the headers too.
    target_sources(crossserial PRIVATE src/SerialPortDesktop.cpp)

    find_package(Boost REQUIRED)
    if(TARGET Boost::headers)
        target_link_libraries(crossserial PUBLIC Boost::headers)
    else()
        target_include_directories(crossserial PUBLIC ${Boost_INCLUDE_DIRS})
    endif()

    if(WIN32)
        # setupapi: port enumeration (impl detail). ws2_32 must be PUBLIC: the
        # public header includes <boost/asio.hpp>, whose inline winsock calls
        # (WSAStartup & friends) land in consumer TUs as well.
        target_link_libraries(crossserial PRIVATE setupapi PUBLIC ws2_32)
    endif()
endif()

# ---- examples / tests ------------------------------------------------------

if(CROSSSERIAL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(CROSSSERIAL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- install + export ------------------------------------------------------

install(TARGETS crossserial
    EXPORT crossserialTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT crossserialTargets
    FILE crossserialTargets.cmake
    NAMESPACE crossserial::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/crossserial)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/crossserialConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/crossserialConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/crossserialConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/crossserial)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/crossserialConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/crossserialConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/crossserial)
