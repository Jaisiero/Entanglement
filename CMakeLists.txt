cmake_minimum_required(VERSION 3.20)
project(Entanglement
    VERSION 0.1.0
    DESCRIPTION "A new network protocol"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Recopilar fuentes en src/
file(GLOB_RECURSE LIB_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

# Ejecutable principal
add_executable(${PROJECT_NAME} main.cpp ${LIB_SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE src)
