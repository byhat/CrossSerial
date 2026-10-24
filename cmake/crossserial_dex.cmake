# crossserial_embed_dex(TARGET JAVA_DIR USB_SERIAL_DIR GENERATED_DIR MIN_API)
#
# Compiles the Java bridge + vendored usb-serial-for-android into classes.dex,
# then embeds the dex bytes as a C array compiled into the target.
#
# Requires a JDK (javac) on PATH and ANDROID_SDK_ROOT pointing at the Android
# SDK (with platforms/ and build-tools/ installed).

cmake_minimum_required(VERSION 3.16)

set(CS_DEX_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(crossserial_embed_dex TARGET_NAME)
    cmake_parse_arguments(ARG "" "JAVA_DIR;USB_SERIAL_DIR;GENERATED_DIR;MIN_API" "" ${ARGN})

    set(DEX_OUT     "${ARG_GENERATED_DIR}/classes.dex")
    set(CLASSES_DIR "${ARG_GENERATED_DIR}/classes")
    set(JAR_OUT     "${ARG_GENERATED_DIR}/crossserial-classes.jar")
    set(GEN_CPP     "${ARG_GENERATED_DIR}/crossserial_classes_dex.cpp")

    # ---- discover toolchain --------------------------------------------------
    find_program(CS_JAVAC NAMES javac)
    if(NOT CS_JAVAC)
        message(FATAL_ERROR "javac not found; a JDK is required to build the Android dex.")
    endif()

    if(NOT ANDROID_SDK_ROOT)
        message(FATAL_ERROR "ANDROID_SDK_ROOT must be set for Android builds (need javac + d8 + android.jar).")
    endif()

    file(GLOB CS_ANDROID_JARS "${ANDROID_SDK_ROOT}/platforms/android-*/android.jar")
    list(SORT CS_ANDROID_JARS)
    list(GET CS_ANDROID_JARS -1 CS_ANDROID_JAR)
    if(NOT CS_ANDROID_JAR)
        message(FATAL_ERROR "No android.jar under ${ANDROID_SDK_ROOT}/platforms/. Install an Android platform.")
    endif()

    if(CMAKE_HOST_WIN32)
        file(GLOB CS_D8_CANDIDATES "${ANDROID_SDK_ROOT}/build-tools/*/d8.bat")
    else()
        file(GLOB CS_D8_CANDIDATES "${ANDROID_SDK_ROOT}/build-tools/*/d8")
    endif()
    list(SORT CS_D8_CANDIDATES)
    if(NOT CS_D8_CANDIDATES)
        message(FATAL_ERROR "d8 not found under ${ANDROID_SDK_ROOT}/build-tools/. Install Android build-tools.")
    endif()
    list(GET CS_D8_CANDIDATES -1 CS_D8)

    # ---- collect java sources ------------------------------------------------
    file(GLOB_RECURSE CS_BRIDGE_JAVA    "${ARG_JAVA_DIR}/*.java")
    file(GLOB_RECURSE CS_USBSERIAL_JAVA "${ARG_USB_SERIAL_DIR}/usbSerialForAndroid/src/main/java/*.java")

    # Minimal stubs (androidx.annotation.IntDef, BuildConfig) so the vendored
    # usb-serial-for-android sources compile with plain javac, no Gradle/AndroidX.
    set(CS_STUBS_DIR "${CMAKE_SOURCE_DIR}/android/stubs")
    file(GLOB_RECURSE CS_STUBS_JAVA "${CS_STUBS_DIR}/*.java")

    set(CS_ALL_JAVA ${CS_BRIDGE_JAVA} ${CS_USBSERIAL_JAVA} ${CS_STUBS_JAVA})
    if(NOT CS_ALL_JAVA)
        message(WARNING "No Java sources found for dex embedding.")
    endif()

    # ---- compile java -> jar -> dex ------------------------------------------
    add_custom_command(
        OUTPUT "${DEX_OUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CLASSES_DIR}"
        COMMAND "${CS_JAVAC}" -source 1.8 -target 1.8 -encoding UTF-8
                -classpath "${CS_ANDROID_JAR}"
                -d "${CLASSES_DIR}"
                ${CS_ALL_JAVA}
        COMMAND ${CMAKE_COMMAND} -E chdir "${CLASSES_DIR}" ${CMAKE_COMMAND}
                -E tar cf "${JAR_OUT}" --format=zip .
        COMMAND "${CS_D8}" --min-api "${ARG_MIN_API}" --lib "${CS_ANDROID_JAR}"
                --output "${ARG_GENERATED_DIR}" "${JAR_OUT}"
        DEPENDS ${CS_ALL_JAVA}
        COMMENT "Building embedded classes.dex (crossserial Android bridge)"
        VERBATIM)

    # ---- embed dex bytes as a C array ----------------------------------------
    add_custom_command(
        OUTPUT "${GEN_CPP}"
        COMMAND ${CMAKE_COMMAND}
            "-DCS_DEX_IN=${DEX_OUT}"
            "-DCS_CPP_OUT=${GEN_CPP}"
            -P "${CS_DEX_MODULE_DIR}/crossserial_dex_embed.cmake"
        DEPENDS "${DEX_OUT}"
        COMMENT "Embedding classes.dex into crossserial_classes_dex.cpp"
        VERBATIM)

    target_sources(${TARGET_NAME} PRIVATE "${GEN_CPP}")
endfunction()
