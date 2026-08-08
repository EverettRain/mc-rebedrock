if(NOT DEFINED SOURCE_ROOT OR NOT DEFINED GAME_ROOT OR NOT DEFINED DEFAULT_OPTIONS)
    message(FATAL_ERROR "StageRuntime.cmake requires SOURCE_ROOT, GAME_ROOT and DEFAULT_OPTIONS")
endif()

set(vanilla_source "${SOURCE_ROOT}/vanilla/1.16.1")
set(vanilla_target "${GAME_ROOT}/resources/vanilla/1.16.1")
file(MAKE_DIRECTORY "${vanilla_target}")
file(COPY "${vanilla_source}/textures" DESTINATION "${vanilla_target}")

set(sound_target "${vanilla_target}/audio/minecraft/sounds")
file(MAKE_DIRECTORY "${sound_target}")
foreach(sound_group dig step liquid random damage)
    file(COPY "${vanilla_source}/audio/minecraft/sounds/${sound_group}"
         DESTINATION "${sound_target}")
endforeach()
# The pig is the only mob whose sounds are wired up (PigEntity's hurt and death
# clips), so stage just its folder instead of the whole ~24 MB mob group.
if(EXISTS "${vanilla_source}/audio/minecraft/sounds/mob/pig")
    file(MAKE_DIRECTORY "${sound_target}/mob")
    file(COPY "${vanilla_source}/audio/minecraft/sounds/mob/pig"
         DESTINATION "${sound_target}/mob")
endif()

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
