set(KWIK_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

if(NINTENDO_3DS)
    find_library(SDL2_3DS_LIB SDL2 PATHS $ENV{DEVKITPRO}/portlibs/3ds/lib)
    find_path(SDL2_3DS_INCLUDE SDL2/SDL.h PATHS $ENV{DEVKITPRO}/portlibs/3ds/include)
    if(NOT SDL2_3DS_LIB OR NOT SDL2_3DS_INCLUDE)
        message(FATAL_ERROR "kwik: could not find the 3ds-sdl2 portlib - install it with dkp-pacman -S 3ds-sdl2")
    endif()

    add_library(kwik_runtime STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/source/value.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/game.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/builtins_core.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/builtins_data.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/builtins_draw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/builtins_misc.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/builtins_ch2.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/builtins_ch5.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/backends/sdl2.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/assets_rt.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/audio_stub_3ds.cpp
        ${KWIK_THIRD_PARTY}/stb/stb_vorbis.c
    )

    set_target_properties(kwik_runtime PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(kwik_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_include_directories(kwik_runtime PRIVATE ${KWIK_THIRD_PARTY}/stb ${SDL2_3DS_INCLUDE})

    target_link_libraries(kwik_runtime PUBLIC ${SDL2_3DS_LIB} citro3d ctru m)
    target_compile_definitions(kwik_runtime PUBLIC __3DS__)
    target_compile_options(kwik_runtime PUBLIC -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O2 -ffast-math)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
    endif()

    return()
endif()

find_package(Threads REQUIRED)

set(KWIK_BACKEND "sdl2" CACHE STRING "render backend: glfw or sdl2")

if(KWIK_BACKEND STREQUAL "sdl2")
    find_package(SDL2 REQUIRED)
    set(KWIK_BACKEND_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/source/backends/sdl2.cpp)
else()
    find_package(OpenGL REQUIRED)
    find_package(glfw3 REQUIRED)
    set(KWIK_BACKEND_SOURCE
        ${CMAKE_CURRENT_SOURCE_DIR}/source/backends/opengl.cpp
        ${KWIK_THIRD_PARTY}/glad/src/glad.c
    )
endif()

add_library(kwik_runtime STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/source/value.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/game.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/builtins_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/builtins_data.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/builtins_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/builtins_misc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/builtins_ch2.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/builtins_ch5.cpp
    ${KWIK_BACKEND_SOURCE}
    ${CMAKE_CURRENT_SOURCE_DIR}/source/assets_rt.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/audio.cpp
    ${KWIK_THIRD_PARTY}/stb/stb_vorbis.c
)

set_target_properties(kwik_runtime PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

target_include_directories(kwik_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(kwik_runtime PRIVATE
    ${KWIK_THIRD_PARTY}/stb
    ${KWIK_THIRD_PARTY}/miniaudio
)

if(KWIK_BACKEND STREQUAL "sdl2")
    if(TARGET SDL2::SDL2)
        set(KWIK_WINDOW_LIBS SDL2::SDL2)
    else()
        target_include_directories(kwik_runtime PRIVATE ${SDL2_INCLUDE_DIRS})
        set(KWIK_WINDOW_LIBS ${SDL2_LIBRARIES})
    endif()
else()
    target_include_directories(kwik_runtime PRIVATE ${KWIK_THIRD_PARTY}/glad/include)
    set(KWIK_WINDOW_LIBS glfw OpenGL::GL)
endif()

if(WIN32)
    target_link_libraries(kwik_runtime PUBLIC ${KWIK_WINDOW_LIBS} ${CMAKE_DL_LIBS} Threads::Threads)
    target_compile_definitions(kwik_runtime PRIVATE _USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_link_libraries(kwik_runtime PUBLIC ${KWIK_WINDOW_LIBS} ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
