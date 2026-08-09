if(NOT DEFINED SOURCE_ROOT OR NOT DEFINED GAME_ROOT OR NOT DEFINED DEFAULT_OPTIONS)
    message(FATAL_ERROR "StageRuntime.cmake requires SOURCE_ROOT, GAME_ROOT and DEFAULT_OPTIONS")
endif()

set(vanilla_source "${SOURCE_ROOT}/vanilla/1.16.1")
set(vanilla_target "${GAME_ROOT}/resources/vanilla/1.16.1")
file(MAKE_DIRECTORY "${vanilla_target}")
file(COPY "${vanilla_source}/textures" DESTINATION "${vanilla_target}")

# Every sound asset ships — the block families, the whole mob set (pig, cow,
# zombie and anything wired up later), weather/rain, ambient, entity, records,
# UI. Staging a subset repeatedly fell behind the wired-up sounds and spammed
# "Missing sound asset" at runtime, so copy the whole tree instead of
# cherry-picking groups. The 155 MB music catalogue is the exception: only the
# two classic C418 tracks — Sweden (music/game/calm1) and the main-menu theme
# (music/menu/menu1) — are staged, since no music player exists yet and the
# rest is dead weight.
file(COPY "${vanilla_source}/audio/minecraft/sounds"
     DESTINATION "${vanilla_target}/audio/minecraft"
     PATTERN "music" EXCLUDE)
# file(COPY) adds and overwrites but never removes stale destinations, so an
# earlier full-tree staging may have left the whole music catalogue behind —
# drop it explicitly, then stage only the two kept tracks.
file(REMOVE_RECURSE "${vanilla_target}/audio/minecraft/sounds/music")
file(MAKE_DIRECTORY "${vanilla_target}/audio/minecraft/sounds/music/game")
file(MAKE_DIRECTORY "${vanilla_target}/audio/minecraft/sounds/music/menu")
file(COPY "${vanilla_source}/audio/minecraft/sounds/music/game/calm1.ogg"
     DESTINATION "${vanilla_target}/audio/minecraft/sounds/music/game")
file(COPY "${vanilla_source}/audio/minecraft/sounds/music/menu/menu1.ogg"
     DESTINATION "${vanilla_target}/audio/minecraft/sounds/music/menu")

# glyph_sizes.bin drives the legacy unicode font used by the CJK languages.
if(EXISTS "${vanilla_source}/fonts")
    file(COPY "${vanilla_source}/fonts" DESTINATION "${vanilla_target}")
endif()

# Only the shipped interface languages are staged; the full vanilla set is
# roughly fifty megabytes of JSON.
set(localization_target "${vanilla_target}/localization/minecraft")
file(MAKE_DIRECTORY "${localization_target}")
foreach(language_code en_us zh_cn)
    set(language_file
        "${vanilla_source}/localization/minecraft/${language_code}.json")
    if(EXISTS "${language_file}")
        file(COPY_FILE "${language_file}"
             "${localization_target}/${language_code}.json")
    endif()
endforeach()

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
