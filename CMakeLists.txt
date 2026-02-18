cmake_minimum_required(VERSION 3.5)
project(lsd)
enable_testing()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


include(FetchContent)

# ---- OpenGL ----
find_package(OpenGL REQUIRED)

# ---- FreeType ----
find_package(Freetype REQUIRED)

# ---- GLFW ----
FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
)



add_executable(lsd
        src/main.cpp
        external/glad/src/glad.c
        src/tty.cpp
        src/tty.h
        src/lsd_pty.cpp
        src/lsd_pty.h
)

target_include_directories(lsd PRIVATE
        external/glad/include
        src/
)

target_link_libraries(lsd
        PRIVATE
        glfw
        OpenGL::GL
        Freetype::Freetype
)


add_executable(echo_test
        tests/echo_test.cpp
)

target_include_directories(echo_test PRIVATE src/)

add_test(NAME echo COMMAND echo_test)
