cmake_minimum_required(VERSION 3.21)

set(CMAKE_VERBOSE_MAKEFILE ON)

project(aws_tcp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(CMAKE_CXX_EXTENSIONS False) # Use -std=c++14 instead of -std=gnu++14

if (CMAKE_CXX_COMPILER_ID MATCHES MSVC)
    add_compile_definitions(UNICODE _UNICODE)

    # https://stackoverflow.com/a/56490614
    if (CMAKE_BUILD_TYPE STREQUAL "Release")
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded") # Instead of MultiThreadedReleaseDLL
    elseif (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDebug") # Instead of MultiThreadedDebugDLL
    else()
        message(FATAL_ERROR "Must be either Debug or Release")
    endif()

	if (CMAKE_BUILD_TYPE STREQUAL "Release")
		# Visual Studio performance profiler for Release builds
		# add_link_options("/PROFILE")
		# add_compile_options("/Z7")
	endif()
elseif (CMAKE_CXX_COMPILER_ID MATCHES GNU)
    add_compile_options(-Wall)
    if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        add_link_options(-static -static-libgcc -static-libstdc++)
    endif()
    add_link_options(-pthread)

    # add_compile_options(-ftest-coverage -fprofile-arcs) # gcov

    # ./valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --verbose --log-file=output_file.txt ./executable_name
    # add_compile_options(-ggdb3 -fno-omit-frame-pointer) # g++ Linux valgrind
    # add_compile_options(-gdwarf-5 -fno-omit-frame-pointer) # clang++ Linux valgrind

endif()

add_executable(aws_tcp main.cpp)

target_include_directories(aws_tcp PUBLIC "${CMAKE_SOURCE_DIR}/libs/GSL-4.0.0/include")
target_include_directories(aws_tcp PUBLIC "${CMAKE_SOURCE_DIR}/libs/boost_1_79_0")

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if (CMAKE_CXX_COMPILER_ID MATCHES GNU)
        target_link_libraries(aws_tcp PUBLIC wsock32 ws2_32)
        # target_link_options(hello_world_exe PRIVATE -Wl,--subsystem,windows)
        # target_link_options(hello_world_exe PRIVATE -Wl,--subsystem,console)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES MSVC)
        # set_target_properties(hello_world_exe PROPERTIES LINK_FLAGS /SUBSYSTEM:WINDOWS)
        # set_target_properties(hello_world_exe PROPERTIES LINK_FLAGS /SUBSYSTEM:CONSOLE)
    endif()
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
else()
    message(FATAL_ERROR "Not Windows or Linux")
endif()
