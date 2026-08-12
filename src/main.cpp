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

// A streambuf that writes every character to a log file and the original
// terminal stream alike. Installed over std::cout / std::cerr so a fatal error
// or a crash that closes the console still leaves a record on disk.
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

// Tees both standard streams into `path` (appended per run, with a separator so
// sessions are easy to tell apart). The streams keep writing to the terminal
// too, so development output is unchanged. Every line is flushed to the file,
// so even a hard crash does not lose the tail of the log. `active()` is false
// when the file could not be opened, in which case the app simply keeps its
// terminal-only behaviour. Restores the original streams on destruction so the
// global iostream objects never flush through freed buffers during teardown.
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
            // The game ships no Mojang assets, so a staged runtime is recognised
            // by ReBedrock's own resources — the compiled shaders and the staging
            // marker — never by a bundled vanilla tree. Minecraft content comes
            // from a resource pack the player supplies at runtime.
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
    // Record every line of output in a file as well as the terminal, so a
    // Vulkan initialisation failure that closes the console still leaves a
    // diagnosable record in <config>/rebedrock.log. Lives for the whole run,
    // which keeps the fatal error below inside the file too.
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
