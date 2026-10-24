# Functional tests over pty pairs (POSIX desktop only).
if(UNIX AND NOT ANDROID)
    add_executable(cs_tests test_serial.cpp)
    target_link_libraries(cs_tests PRIVATE crossserial)
    target_include_directories(cs_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../testutil)
    add_test(NAME cs_tests COMMAND cs_tests)
endif()
