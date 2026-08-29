// 进程入口
// 只做三件事：定位三个根目录、接管标准流写日志、把控制权交给 Application
// 资源、着色器、配置三个根目录在开发树里取编译期默认值
// 在部署包里则改用 bin/ 旁边的 resources 与 config
// 除此之外不含任何游戏逻辑

#include "core/Application.hpp"
#include "render/TestScene.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <streambuf>
#include <utility>
#include <vector>

#ifndef MC_REBEDROCK_RESOURCE_ROOT
#define MC_REBEDROCK_RESOURCE_ROOT "resources"
#endif

#ifndef MC_REBEDROCK_SHADER_ROOT
#define MC_REBEDROCK_SHADER_ROOT "resources/shaders"
#endif
#ifndef MC_REBEDROCK_CONFIG_ROOT
#define MC_REBEDROCK_CONFIG_ROOT "config"
#endif

namespace {

// 把每个字符同时写进日志文件和原终端流的 streambuf
// 装在 std::cout / std::cerr 上，于是致命错误或崩溃关掉控制台之后，磁盘上仍留有现场
class TeeBuffer final : public std::streambuf {
  public:
    TeeBuffer(std::streambuf* terminal, std::ofstream& file)
        : terminal_(terminal), file_(file) {}

  protected:
    int_type overflow(int_type character) override {
        if (traits_type::eq_int_type(character, traits_type::eof())) {
            return traits_type::not_eof(character);
        }
        const char byte = traits_type::to_char_type(character);
        file_.write(&byte, 1);
        terminal_->sputc(byte);
        return traits_type::not_eof(character);
    }

    std::streamsize xsputn(const char* data, std::streamsize count) override {
        file_.write(data, count);
        return terminal_->sputn(data, count);
    }

    int sync() override {
        file_.flush();
        return 0;
    }

  private:
    std::streambuf* terminal_;
    std::ofstream& file_;
};

// 把两个标准流同时接到 `path`（按次追加，带分隔行便于区分场次），终端输出保持不变
// 每行立即刷盘，硬崩溃也不会丢日志尾部
// 文件打不开时 active() 为 false，程序退回纯终端行为
// 析构时还原原始 streambuf，避免全局 iostream 在销毁期刷进已释放的缓冲区
class FileLog final {
  public:
    explicit FileLog(std::filesystem::path path)
        : coutTerminal_(std::cout.rdbuf()), cerrTerminal_(std::cerr.rdbuf()) {
        file_.open(path, std::ios::app);
        if (!file_.is_open()) {
            return;
        }
        file_ << "\n===== MC Rebedrock run started =====\n";
        file_.flush();
        file_.setf(std::ios::unitbuf);
        coutTee_ = std::make_unique<TeeBuffer>(coutTerminal_, file_);
        cerrTee_ = std::make_unique<TeeBuffer>(cerrTerminal_, file_);
        std::cout.rdbuf(coutTee_.get());
        std::cerr.rdbuf(cerrTee_.get());
        std::cout.setf(std::ios::unitbuf);
        active_ = true;
    }

    ~FileLog() {
        if (!active_) {
            return;
        }
        std::cout.rdbuf(coutTerminal_);
        std::cerr.rdbuf(cerrTerminal_);
    }

    FileLog(const FileLog&) = delete;
    FileLog& operator=(const FileLog&) = delete;

    [[nodiscard]] bool active() const { return active_; }

  private:
    std::streambuf* coutTerminal_;
    std::streambuf* cerrTerminal_;
    std::ofstream file_;
    std::unique_ptr<TeeBuffer> coutTee_;
    std::unique_ptr<TeeBuffer> cerrTee_;
    bool active_ = false;
};

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path resourceRoot{MC_REBEDROCK_RESOURCE_ROOT};
    std::filesystem::path shaderRoot{MC_REBEDROCK_SHADER_ROOT};
    std::filesystem::path configRoot{MC_REBEDROCK_CONFIG_ROOT};
    if (argc > 0 && argv[0] != nullptr) {
        std::error_code error;
        const auto executable = std::filesystem::weakly_canonical(
            std::filesystem::absolute(argv[0]), error);
        if (!error && executable.parent_path().filename() == "bin") {
            const auto gameRoot = executable.parent_path().parent_path();
            const auto stagedResources = gameRoot / "resources";
            const auto stagedShaders = stagedResources / "shaders";
            // 发行包不含任何 Mojang 资源，所以不能靠随包的 vanilla 资源树来判定
            // 认出"已部署的运行时"只看 ReBedrock 自己的东西：编译好的着色器加部署标记文件
            // Minecraft 内容一律由玩家自备的资源包在运行时提供
            std::error_code shaderError;
            std::error_code markerError;
            if (std::filesystem::is_directory(stagedShaders, shaderError) &&
                std::filesystem::exists(stagedResources / ".runtime-assets-ready",
                                        markerError)) {
                resourceRoot = stagedResources;
                shaderRoot = stagedShaders;
                configRoot = gameRoot / "config";
            }
        }
    }
    // 终端输出同时落盘到 <config>/rebedrock.log
    // Vulkan 初始化失败会关掉控制台，那份日志是唯一能诊断的现场
    // 生命周期覆盖整个 run()，所以下面 catch 到的致命错误也会进文件
    FileLog fileLog{configRoot / "rebedrock.log"};
    try {
        std::vector<std::string_view> arguments;
        for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
        const auto testScene = mc::render::parseTestSceneArguments(arguments);
        mc::Application application{
            std::move(resourceRoot),
            std::move(shaderRoot),
            std::move(configRoot),
            testScene};
        return application.run();
    } catch (const std::exception& exception) {
        std::cerr << "Fatal error: " << exception.what() << '\n';
        return 1;
    }
}
