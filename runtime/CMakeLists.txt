set(KWIK_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

if(NINTENDO_3DS)
    find_program(KWIK_PICASSO picasso PATHS $ENV{DEVKITPRO}/tools/bin)
    find_program(KWIK_BIN2S bin2s PATHS $ENV{DEVKITPRO}/tools/bin)
    if(NOT KWIK_PICASSO OR NOT KWIK_BIN2S)
        message(FATAL_ERROR "kwik: could not find picasso/bin2s - is DEVKITPRO set correctly?")
    endif()

    set(KWIK_VSHADER_PICA ${CMAKE_CURRENT_SOURCE_DIR}/source/backends/3ds/vshader.v.pica)
    set(KWIK_VSHADER_SHBIN ${CMAKE_CURRENT_BINARY_DIR}/vshader.shbin)
    set(KWIK_VSHADER_ASM ${CMAKE_CURRENT_BINARY_DIR}/vshader_shbin.s)

    add_custom_command(
        OUTPUT ${KWIK_VSHADER_SHBIN}
        COMMAND ${KWIK_PICASSO} -o ${KWIK_VSHADER_SHBIN} ${KWIK_VSHADER_PICA}
        DEPENDS ${KWIK_VSHADER_PICA}
        COMMENT "kwik: compiling vshader.v.pica"
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${KWIK_VSHADER_ASM}
        COMMAND sh -c "${KWIK_BIN2S} ${KWIK_VSHADER_SHBIN} > ${KWIK_VSHADER_ASM}"
        DEPENDS ${KWIK_VSHADER_SHBIN}
        COMMENT "kwik: embedding vshader.shbin"
        VERBATIM
    )

    enable_language(ASM)

    add_library(kwik_runtime STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/source/value.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/game.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/builtins_core.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/builtins_data.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/builtins_draw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/builtins_misc.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/builtins_ch2.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/builtins_ch5.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/backends/3ds.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/assets_rt.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/audio_stub_3ds.cpp
        ${KWIK_THIRD_PARTY}/stb/stb_vorbis.c
        ${KWIK_VSHADER_ASM}
    )

    set_target_properties(kwik_runtime PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(kwik_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_include_directories(kwik_runtime PRIVATE ${KWIK_THIRD_PARTY}/stb)

    target_link_libraries(kwik_runtime PUBLIC citro3d ctru m)
    target_compile_definitions(kwik_runtime PUBLIC __3DS__)

    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
    endif()

    find_program(KWIK_CCACHE ccache)
    if(KWIK_CCACHE)
        set(CMAKE_C_COMPILER_LAUNCHER ${KWIK_CCACHE} PARENT_SCOPE)
        set(CMAKE_CXX_COMPILER_LAUNCHER ${KWIK_CCACHE} PARENT_SCOPE)
        set(CMAKE_C_COMPILER_LAUNCHER ${KWIK_CCACHE})
        set(CMAKE_CXX_COMPILER_LAUNCHER ${KWIK_CCACHE})
        message(STATUS "kwik: using ccache for 3DS builds")
    endif()

    target_compile_options(kwik_runtime PUBLIC -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(kwik_runtime PUBLIC -O0 -g)
    else()
        target_compile_options(kwik_runtime PUBLIC -O2)
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
