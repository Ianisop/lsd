cmake_minimum_required(VERSION 4.0)
project(lsd)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

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
FetchContent_MakeAvailable(glfw)

# ---- GLM ----
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 0.9.9.8
)
FetchContent_MakeAvailable(glm)

# ---- Executable ----
add_executable(lsd
    src/lsd.cpp
    src/lsd.h
    src/types/lsd_types.h
    external/glad/src/glad.c
    src/tty.cpp
    src/tty.h
    src/parse/ansi_parser.h
    src/parse/csi_parser.h
    src/parse/osc_parser.h
    src/lsd_pty.cpp
    src/lsd_pty.h
)

target_include_directories(lsd PRIVATE
    external/glad/include
    src/
    ${glm_SOURCE_DIR}
)

target_link_libraries(lsd
    PRIVATE
    glfw
    OpenGL::GL
    Freetype::Freetype
)

# Add pthread on Linux
if (UNIX AND NOT APPLE)
    target_link_libraries(lsd PRIVATE pthread)
endif()

# Optional: Add compiler warnings
target_compile_options(lsd PRIVATE
    -Wall -Wextra -Wpedantic
)

# ---- Tests ----
add_executable(echo_test
    tests/echo_test.cpp
)

target_include_directories(echo_test PRIVATE src/)

add_test(NAME echo COMMAND echo_test)