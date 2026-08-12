if(NOT DEFINED SOURCE_ROOT OR NOT DEFINED GAME_ROOT OR NOT DEFINED DEFAULT_OPTIONS)
    message(FATAL_ERROR "StageRuntime.cmake requires SOURCE_ROOT, GAME_ROOT and DEFAULT_OPTIONS")
endif()

# ReBedrock no longer ships Mojang's vanilla assets: textures, sounds, fonts and
# translations come from a standard resource pack the user drops into
# game/resourcepacks (see the required-pack check in Application). Only
# ReBedrock's own authored assets are staged below. A stale vanilla tree left by
# an older staging is removed so the release genuinely carries no Mojang content.
file(REMOVE_RECURSE "${GAME_ROOT}/resources/vanilla")

if(EXISTS "${SOURCE_ROOT}/animation")
    file(COPY "${SOURCE_ROOT}/animation" DESTINATION "${GAME_ROOT}/resources")
endif()

# Project-authored entity skins (the converted box-UV zombie skin) override the
# vanilla textures they were converted from, so they ship with the game too.
if(EXISTS "${SOURCE_ROOT}/entity")
    file(COPY "${SOURCE_ROOT}/entity" DESTINATION "${GAME_ROOT}/resources")
endif()

file(MAKE_DIRECTORY "${GAME_ROOT}/config")
file(MAKE_DIRECTORY "${GAME_ROOT}/saves")
file(COPY_FILE "${SOURCE_ROOT}/../CHANGELOG.md" "${GAME_ROOT}/CHANGELOG.md")
if(NOT EXISTS "${GAME_ROOT}/config/options.properties")
    file(COPY_FILE
        "${DEFAULT_OPTIONS}"
        "${GAME_ROOT}/config/options.properties")
endif()
file(WRITE "${GAME_ROOT}/resources/.runtime-assets-ready" "ready\n")
