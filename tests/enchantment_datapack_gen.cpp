// DDC-3: the built-in enchantment datapack generator.
//
// Emits one JE-schema JSON file per built-in enchantment (kEnchantmentTable),
// under <out>/data/rebedrock/enchantment/<name>.json, reproducing ENCH-0's exact
// numbers. This is how resources/data/rebedrock/enchantment/*.json — the internal
// datapack DDC-3 ships — is produced: the constexpr table is the single source of
// truth, and this tool is the one-way projection into datapack files, so the
// files can never silently drift from the table (the golden test re-derives the
// same JSON in memory and byte-compares).
//
// Built only under -DMC_REBEDROCK_BUILD_TOOLS=ON; it is not part of the test
// suite (it writes files). Run once by hand after changing the table:
//   mc_rebedrock_enchantment_datapack_gen <repo-root>
//
// The generated files are what a player would drop into <save>/datapacks, and
// what the golden test loads through StandardPackResourceProvider to prove the
// migration round-trips with zero drift.

#include "core/Json.hpp"
#include "gameplay/Enchantment.hpp"
#include "gameplay/EnchantmentContentMigration.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <repo-root>\n";
        return 2;
    }
    const std::filesystem::path root{argv[1]};
    const std::filesystem::path outDir =
        root / "resources" / "data" / "rebedrock" / "enchantment";
    std::error_code error;
    std::filesystem::create_directories(outDir, error);
    if (error) {
        std::cerr << "failed to create " << outDir << ": " << error.message() << '\n';
        return 1;
    }

    int written = 0;
    for (const mc::gameplay::EnchantmentDefinition& def : mc::gameplay::kEnchantmentTable) {
        const mc::core::Json json = mc::gameplay::enchantmentContentJson(def);
        const std::filesystem::path file =
            outDir / (std::string{def.vanillaName} + ".json");
        std::ofstream out{file, std::ios::binary | std::ios::trunc};
        if (!out) {
            std::cerr << "failed to open " << file << '\n';
            return 1;
        }
        out << json.dump() << '\n';
        ++written;
    }
    std::cout << "wrote " << written << " enchantment files to " << outDir << '\n';
    return 0;
}
