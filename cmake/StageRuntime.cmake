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

# Entity skins are NOT staged. The only file that ever lived under
# resources/entity was converted from Mojang's zombie skin, so shipping it
# contradicted the no-vanilla-assets rule above. Entity textures now come from
# the standard pack (minecraft:textures/entity/...), or from the procedural
# placeholder buildSpeciesSkin paints. A stale tree from an older staging is
# removed so an upgraded install stops carrying it too.
file(REMOVE_RECURSE "${GAME_ROOT}/resources/entity")

# Project-owned UI translations use their own namespace. Vanilla's language
# files still come exclusively from the required standard resource pack.
if(EXISTS "${SOURCE_ROOT}/lang")
    file(COPY "${SOURCE_ROOT}/lang" DESTINATION "${GAME_ROOT}/resources")
endif()

file(MAKE_DIRECTORY "${GAME_ROOT}/config")
file(MAKE_DIRECTORY "${GAME_ROOT}/saves")
file(COPY_FILE "${SOURCE_ROOT}/../CHANGELOG.md" "${GAME_ROOT}/CHANGELOG.md")
file(COPY_FILE "${SOURCE_ROOT}/../CHANGELOG_EN.md" "${GAME_ROOT}/CHANGELOG_EN.md")
file(COPY_FILE "${SOURCE_ROOT}/../CHANGELOG_CH.md" "${GAME_ROOT}/CHANGELOG_CH.md")
if(NOT EXISTS "${GAME_ROOT}/config/options.properties")
    file(COPY_FILE
        "${DEFAULT_OPTIONS}"
        "${GAME_ROOT}/config/options.properties")
endif()
file(WRITE "${GAME_ROOT}/resources/.runtime-assets-ready" "ready\n")
