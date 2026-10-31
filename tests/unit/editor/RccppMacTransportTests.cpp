#include "Compiler.h"
#include "GuiLiveBuildRevision.h"
#include "GuiRuntimeBridge.h"
#include "Horo/Editor/GuiLiveReload.h"
#include "ICompilerLogger.h"
#include "IFileChangeNotifier.h"
#include "RuntimeObjectSystem.h"
#include "SimpleFileWatcher/FileWatcher.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

// The candidate imports both this function and its typed host address getter.
int HoroLiveTransportHostValue() {
    static int value = 40;
    return ++value;
}

namespace {
    struct Directory {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            std::filesystem::path{"horo mac live ü " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())};

        Directory() {
            std::filesystem::create_directories(path);
        }

        ~Directory() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    };

    void Write(const std::filesystem::path &path, const std::string &text) {
        std::ofstream output{path};
        output << text;
        REQUIRE(output.good());
    }

    struct Logger : ICompilerLogger {
        std::vector<std::string> lines;

        void Capture(const char *format, va_list arguments) {
            char text[4096]{};
            std::vsnprintf(text, sizeof(text), format, arguments);
            lines.emplace_back(text);
        }

        void LogError(const char *format, ...) override {
            va_list arguments;
            va_start(arguments, format);
            Capture(format, arguments);
            va_end(arguments);
        }

        void LogWarning(const char *format, ...) override {
            va_list arguments;
            va_start(arguments, format);
            Capture(format, arguments);
            va_end(arguments);
        }

        void LogInfo(const char *format, ...) override {
            va_list arguments;
            va_start(arguments, format);
            Capture(format, arguments);
            va_end(arguments);
        }
    };

    struct ExceptionPorts {
        exception_mask_t masks[EXC_TYPES_COUNT]{};
        mach_port_t ports[EXC_TYPES_COUNT]{};
        exception_behavior_t behaviors[EXC_TYPES_COUNT]{};
        thread_state_flavor_t flavors[EXC_TYPES_COUNT]{};
        mach_msg_type_number_t count{EXC_TYPES_COUNT};

        ExceptionPorts() {
            REQUIRE(task_get_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION, masks, &count, ports,
                                             behaviors, flavors) == KERN_SUCCESS);
        }

        ~ExceptionPorts() {
            for (mach_msg_type_number_t index = 0; index < count; ++index)
                if (ports[index] != MACH_PORT_NULL)
                    mach_port_deallocate(mach_task_self(), ports[index]);
        }

        void CheckUnchanged(const ExceptionPorts &other) const {
            REQUIRE(count == other.count);
            for (mach_msg_type_number_t index = 0; index < count; ++index) {
                CHECK(masks[index] == other.masks[index]);
                CHECK(ports[index] == other.ports[index]);
                CHECK(behaviors[index] == other.behaviors[index]);
                CHECK(flavors[index] == other.flavors[index]);
            }
        }
    };

    std::string Executable() {
        std::uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::vector<char> path(size);
        REQUIRE(_NSGetExecutablePath(path.data(), &size) == 0);
        return std::filesystem::canonical(path.data()).string();
    }

    void WaitForCompile(Compiler &compiler) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        while (!compiler.GetIsComplete() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        REQUIRE(compiler.GetIsComplete());
    }

    struct Module {
        void *handle;

        explicit Module(const std::filesystem::path &path) : handle(dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL)) {
            const auto *error = dlerror();
            INFO((error ? error : "module loaded"));
            REQUIRE(handle != nullptr);
        }

        ~Module() {
            dlclose(handle);
        }

        int Value() {
            const auto function = reinterpret_cast<int (*)()>(dlsym(handle, "LiveValue"));
            REQUIRE(function != nullptr);
            return function();
        }
    };

    struct Listener : FW::FileWatchListener {
        std::vector<std::string> changed;

        void handleFileAction(FW::WatchID, const FW::String &, const FW::String &filename, FW::Action) override {
            changed.push_back(filename.m_string);
        }

        void Wait(FW::FileWatcher &watcher, const std::filesystem::path &path) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
            while (std::chrono::steady_clock::now() < deadline) {
                watcher.update();
                if (std::find(changed.begin(), changed.end(), path.string()) != changed.end()) {
                    changed.clear();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
            FAIL("No native file-change event for " << path.string());
        }
    };
}  // namespace

TEST_CASE("GUI build revisions preserve fractional timestamps and native clock range", "[editor][live-ui][macos]") {
    using Horo::Editor::Development::GuiLiveBuildRevision;
    using Clock = std::filesystem::file_time_type;
    const Clock epoch{};
    const auto tick = std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds{100});
    const auto windowsEpochSpan = std::chrono::duration_cast<Clock::duration>(std::chrono::seconds{13'000'000'000LL});
    const auto large = epoch + windowsEpochSpan;
    CHECK(GuiLiveBuildRevision(epoch) == std::filesystem::path{"0_0"});
    CHECK(GuiLiveBuildRevision(epoch + tick) != GuiLiveBuildRevision(epoch));
    CHECK(GuiLiveBuildRevision(epoch - tick) != GuiLiveBuildRevision(epoch + tick));
    CHECK(GuiLiveBuildRevision(large) == std::filesystem::path{"13000000000_0"});
    CHECK(GuiLiveBuildRevision(large + tick) != GuiLiveBuildRevision(large));
    CHECK(GuiLiveBuildRevision(large).parent_path().empty());
}

TEST_CASE("macOS compiler imports the original host and survives verbose errors and recovery", "[editor][live-ui][macos]") {
    Directory directory;
    Logger logger;
    Compiler compiler;
    compiler.Initialise(&logger);
    CompilerOptions options{};
    options.optimizationLevel = RCCPPOPTIMIZATIONLEVEL_DEBUG;
    options.intermediatePath = (directory.path / "objects").string();
    options.compilerLocation = HORO_GUI_LIVE_COMPILER;
    options.includeDirList.emplace_back(HORO_TEST_LIVE_HEADER_DIRECTORY);
    // This exceeds macOS ARG_MAX without producing an oversized clang argument.
    // Real registered GUI batches exceed that limit when transported with sh -c.
    options.compileOptions = "-std=c++20 -DHORO_GUI_RUNTIME_COMPILATION=1" + std::string(300'000, ' ');
    options.linkOptions = "-bundle_loader \"" + Executable() + "\"";
    const auto source = directory.path / "drawing.cpp";
    const auto first = directory.path / "first.bundle";
    const std::string drawing = R"(
#include "Horo/Editor/GuiLiveReload.h"
int HoroLiveTransportHostValue();
extern "C" __attribute__((visibility("default"))) int LiveValue() {
    return Horo::Editor::GuiLive::HostSymbol<&HoroLiveTransportHostValue>()();
}
)";
    REQUIRE(Horo::Editor::GuiLive::HostSymbol<&HoroLiveTransportHostValue>()() == 41);
    Write(source, drawing);
    compiler.RunCompile({source.string()}, options, {}, first.string());
    WaitForCompile(compiler);
    Module installed{first};
    REQUIRE(installed.Value() == 42);

    std::string errors;
    for (int index = 0; index < 1200; ++index)
        errors += "#warning 100% diagnostic output that must not fill a pipe and block compilation\n";
    Write(source, errors + "#error deliberate invalid candidate\n");
    const auto failed = directory.path / "failed.bundle";
    compiler.RunCompile({source.string()}, options, {}, failed.string());
    WaitForCompile(compiler);
    REQUIRE_FALSE(std::filesystem::exists(failed));
    REQUIRE(installed.Value() == 43);
    REQUIRE_FALSE(logger.lines.empty());
    REQUIRE(std::any_of(logger.lines.begin(), logger.lines.end(), [](const std::string &line) {
        return line.find("100%") != std::string::npos;
    }));

    Write(source, drawing);
    const auto recovered = directory.path / "recovered.bundle";
    compiler.RunCompile({source.string()}, options, {}, recovered.string());
    WaitForCompile(compiler);
    Module replacement{recovered};
    REQUIRE(replacement.Value() == 44);
}

TEST_CASE("macOS compiler teardown stops its compiler descendants", "[editor][live-ui][macos]") {
    Directory directory;
    Logger logger;
    const auto marker = directory.path / "should never be written";
    const auto started = directory.path / "started";
    const auto tool = directory.path / "slow compiler";
    Write(tool, "#!/bin/sh\ntouch '" + started.string() + "'\nsleep 2\ntouch '" + marker.string() + "'\n");
    std::filesystem::permissions(tool, std::filesystem::perms::owner_all);
    {
        Compiler compiler;
        compiler.Initialise(&logger);
        CompilerOptions options{};
        options.optimizationLevel = RCCPPOPTIMIZATIONLEVEL_DEBUG;
        options.intermediatePath = (directory.path / "objects").string();
        options.compilerLocation = tool.string();
        compiler.RunCompile({(directory.path / "unused.cpp").string()}, options, {}, (directory.path / "unused.bundle").string());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (!std::filesystem::exists(started) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        REQUIRE(std::filesystem::exists(started));
        REQUIRE_FALSE(compiler.GetIsComplete());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{2200});
    REQUIRE_FALSE(std::filesystem::exists(marker));
}

TEST_CASE("macOS RCC++ publishes typed modules and retains the installed module on failure", "[editor][live-ui][macos]") {
    Directory directory;
    Logger logger;
    SystemTable system;
    const ExceptionPorts debuggerPorts;
    RuntimeObjectSystem runtime;
    debuggerPorts.CheckUnchanged(ExceptionPorts{});
    REQUIRE(runtime.Initialise(&logger, &system));
    runtime.GetFileChangeNotifier()->SetMonitoringActive(false);
    runtime.SetIntermediateDir((directory.path / "objects").string().c_str());
    runtime.SetCompilerLocation(HORO_GUI_LIVE_COMPILER);
    runtime.AddIncludeDir(HORO_TEST_LIVE_HEADER_DIRECTORY);
    runtime.AddIncludeDir(HORO_TEST_LIVE_BRIDGE_DIRECTORY);
    runtime.SetAdditionalCompileOptions("-std=c++20 -DHORO_GUI_RUNTIME_COMPILATION=1");
    runtime.SetAdditionalLinkOptions(("-bundle_loader \"" + Executable() + "\"").c_str());
    const auto source = directory.path / "RegisteredProbe.cpp";
    const std::string drawing = R"(
#include "GuiRuntimeBridge.h"
#include "ISimpleSerializer.h"
#include "ObjectInterfacePerModule.h"
class HoroMacProbe : public HoroGuiModule {
public:
    void Init(bool) override { PerModuleInterface::g_pSystemTable->modules[0] = this; }
    void Serialize(ISimpleSerializer *) override {}
    Horo::Editor::GuiLive::Binding Find(Horo::Editor::GuiLive::Id route, Horo::Editor::GuiLive::Id signature) const noexcept override {
        using namespace Horo::Editor;
        if (route == GuiLive::Id{"Probe"})
            return GuiLive::Adapter<int(), [] { return 7; }>::Bind(signature);
        return {};
    }
};
REGISTERSINGLETON(HoroMacProbe, true);
)";
    Write(source, drawing);
    runtime.AddToRuntimeFileList(source.string().c_str());
    const auto compile = [&] {
        runtime.CompileAllInProject(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        while (!runtime.GetIsCompiledComplete() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        REQUIRE(runtime.GetIsCompiledComplete());
    };
    const auto value = [&] {
        REQUIRE(system.modules[0] != nullptr);
        const auto binding =
            system.modules[0]->Find(Horo::Editor::GuiLive::Id{"Probe"}, Horo::Editor::GuiLive::Detail::SignatureId<int()>());
        const Horo::Editor::GuiLive::Function<int()> function{binding};
        REQUIRE(static_cast<bool>(function));
        return function();
    };
    compile();
    for (const auto &line : logger.lines)
        INFO(line);
    REQUIRE(runtime.LoadCompiledModule());
    REQUIRE(value() == 7);
    Write(source, "#error deliberate invalid module\n");
    compile();
    REQUIRE_FALSE(runtime.LoadCompiledModule());
    REQUIRE(value() == 7);
    auto changed = drawing;
    changed.replace(changed.find("return 7"), 8, "return 9");
    Write(source, changed);
    compile();
    REQUIRE(runtime.LoadCompiledModule());
    REQUIRE(value() == 9);
    debuggerPorts.CheckUnchanged(ExceptionPorts{});
}

TEST_CASE("macOS watcher observes in-place saves, atomic replacement and recreated files", "[editor][live-ui][macos]") {
    Directory directory;
    const auto source = directory.path / "drawing ü.cpp";
    Write(source, "before");
    Listener listener;
    FW::FileWatcher watcher;
    const auto watch = watcher.addWatch(directory.path.string(), &listener);
    Write(source, "edited");
    listener.Wait(watcher, source);
    const auto replacement = directory.path / "temporary";
    Write(replacement, "atomic");
    std::filesystem::rename(replacement, source);
    listener.Wait(watcher, source);
    Write(source, "again!");
    listener.Wait(watcher, source);
    std::filesystem::remove(source);
    listener.Wait(watcher, source);
    Write(source, "return");
    listener.Wait(watcher, source);
    Write(source, "final!");
    listener.Wait(watcher, source);
    watcher.removeWatch(watch);
}

TEST_CASE("macOS watcher ignores access metadata changes while retaining content saves", "[editor][live-ui][macos]") {
    Directory directory;
    const auto source = directory.path / "metadata.cpp";
    Write(source, "before");
    Listener listener;
    FW::FileWatcher watcher;
    const auto watch = watcher.addWatch(directory.path.string(), &listener);
    const timespec times[2]{{1, 0}, {0, UTIME_OMIT}};
    REQUIRE(utimensat(AT_FDCWD, source.c_str(), times, 0) == 0);
    for (int poll = 0; poll < 4; ++poll) {
        watcher.update();
        CHECK(listener.changed.empty());
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    Write(source, "after");
    listener.Wait(watcher, source);
    watcher.removeWatch(watch);
}
