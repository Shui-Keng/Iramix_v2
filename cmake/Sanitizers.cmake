function(iramix_enable_sanitizers)
    if(NOT IRAMIX_ENABLE_SANITIZERS)
        return()
    endif()

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(
            FATAL_ERROR
            "IRAMIX_ENABLE_SANITIZERS requires Clang or GCC"
        )
    endif()

    add_compile_options(
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
    add_link_options(
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
endfunction()
