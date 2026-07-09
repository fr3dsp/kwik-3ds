cmake_minimum_required(VERSION 3.12)
project(kwik_game C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(KWIK_DIR "@KWIK_DIR@" CACHE PATH "kwik repo root")

add_subdirectory(${KWIK_DIR}/runtime ${CMAKE_BINARY_DIR}/kwik_runtime)

file(GLOB GAME_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/objects/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rooms/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.cpp)

add_executable(game ${GAME_SOURCES})
target_include_directories(game PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(game PRIVATE kwik_runtime)

if(MSVC)
    set_target_properties(game PROPERTIES LINK_FLAGS "/STACK:8388608")
endif()

if(NINTENDO_3DS)
    find_program(KWIK_3DSXTOOL 3dsxtool PATHS $ENV{DEVKITPRO}/tools/bin)
    find_program(KWIK_SMDHTOOL smdhtool PATHS $ENV{DEVKITPRO}/tools/bin)
    if(NOT KWIK_3DSXTOOL OR NOT KWIK_SMDHTOOL)
        message(FATAL_ERROR "kwik: could not find 3dsxtool/smdhtool - is DEVKITPRO set correctly?")
    endif()

    if(NOT KWIK_3DS_TITLE)
        set(KWIK_3DS_TITLE "${CMAKE_PROJECT_NAME}")
    endif()
    if(NOT KWIK_3DS_AUTHOR)
        set(KWIK_3DS_AUTHOR "kwik")
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/icon.png)
        set(KWIK_3DS_ICON ${CMAKE_CURRENT_SOURCE_DIR}/icon.png)
    else()
        set(KWIK_3DS_ICON ${KWIK_DIR}/assets/default_3ds_icon.png)
    endif()

    set(KWIK_SDCARD_APP_DIR ${CMAKE_BINARY_DIR}/sdcard/3ds/${KWIK_3DS_TITLE})
    file(MAKE_DIRECTORY ${KWIK_SDCARD_APP_DIR})

    set(KWIK_SMDH_OUT ${CMAKE_CURRENT_BINARY_DIR}/game.smdh)
    add_custom_command(TARGET game POST_BUILD
        COMMAND ${KWIK_SMDHTOOL} --create "${KWIK_3DS_TITLE}" "Built with kwik" "${KWIK_3DS_AUTHOR}"
                ${KWIK_3DS_ICON} ${KWIK_SMDH_OUT}
        BYPRODUCTS ${KWIK_SMDH_OUT})

    set(KWIK_3DSX_OUT ${KWIK_SDCARD_APP_DIR}/game.3dsx)
    add_custom_command(TARGET game POST_BUILD
        COMMAND ${KWIK_3DSXTOOL} $<TARGET_FILE:game> ${KWIK_3DSX_OUT} --smdh=${KWIK_SMDH_OUT}
        BYPRODUCTS ${KWIK_3DSX_OUT})

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Assets.dat)
        set(KWIK_REPACK_TOOL "" CACHE FILEPATH
            "Path 2 texture repack tool.")
        if(NOT KWIK_REPACK_TOOL)
            file(GLOB KWIK_REPACK_TOOL_CANDIDATES
                 ${KWIK_DIR}/build*/compiler/kwik_repack_textures
                 ${KWIK_DIR}/build*/compiler/kwik_repack_textures.exe)
            list(LENGTH KWIK_REPACK_TOOL_CANDIDATES KWIK_REPACK_TOOL_CANDIDATE_COUNT)
            if(KWIK_REPACK_TOOL_CANDIDATE_COUNT GREATER 0)
                list(GET KWIK_REPACK_TOOL_CANDIDATES 0 KWIK_REPACK_TOOL_FOUND)
                set(KWIK_REPACK_TOOL ${KWIK_REPACK_TOOL_FOUND} CACHE FILEPATH "" FORCE)
                message(STATUS "kwik: auto-detected texture repack tool at ${KWIK_REPACK_TOOL}")
            else()
                message(STATUS "no repack 2ool found lol!")
            endif()
        endif()
        find_program(KWIK_TEX3DS tex3ds PATHS $ENV{DEVKITPRO}/tools/bin)

        if(KWIK_REPACK_TOOL AND KWIK_TEX3DS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/game_data.cpp)
            file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/game_data.cpp KWIK_IMG_COUNT_LINE
                 REGEX "g_image_count = ")
            file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/game_data.cpp KWIK_SND_COUNT_LINE
                 REGEX "g_sound_count = ")
            string(REGEX MATCH "[0-9]+" KWIK_IMAGE_COUNT "${KWIK_IMG_COUNT_LINE}")
            string(REGEX MATCH "[0-9]+" KWIK_SOUND_COUNT "${KWIK_SND_COUNT_LINE}")

            if(KWIK_IMAGE_COUNT MATCHES "^[0-9]+$" AND KWIK_SOUND_COUNT MATCHES "^[0-9]+$")
                add_custom_command(TARGET game POST_BUILD
                    COMMAND ${KWIK_REPACK_TOOL}
                            ${CMAKE_CURRENT_SOURCE_DIR}/Assets.dat
                            ${KWIK_SDCARD_APP_DIR}/Assets.dat
                            ${KWIK_IMAGE_COUNT} ${KWIK_SOUND_COUNT} ${KWIK_TEX3DS}
                    COMMENT "kwik: repacking textures to .t3x via ${KWIK_REPACK_TOOL}")
            else()
                message(WARNING "kwik: could not read g_image_count/g_sound_count from game_data.cpp, falling back to a plain Assets.dat copy")
                add_custom_command(TARGET game POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            ${CMAKE_CURRENT_SOURCE_DIR}/Assets.dat ${KWIK_SDCARD_APP_DIR}/Assets.dat)
            endif()
        else()
            add_custom_command(TARGET game POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${CMAKE_CURRENT_SOURCE_DIR}/Assets.dat ${KWIK_SDCARD_APP_DIR}/Assets.dat)
        endif()
    else()
        message(WARNING "kwik: Assets.dat not found next to this project")
    endif()

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/romfs)
        add_custom_command(TARGET game POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${CMAKE_CURRENT_SOURCE_DIR}/romfs ${KWIK_SDCARD_APP_DIR})
    endif()

    message(STATUS "kwik: 3DS build output: ${KWIK_3DSX_OUT}")
    message(STATUS "kwik: copy ${CMAKE_BINARY_DIR}/sdcard/3ds onto your SD card's /3ds folder")
endif()
