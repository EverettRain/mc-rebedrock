// 启动编排：读选项、装配区块流送、扫描并叠好资源包栈、备好数据驱动玩法表的内置底座
// 最后构造 VulkanRenderer，并把线程交给它的主循环
// 本文件不含玩法，只做"按什么顺序把各子系统搭起来"这一件事

#include "core/Application.hpp"

#include "assets/PackManager.hpp"
#include "assets/PackMetadata.hpp"
#include "assets/ResourceProvider.hpp"
#include "core/VersionManifest.hpp"
#include "gameplay/DataPackStack.hpp"
#include "assets/ZipResourcePack.hpp"
#include "config/GameOptions.hpp"
#include "render/vulkan/VulkanRenderer.hpp"
#include "world/ChunkStreamer.hpp"
#include "world/WorldConstants.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <utility>

namespace mc {
namespace {

[[nodiscard]] int developmentLoadRadius(int configuredRadius) {
    constexpr int kMaximumRadius = 36;
    const char* value = std::getenv("MC_REBEDROCK_VIEW_DISTANCE");
    if (value == nullptr) {
        return configuredRadius;
    }

    const std::string_view text{value};
    int radius = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), radius);
    if (error != std::errc{} || end != text.data() + text.size() ||
        radius < 0 || radius > kMaximumRadius) {
        throw std::invalid_argument(
            "MC_REBEDROCK_VIEW_DISTANCE must be an integer between 0 and 36");
    }
    return radius;
}

} // namespace

Application::Application(
    std::filesystem::path resourceRoot,
    std::filesystem::path shaderRoot,
    std::filesystem::path configRoot,
    std::optional<render::TestSceneOptions> testScene)
    : resourceRoot_(std::move(resourceRoot)),
      shaderRoot_(std::move(shaderRoot)),
      configRoot_(std::move(configRoot)), testScene_(testScene) {}

int Application::run() {
    // 资源位置只有一个知情者：渲染器和纹理管理器向 provider 要文件，而不是各自拼路径
    // 这一个只服务 rebedrock 自持有的资源，同时充当下面资源包栈的最底层
    const assets::DirectoryResourceProvider bundled{resourceRoot_};

    std::cout << "MC Rebedrock Vulkan milestone\n";
    std::cout << "Chunk: " << world::kChunkWidth << 'x' << world::kChunkDepth << 'x'
              << world::kWorldHeight << "\n";
    std::cout << "Sections per chunk: " << world::kSectionCount << "\n";
    std::cout << "Sea level: " << world::kSeaLevel << "\n";
    // 内置根只有本项目自己的资源；vanilla 内容全部来自下面打印的资源包栈，缺包直接拒绝启动
    std::cout << "Resources: " << resourceRoot_ << "\n";

    const auto optionsPath = configRoot_ / "options.properties";
    config::GameOptions options = config::GameOptions::load(optionsPath);
    const int loadRadius = testScene_.has_value()
        ? 0 : developmentLoadRadius(options.viewDistance);
    // 卸载半径比加载半径多留几圈迟滞，见 world::kUnloadHysteresisChunks
    // 玩家骑在区块边界上时不会把同一圈反复加载卸载，每次卸载都是一次 region 写
    // 见 world::kUnloadHysteresisChunks
    const int unloadRadius = loadRadius + world::kUnloadHysteresisChunks;
    world::ChunkStreamer chunkStreamer{
        0x5EEDULL,
        loadRadius,
        unloadRadius,
    };
    const int diameter = loadRadius * 2 + 1;
    std::cout << "Streaming window: " << diameter << 'x' << diameter
              << " chunks (load radius " << loadRadius
              << ", unload radius " << unloadRadius << ")\n";

    // 玩家导入的标准资源包放在 <游戏根>/resourcepacks，与 bin/、config/、saves/ 平级
    // 游戏根即 config/ 的父目录
    // 首次运行就建好这个目录并打印绝对路径，方便直接把包丢进去
    // 启用的包逐文件覆盖内置资源，它没提供的仍回落到内置资源
    const auto packDirectory = configRoot_.parent_path() / "resourcepacks";
    std::error_code packError;
    std::filesystem::create_directories(packDirectory, packError);
    std::cout << "Resource packs: " << std::filesystem::weakly_canonical(packDirectory, packError)
              << "\n";
    // 一个包要么是根下带 pack.mcmeta 的目录，要么是同样结构的 .zip
    // 两种都收，按名字排序以保证启用顺序稳定
    struct PackEntry final {
        std::filesystem::path path;
        bool isZip = false;
    };
    std::vector<PackEntry> found;
    if (std::filesystem::is_directory(packDirectory, packError)) {
        for (const auto& entry : std::filesystem::directory_iterator(packDirectory, packError)) {
            if (entry.is_directory() && std::filesystem::exists(entry.path() / "pack.mcmeta")) {
                found.push_back({entry.path(), false});
            } else if (entry.is_regular_file() && entry.path().extension() == ".zip") {
                found.push_back({entry.path(), true});
            }
        }
    }
    std::sort(found.begin(), found.end(),
              [](const PackEntry& a, const PackEntry& b) { return a.path < b.path; });

    // 游戏里已没有任何地方向 provider 要文件系统路径，纹理声音标签群系数据一律读字节流
    // 因此 zip 包全程在内存里消费，这个缓存目录应当始终为空，被建出来就是回归
    // 路径仍传给 provider，因为 locate() 还是文档化的兜底
    // 旧版本确实往这里解压过，因此启动时清一次，免得几百 MB 的陈旧缓存永远躺着
    const auto packCacheRoot = configRoot_.parent_path() / ".packcache";
    if (std::filesystem::exists(packCacheRoot)) {
        std::error_code cleanup;
        std::filesystem::remove_all(packCacheRoot, cleanup);
        if (!cleanup) {
            std::cout << "Removed the legacy resource-pack extraction cache at "
                      << packCacheRoot.string() << "\n";
        }
    }
    // 用 deque：继续压入 provider（某个包的 overlay）不会让已交给 `enabled` 的指针失效
    std::deque<assets::StandardPackResourceProvider> directoryPacks;
    std::deque<assets::ZipResourcePackProvider> zipPacks;
    // 启用顺序，每项配一个 provider
    // 同一个包内低优先级在前，先主 assets，再是版本门控的 overlay
    std::vector<const assets::ResourceProvider*> enabled;
    // ReBedrock 对齐 26.1 的资源包格式号，门控到该格式的 overlay 才生效
    constexpr int kTargetPackFormat = 84;
    const auto readMetadata = [](const std::filesystem::path& mcmeta) -> assets::PackMetadata {
        std::ifstream input{mcmeta, std::ios::binary};
        if (!input) {
            return {};
        }
        std::ostringstream text;
        text << input.rdbuf();
        try {
            return assets::PackMetadata::parse(text.str());
        } catch (const std::exception&) {
            return {};
        }
    };
    for (const auto& entry : found) {
        if (entry.isZip) {
            zipPacks.emplace_back(entry.path, packCacheRoot / entry.path.stem());
            if (!zipPacks.back().valid()) {
                std::cerr << "Skipping unreadable resource pack: " << entry.path.filename().string()
                          << "\n";
                continue;
            }
            enabled.push_back(&zipPacks.back());
            std::cout << "Resource pack: " << entry.path.filename().string() << " (zip)\n";
        } else {
            directoryPacks.emplace_back(entry.path);
            enabled.push_back(&directoryPacks.back());
            std::cout << "Resource pack: " << entry.path.filename().string() << "\n";
            // pack.mcmeta 的 overlay 是带自己 assets/ 的版本门控子目录，声明越靠后优先级越高
            // 每个都成为一个叠在主 assets 之上的 provider
            const auto metadata = readMetadata(entry.path / "pack.mcmeta");
            // pack_format 与本 build 的 packVersion 只做比对记录，不硬失败
            // vanilla 对超范围的包同样是给个警告照样加载
            // 这里比的是资源半边的格式号，因为整个扫描针对的就是资源包目录
            // 逐存档的数据包走 packVersion.data 做同样的检查
            const auto compat = assets::PackManager::checkCompatibility(
                metadata, assets::PackStackKind::Resources, core::kVersion.packVersion);
            if (!compat.compatible) {
                std::cerr << "  警告：pack_format 不兼容 (declares " << compat.packFormatMin << "-"
                          << compat.packFormatMax << ", build targets " << compat.buildPackVersion
                          << ") — loading anyway\n";
            }
            for (const auto& overlay : metadata.overlays) {
                if (!overlay.appliesTo(kTargetPackFormat)) {
                    continue;
                }
                const auto overlayRoot = entry.path / overlay.directory;
                if (!std::filesystem::is_directory(overlayRoot, packError)) {
                    continue;
                }
                directoryPacks.emplace_back(overlayRoot);
                enabled.push_back(&directoryPacks.back());
                std::cout << "  overlay: " << overlay.directory << "\n";
            }
        }
    }

    // 资源包是必需品：本 build 不含任何 Mojang 资源，没有包就没有东西可画可放
    // 目录上面已经建好，这里直接告诉玩家往哪儿放并停下，而不是进到一个满屏缺失纹理的世界里
    if (enabled.empty()) {
        const auto where = std::filesystem::weakly_canonical(packDirectory, packError);
        std::cerr << "\n启动失败：缺少资源包 (Missing resource pack)\n"
                  << "请将一个标准资源包（目录或 .zip，含 pack.mcmeta 与 assets/）放入：\n  "
                  << where.string() << "\n"
                  << "然后重新启动。\n";
        return 1;
    }

    // 资源包与数据包是两条独立的线，这里只负责资源半边
    // <游戏根>/resourcepacks 是启动期的、全局的、纯客户端的
    // 数据半边是逐存档的，由 GameRuntime::loadWorld 从各存档自己的 <save>/datapacks/ 重建
    // 见 gameplay::PerSaveDataStack，它不吃这个全局目录
    // 启动期对数据半边唯一要做的就是备好下面那层内置默认值
    // 于是在任何世界加载之前就读 blockTags()、recipeTable() 的调用方拿到的是干净的内置值
    // 直接构造这些表的进程内测试同理，都不会读到上一次运行残留在静态变量里的半截状态
    assets::PackManager packManager;
    for (std::size_t index = 0; index < enabled.size(); ++index) {
        const std::string id = "pack" + std::to_string(index);
        packManager.registerPack(id, *enabled[index], assets::PackMetadata{},
                                 /*hasDataHalf=*/false, /*hasResourceHalf=*/true);
        packManager.enable(assets::PackStackKind::Resources, id);
    }

    // 数据驱动玩法表的内置底座，不挂任何包栈
    // 这就是一个没有 <save>/datapacks/ 的全新存档重建之后的状态
    gameplay::PerSaveDataStack::rebuildBuiltinOnly(bundled);

    // 资源栈：只服务渲染，只含 assets/ 半边
    // 专用服务器不会走到这里，这是结构性保证
    // 本行以上没有任何东西需要它，而 dedicated_server_main.cpp 根本不链接 render/vulkan
    const assets::LayeredResourceProvider resourceStack =
        packManager.buildProvider(assets::PackStackKind::Resources, bundled);

    render::VulkanRenderer renderer{
        shaderRoot_,
        resourceStack,
        chunkStreamer,
        options,
        optionsPath,
        configRoot_.parent_path() / "saves",
        testScene_};
    renderer.run();
    chunkStreamer.stop();

    return 0;
}

} // namespace mc
