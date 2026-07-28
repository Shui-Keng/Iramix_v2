function(iramix_enable_sanitizers)
    if(
        NOT IRAMIX_ENABLE_SANITIZERS
        AND NOT IRAMIX_ENABLE_THREAD_SANITIZER
    )
        return()
    endif()

    if(
        IRAMIX_ENABLE_SANITIZERS
        AND IRAMIX_ENABLE_THREAD_SANITIZER
    )
        message(
            FATAL_ERROR
            "Address/undefined and thread sanitizers cannot be combined"
        )
    endif()

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(
            FATAL_ERROR
            "Iramix sanitizers require Clang or GCC"
        )
    endif()

    if(IRAMIX_ENABLE_THREAD_SANITIZER)
        set(sanitizerFlag -fsanitize=thread)
    else()
        set(sanitizerFlag -fsanitize=address,undefined)
    endif()

    add_compile_options(${sanitizerFlag} -fno-omit-frame-pointer)
    add_link_options(${sanitizerFlag} -fno-omit-frame-pointer)
endfunction()
