add_executable(test_logger test_logger.cpp)
target_link_libraries(test_logger 
    mini_ccl
    fmt::fmt
)
target_include_directories(test_logger
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)


add_executable(test_misc test_misc.cpp)
target_link_libraries(test_misc 
    mini_ccl
    fmt::fmt
)
target_include_directories(test_misc
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)


add_executable(test_transport_tcp test_transport_tcp.cpp)
target_link_libraries(test_transport_tcp 
    mini_ccl
    fmt::fmt
)
target_include_directories(test_transport_tcp
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)


add_executable(test_ccl test_ccl.cpp)
target_link_libraries(test_ccl 
    mini_ccl
    fmt::fmt
)
target_include_directories(test_ccl
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)


add_executable(test_zero_copy test_zero_copy.cpp)
target_link_libraries(test_zero_copy 
    mini_ccl
    fmt::fmt
)
target_include_directories(test_zero_copy
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)


add_executable(test_polling test_polling.cpp)
target_link_libraries(test_polling 
    mini_ccl
    fmt::fmt
)
target_include_directories(test_polling
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)
