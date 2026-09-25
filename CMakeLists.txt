cmake_minimum_required(VERSION 3.10)
project(sbtb C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)


add_compile_options(-O -Wall -Wextra -pedantic -Wold-style-declaration -Wmissing-prototypes -Wno-unused-parameter)

find_package(PkgConfig REQUIRED)
find_library(X11_LIB X11 REQUIRED)
find_package(X11 REQUIRED)
find_library(XRANDR_LIB Xrandr REQUIRED)
find_library(XINERAMA_LIB Xinerama REQUIRED)
find_library(XCOMPOSITE_LIB Xcomposite REQUIRED)
find_library(XEXT_LIB Xext REQUIRED)
find_library(GL_LIB GL REQUIRED)
find_package(PNG REQUIRED)
find_package(Threads REQUIRED)

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/src/config.h")
    configure_file(${CMAKE_SOURCE_DIR}/src/config.def.h ${CMAKE_SOURCE_DIR}/src/config.h COPYONLY)
endif()

add_executable(sbtb src/sbtb.c)

target_include_directories(sbtb PRIVATE ${CMAKE_SOURCE_DIR} ${XFT_INCLUDE_DIRS})
target_link_libraries(sbtb
    ${X11_LIB}
    ${XINERAMA_LIB}
    ${XCOMPOSITE_LIB}
    ${XEXT_LIB}
    ${XRANDR_LIB}
    ${XFT_LIBRARIES}
    ${GL_LIB}
    PNG::PNG
    Threads::Threads
)
target_link_directories(sbtb PRIVATE ${XFT_LIBRARY_DIRS})
target_compile_options(sbtb PRIVATE ${XFT_CFLAGS_OTHER})

add_custom_command(TARGET sbtb POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/src/dicon.png
        ${CMAKE_BINARY_DIR}/bin/dicon.png)

install(TARGETS sbtb RUNTIME DESTINATION bin)
install(FILES src/dicon.png DESTINATION bin)
