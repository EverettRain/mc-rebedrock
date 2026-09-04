#pragma once

// RN-15d: `--pack <目录或 .zip>`。
//
// 这个 build 不 bundle 任何 Mojang 资源，vanilla 内容一律由玩家自备的资源包在运行
// 时提供（版权铁律）。平时这些包放在 <游戏根>/resourcepacks；但预览导出是要被自动化
// 一条命令跑起来的，让它先把包拷进游戏目录既别扭又容易拷错一版，所以这里额外接受在
// 命令行上直接点名的包。
//
// 命令行点名的包排在扫描到的包之后，也就是优先级最高：一条命令说了用哪个包，就该是
// 那个包，而不是被目录里恰好同名的另一份盖掉。
//
// 参数写错直接抛，与 `--test-scene` 同一条纪律：自动化最坏的结局不是报错停下，而是
// 悄悄用了别的资源渲完八张图还报成功。

#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mc {

[[nodiscard]] inline std::vector<std::string> parsePackArguments(
    std::span<const std::string_view> arguments) {
    std::vector<std::string> packs;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] != "--pack") {
            continue;
        }
        if (++index >= arguments.size()) {
            throw std::invalid_argument("--pack requires a directory or .zip path");
        }
        if (arguments[index].empty()) {
            throw std::invalid_argument("--pack was given an empty path");
        }
        // 一个空串会被 std::filesystem 当成"当前目录"，于是 `--pack ""` 会静静地把
        // 工作目录当资源包挂上去，这正是上面那条纪律要拦的东西。
        packs.emplace_back(arguments[index]);
    }
    return packs;
}

} // namespace mc
