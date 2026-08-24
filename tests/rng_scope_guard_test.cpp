// RNG-0 scope guard: the shared authoritative generator mc::rng is for
// *gameplay* RNG only. Decorative RNG — particles, audio pick/pan, the ambient
// music scheduler, the renderer's own jitter — is deliberately left on its own
// local LCG: it is non-authoritative, needs no vanilla parity, and must not
// widen the deterministic/save surface. This test pins that boundary by
// scanning the decorative sources and asserting none of them pulls in
// gameplay/Random.hpp or calls into mc::rng. Sabotage ③ (swapping a decorative
// LCG to mc::rng) trips exactly this.
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>

#ifndef MC_REBEDROCK_SRC_DIR
#error "MC_REBEDROCK_SRC_DIR must be defined to the repository src/ directory"
#endif

namespace {

std::string readFile(const std::string& path) {
    std::ifstream input{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

bool mentionsGameplayRng(const std::string& contents) {
    return contents.find("gameplay/Random.hpp") != std::string::npos ||
           contents.find("mc::rng::") != std::string::npos ||
           contents.find("rng::nextInt") != std::string::npos ||
           contents.find("rng::nextFloat") != std::string::npos;
}

}  // namespace

int main() {
    const std::string src = MC_REBEDROCK_SRC_DIR;
    // The decorative RNG owners. Each keeps its own local generator; none is
    // authoritative gameplay, so none may reach for mc::rng.
    const char* decorativeSources[] = {
        "render/ParticleSystem.cpp",
        "audio/AudioSystem.cpp",
        "audio/AmbientMusicScheduler.cpp",
        "render/vulkan/VulkanRenderer.cpp",
    };
    for (const char* relative : decorativeSources) {
        const std::string path = src + "/" + relative;
        const std::string contents = readFile(path);
        assert(!contents.empty());  // the guarded file must exist
        assert(!mentionsGameplayRng(contents));  // and must not use gameplay mc::rng
    }
    std::printf("rng_scope_guard: decorative RNG stays off mc::rng\n");
    return 0;
}
