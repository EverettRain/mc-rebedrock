#include "persistence/SaveRepository.hpp"

#include "assets/ImageData.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>

// The save thumbnail's storage layer: where icon.png lives, what writeIcon
// refuses, and how a listing reports "this world has one" without paying to
// decode it. Everything here is the backend half — no screen is involved.
//
// Two of these questions have no other guard anywhere. The first is the byte
// count: writeIcon hands a raw pointer and a width/height to a PNG encoder, so
// a span that is short for its declared dimensions is an out-of-bounds *read*,
// not a wrong-looking picture — a bug that produces a perfectly valid file and
// a silent heap overread. The second is that the listing must decide hasIcon by
// asking the filesystem, not by decoding: a world list that loaded every
// world's 16 KB of pixels to draw a screen that may show none would still pass
// every "is the flag right" assertion.

namespace {

using namespace mc;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"save_icon_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

// A test root nobody else is using, so a stale directory from an earlier run
// cannot make an assertion pass (or fail) for the wrong reason.
[[nodiscard]] std::filesystem::path makeUniqueRoot() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ("mc-rebedrock-save-icon-test-" + std::to_string(unique));
}

// A thumbnail whose every pixel is different from every other one. A flat
// colour would pass just as happily if the encoder wrote a *different* flat
// colour, if the rows came out flipped, or if the channels were rotated — so
// the fixture has to vary along both axes and across all four channels, or the
// "read it back and compare" assertion cannot separate "wrote this picture"
// from "wrote some picture of the right size".
[[nodiscard]] std::vector<std::uint8_t> makeGradient(std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4U);
    for (std::uint32_t y = 0U; y < height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4U;
            rgba[offset + 0U] = static_cast<std::uint8_t>(x * 3U + 1U);
            rgba[offset + 1U] = static_cast<std::uint8_t>(y * 5U + 2U);
            rgba[offset + 2U] = static_cast<std::uint8_t>(x * 7U + y * 11U + 3U);
            // Alpha varies too: an encoder told "3 channels" would still produce
            // a readable PNG, and only a varying alpha proves the fourth channel
            // made the trip.
            rgba[offset + 3U] = static_cast<std::uint8_t>(255U - ((x + y) & 0x3FU));
        }
    }
    return rgba;
}

// Creates a real, listable world (level.properties + world.dat on disk) and
// returns its identifier. list() skips a directory with no metadata, so a
// hasIcon assertion needs a world that actually appears in the listing.
[[nodiscard]] std::string createWorld(const persistence::SaveRepository& repository,
                                      const std::string& displayName) {
    auto game = repository.create(displayName, 1234ULL);
    repository.save(game);
    return game.summary.identifier;
}

[[nodiscard]] const persistence::SaveSummary* findSummary(
    const std::vector<persistence::SaveSummary>& saves, std::string_view identifier) {
    const auto found = std::ranges::find(saves, identifier, &persistence::SaveSummary::identifier);
    return found == saves.end() ? nullptr : &*found;
}

// iconPath is pure path arithmetic: it answers for a world that does not exist,
// and asking must not create anything. The UI needs the path before the file is
// there (it is about to write one), and the "does it exist" question belongs to
// SaveSummary::hasIcon.
void testIconPathIsPureArithmetic() {
    const auto root = makeUniqueRoot();
    const persistence::SaveRepository repository{root};

    const auto path = repository.iconPath("never-created");
    // Vanilla parity: the name and the location are the two things a JE import
    // or export depends on. Not `<world>/icons/thumb.png`, not `<world>.png`.
    REQUIRE(path.filename() == "icon.png");
    REQUIRE(path.parent_path() == root / "never-created");
    // No stat, no mkdir: the repository root itself was never created, and the
    // call must not have changed that. This is what separates "pure path
    // arithmetic" from an implementation that quietly ensures the directory.
    REQUIRE(!std::filesystem::exists(root));

    // And it is still just concatenation for a world that *does* exist — same
    // answer before and after the world appears on disk.
    std::filesystem::create_directories(root);
    const persistence::SaveRepository live{root};
    const auto identifier = createWorld(live, "Path World");
    REQUIRE(live.iconPath(identifier) == root / identifier / "icon.png");

    std::filesystem::remove_all(root);
}

// A fresh world has no thumbnail; writing one makes the *next* listing report
// it. The rescan matters: hasIcon is derived at listing time from the file, not
// cached in level.properties, so nothing had to be re-saved for it to flip.
void testHasIconFollowsTheFileOnDisk() {
    const auto root = makeUniqueRoot();
    persistence::SaveRepository repository{root};
    const auto identifier = createWorld(repository, "Icon World");

    {
        const auto saves = repository.list();
        const auto* summary = findSummary(saves, identifier);
        REQUIRE(summary != nullptr);
        // A world nobody has screenshotted must report false, or the UI would
        // draw a missing-file placeholder for every world ever created.
        REQUIRE(!summary->hasIcon);
    }
    // The version-aware listing carries the same SaveSummary and must agree; it
    // is a second code path over the same directory scan, and a fix applied to
    // only one of them is exactly the kind of drift this catches.
    {
        const auto worlds = repository.worldSummaries();
        const auto found = std::ranges::find(worlds, identifier,
            [](const persistence::WorldSummary& world) { return world.summary.identifier; });
        REQUIRE(found != worlds.end());
        REQUIRE(!found->summary.hasIcon);
    }

    const auto rgba = makeGradient(64U, 64U);
    REQUIRE(repository.writeIcon(identifier, rgba, 64U, 64U));
    // Written where iconPath promised, not merely somewhere: the getter and the
    // writer have to name the same file or the UI loads nothing.
    REQUIRE(std::filesystem::is_regular_file(repository.iconPath(identifier)));

    {
        const auto saves = repository.list();
        const auto* summary = findSummary(saves, identifier);
        REQUIRE(summary != nullptr);
        REQUIRE(summary->hasIcon);
    }
    {
        const auto worlds = repository.worldSummaries();
        const auto found = std::ranges::find(worlds, identifier,
            [](const persistence::WorldSummary& world) { return world.summary.identifier; });
        REQUIRE(found != worlds.end());
        REQUIRE(found->summary.hasIcon);
    }
    // Opening the world reports the same thing the list did, so a SaveGame's
    // summary can never contradict the entry the player clicked.
    REQUIRE(repository.load(identifier).summary.hasIcon);

    // Deleting just the file takes the flag back down — proof the listing reads
    // the filesystem every time rather than remembering a one-way answer.
    std::filesystem::remove(repository.iconPath(identifier));
    {
        const auto saves = repository.list();
        const auto* summary = findSummary(saves, identifier);
        REQUIRE(summary != nullptr);
        REQUIRE(!summary->hasIcon);
    }

    std::filesystem::remove_all(root);
}

// What writeIcon puts on disk has to be a real PNG that the game's own decoder
// reads back pixel-for-pixel. Anything less — right size, wrong rows; right
// rows, dropped alpha — would still leave a file the listing calls a thumbnail.
void testWrittenIconDecodesBackIdentically() {
    const auto root = makeUniqueRoot();
    persistence::SaveRepository repository{root};
    const auto identifier = createWorld(repository, "Round Trip");

    // Non-square on purpose: a width/height transposition inside the encoder
    // call cannot hide behind a square fixture.
    constexpr std::uint32_t kWidth = 24U;
    constexpr std::uint32_t kHeight = 13U;
    const auto rgba = makeGradient(kWidth, kHeight);
    REQUIRE(repository.writeIcon(identifier, rgba, kWidth, kHeight));

    const auto decoded = assets::ImageData::loadRgba(repository.iconPath(identifier));
    REQUIRE(decoded.width == static_cast<int>(kWidth));
    REQUIRE(decoded.height == static_cast<int>(kHeight));
    REQUIRE(decoded.rgba.size() == rgba.size());
    // Byte-for-byte, all four channels, in row-major top-down order — the same
    // layout every caller of ImageData already assumes.
    REQUIRE(std::ranges::equal(decoded.rgba, rgba));

    // Overwriting replaces the picture rather than appending or being ignored:
    // the icon is re-taken every time the player leaves the world.
    const auto replacement = makeGradient(8U, 8U);
    REQUIRE(repository.writeIcon(identifier, replacement, 8U, 8U));
    const auto reread = assets::ImageData::loadRgba(repository.iconPath(identifier));
    REQUIRE(reread.width == 8);
    REQUIRE(reread.height == 8);
    REQUIRE(std::ranges::equal(reread.rgba, replacement));
    // And no .tmp staging file is left lying about in the world folder.
    REQUIRE(!std::filesystem::exists(repository.iconPath(identifier).string() + ".tmp"));

    std::filesystem::remove_all(root);
}

// The refusals. Every one of these must leave the disk exactly as it was: a
// rejected write that still creates (or truncates) icon.png would make the
// listing advertise a thumbnail that is not there.
void testWriteIconRejectsBadInput() {
    const auto root = makeUniqueRoot();
    persistence::SaveRepository repository{root};
    const auto identifier = createWorld(repository, "Reject");
    const auto path = repository.iconPath(identifier);

    // The load-bearing one: 64*64*4 = 16384 bytes are promised, 16000 supplied.
    // The encoder would read 384 bytes past the end of the buffer and write a
    // perfectly valid-looking PNG. Nothing else in the system can see both the
    // length and the dimensions, so this refusal has to happen here.
    auto tooShort = makeGradient(64U, 64U);
    tooShort.resize(16000U);
    REQUIRE(!repository.writeIcon(identifier, tooShort, 64U, 64U));
    REQUIRE(!std::filesystem::exists(path));

    // Too long is equally wrong: it means the caller and the dimensions
    // disagree, and guessing which one is right is not this layer's business.
    auto tooLong = makeGradient(64U, 64U);
    tooLong.push_back(0U);
    REQUIRE(!repository.writeIcon(identifier, tooLong, 64U, 64U));
    REQUIRE(!std::filesystem::exists(path));

    // Off by exactly one pixel's worth of channels — the case a `>=` instead of
    // `!=` would wave through.
    auto shortByOnePixel = makeGradient(64U, 64U);
    shortByOnePixel.resize(shortByOnePixel.size() - 4U);
    REQUIRE(!repository.writeIcon(identifier, shortByOnePixel, 64U, 64U));
    REQUIRE(!std::filesystem::exists(path));

    // A zero dimension: the byte count would match an empty span, so the length
    // check alone cannot catch it, and the encoder has no picture to write.
    REQUIRE(!repository.writeIcon(identifier, {}, 0U, 0U));
    REQUIRE(!repository.writeIcon(identifier, {}, 64U, 0U));
    REQUIRE(!std::filesystem::exists(path));

    // No such world: writeIcon must not conjure the directory, or a typo would
    // leave a folder holding one orphaned PNG that no listing ever shows (it
    // has no level.properties) and nobody ever deletes.
    const auto orphan = makeGradient(4U, 4U);
    REQUIRE(!repository.writeIcon("no-such-world", orphan, 4U, 4U));
    REQUIRE(!std::filesystem::exists(root / "no-such-world"));

    // An identifier that is not a plain folder name would walk the write out of
    // the save root entirely.
    REQUIRE(!repository.writeIcon("../escape", orphan, 4U, 4U));
    REQUIRE(!std::filesystem::exists(root.parent_path() / "escape"));

    // After all of that, a well-formed call still succeeds — the guards reject
    // bad input, they do not wedge the writer.
    const auto good = makeGradient(64U, 64U);
    REQUIRE(repository.writeIcon(identifier, good, 64U, 64U));
    REQUIRE(std::filesystem::is_regular_file(path));

    std::filesystem::remove_all(root);
}

// Deleting a world takes its thumbnail with it. icon.png lives inside the world
// directory precisely so this needs no extra bookkeeping — the assertion exists
// to keep it that way if the file ever moves.
void testRemoveDeletesTheIcon() {
    const auto root = makeUniqueRoot();
    persistence::SaveRepository repository{root};
    const auto identifier = createWorld(repository, "Doomed");
    const auto rgba = makeGradient(16U, 16U);
    REQUIRE(repository.writeIcon(identifier, rgba, 16U, 16U));
    const auto path = repository.iconPath(identifier);
    REQUIRE(std::filesystem::exists(path));

    repository.remove(identifier);
    REQUIRE(!std::filesystem::exists(path));
    REQUIRE(!std::filesystem::exists(root / identifier));
    // And the world is gone from the listing, so no entry can be left pointing
    // at a thumbnail path that no longer resolves.
    REQUIRE(findSummary(repository.list(), identifier) == nullptr);

    std::filesystem::remove_all(root);
}

// Two worlds under one root keep their own thumbnails: the path is keyed by
// identifier, so writing one must not touch or shadow the other.
void testIconsArePerWorld() {
    const auto root = makeUniqueRoot();
    persistence::SaveRepository repository{root};
    const auto first = createWorld(repository, "Alpha");
    const auto second = createWorld(repository, "Beta");
    REQUIRE(first != second);

    const auto rgba = makeGradient(12U, 12U);
    REQUIRE(repository.writeIcon(first, rgba, 12U, 12U));

    const auto saves = repository.list();
    const auto* alpha = findSummary(saves, first);
    const auto* beta = findSummary(saves, second);
    REQUIRE(alpha != nullptr);
    REQUIRE(beta != nullptr);
    REQUIRE(alpha->hasIcon);
    // The one that was never written stays false — proof the probe looks inside
    // each world's own directory rather than anywhere shared.
    REQUIRE(!beta->hasIcon);
    REQUIRE(!std::filesystem::exists(repository.iconPath(second)));

    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    testIconPathIsPureArithmetic();
    testHasIconFollowsTheFileOnDisk();
    testWrittenIconDecodesBackIdentically();
    testWriteIconRejectsBadInput();
    testRemoveDeletesTheIcon();
    testIconsArePerWorld();
    return 0;
}
