#pragma once

#include "gameplay/GameMode.hpp"
#include "persistence/SaveRepository.hpp"
#include "ui/Language.hpp"
#include "ui/PageStack.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mc::ui {

enum class CreativeTab : std::uint8_t {
    BuildingBlocks,
    Decoration,
    Functional,
    Materials,
    Food,
    Tools,
    SpawnEggs,
    Inventory,
};

struct DisplayResolution final {
    int width = 0;
    int height = 0;
};

inline constexpr std::array<DisplayResolution, 16> kDisplayResolutions{{
    {800, 600},
    {960, 540},
    {960, 720},
    {1024, 768},
    {1152, 864},
    {1280, 720},
    {1280, 800},
    {1366, 768},
    {1440, 900},
    {1536, 864},
    {1600, 900},
    {1680, 1050},
    {1920, 1080},
    {2048, 1152},
    {2560, 1440},
    {3840, 2160},
}};

// Owns the front-end menu state: the page stack, the world list, the language
// list and the options-screen selections. The renderer drives it through these
// fields and keeps the Vulkan/GLFW/audio plumbing to itself. Menu *logic* that
// touches the renderer (save/load, cursor capture, audio) still lives in the
// renderer's handlers, which read/write this state.
class MenuSystem final {
  public:
    ui::PageStack pageStack;
    std::vector<persistence::SaveSummary> saveSummaries;
    std::size_t selectedWorldIndex = 0U;
    std::size_t worldListFirstIndex = 0U;
    std::string createWorldName = "New World";
    gameplay::GameMode createWorldGameMode = gameplay::GameMode::Survival;
    std::string editWorldName;
    std::string editWorldIdentifier;
    std::string saveStatus;
    std::vector<std::string> languageCodes{std::string{kDefaultLanguageCode}};
    std::vector<std::string> languageDisplayNames;
    std::size_t languageListFirstIndex = 0U;
    bool optionsOpen = false;
    bool viewDistanceSliderDragging = false;
    bool simulationDistanceSliderDragging = false;
    bool masterVolumeSliderDragging = false;
    int guiScaleSetting = 0;
    std::size_t resolutionIndex = 0;
    CreativeTab creativeTab = CreativeTab::BuildingBlocks;
    std::size_t creativeScrollRow = 0;
};

} // namespace mc::ui
