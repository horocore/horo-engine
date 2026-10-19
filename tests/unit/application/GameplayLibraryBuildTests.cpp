#include "../packages/GameplaySourceLibraryTestSupport.h"
#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Platform/ExternalProcess.h"

#include <atomic>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Tests::GameplayLibraries;

    /** @brief Counts child admission; inert mode fails configure after real source preparation without pretending to compile. */
    class LibraryProcessRunner final : public IExternalProcessRunner {
    public:
        bool native{};
        bool configureSuccess{};
        std::atomic<std::size_t> calls{};
        NativeExternalProcessRunner nativeRunner;

        Result<ExternalProcessResult> Run(const ExternalProcessRequest &request, const CancellationToken &cancellation) override {
            ++calls;
            if (!native)
                return Result<ExternalProcessResult>::Success({ProcessTerminationReason::Exited, configureSuccess ? 0 : 1});
            return nativeRunner.Run(request, cancellation);
        }
    };

    struct BuildFixture final {
        explicit BuildFixture(const std::string_view range = "1.0.0") : package(0, range) {}

        BuildFixture(const BuildFixture &) = delete;
        BuildFixture &operator=(const BuildFixture &) = delete;
        Fixture package;
        LibraryProcessRunner runner;
        JobSystem jobs{{1, 8}};
        BuildOutputStore output{1024};
        GameplayBuildService service{runner, jobs, package.files, &output};

        ~BuildFixture() {
            service.Shutdown();
            jobs.Shutdown(ShutdownPolicy::Cancel);
        }

        GameplayBuildRequest Request() const {
            return {.projectRoot = package.root,
                    .environment = {.gameplaySdkPackage = HORO_GAMEPLAY_SDK_PACKAGE_DIR,
                                    .cxxCompiler = std::filesystem::path{HORO_GAMEPLAY_CXX_COMPILER},
                                    .generator = HORO_TEST_CMAKE_GENERATOR},
                    .sourceLibraries = std::make_shared<const GameplaySourceLibrarySet>(std::vector{package.Select()})};
        }

        GameplayBuildSnapshot Await(const GameplayBuildSessionId id) const {
            using enum GameplayBuildState;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes{2};
            while (std::chrono::steady_clock::now() < deadline) {
                const auto snapshot = service.Query(id);
                REQUIRE(snapshot.has_value());
                if (snapshot->state == Succeeded || snapshot->state == Failed || snapshot->state == Cancelled ||
                    snapshot->state == TimedOut)
                    return *snapshot;
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
            FAIL("Package library build did not reach a terminal boundary.");
        }
    };

    void WriteProject(const Fixture &package) {
        Write(package.root / "CMakeLists.txt", R"(cmake_minimum_required(VERSION 3.25)
project(SourceLibraryConsumer LANGUAGES CXX)
find_package(HoroEngineGameplay CONFIG REQUIRED)
horo_add_gameplay_module(HoroGameGameplay MODULE_ID game.source_library SOURCES source/gameplay/GameModule.cpp)
)");
        Write(package.root / "source/gameplay/GameModule.cpp", R"(#include <Horo/Gameplay/GameModule.h>
#include "Answer.h"
namespace { class Module final : public Horo::Gameplay::IGameModule {
public:
    Horo::Result<void> Register(Horo::Gameplay::GameRegistrationContext&) override { return Horo::Result<void>::Success(); }
    Horo::Result<void> Start(Horo::Gameplay::GameRuntimeContext&) override {
        return Example::Answer() == 42 ? Horo::Result<void>::Success() : Horo::Result<void>::Failure({});
    }
    void Stop(Horo::Gameplay::GameRuntimeContext&) noexcept override {}
}; }
extern "C" HORO_GAME_EXPORT Horo::Gameplay::IGameModule* CreateGameModule() noexcept { return new Module{}; }
extern "C" HORO_GAME_EXPORT void DestroyGameModule(Horo::Gameplay::IGameModule* module) noexcept { delete module; }
)");
    }

    /** @brief Checks a later read against an independently captured publication across failed admission. */
    void CheckPublishedState(const std::filesystem::path &path, const std::string_view previous) {
        CHECK(Read(path) == previous);
    }

    /** @brief Executes the imported function through actual module startup, then drains the native owner before fixture removal. */
    void RequireLibraryModuleStarts(const Fixture &fixture) {
        const auto metadata = nlohmann::json::parse(Read(fixture.root / ".horo/local/gameplay_module.json"));
        Gameplay::GameModuleHost host;
        auto loaded = host.Load(metadata.at("artifactPath").get<std::string>(),
                                {.moduleId = "game.source_library",
                                 .buildFingerprint = Gameplay::CurrentGameplayBuildFingerprint(),
                                 .descriptorRevision = metadata.at("descriptorRevision").get<std::uint64_t>()});
        REQUIRE(loaded.HasValue());
        REQUIRE_FALSE(loaded.Value().diagnostic.has_value());
        REQUIRE(loaded.Value().owner);
        CHECK(loaded.Value().owner->TryRetire().HasValue());
    }

    /** @brief Releases a deliberately occupied worker even when a test assertion exits early. */
    struct BlockingJobOwner final {
        explicit BlockingJobOwner(JobHandle owned) : handle(std::move(owned)) {}

        BlockingJobOwner(const BlockingJobOwner &) = delete;
        BlockingJobOwner &operator=(const BlockingJobOwner &) = delete;
        BlockingJobOwner(BlockingJobOwner &&) = delete;
        BlockingJobOwner &operator=(BlockingJobOwner &&) = delete;
        JobHandle handle;

        ~BlockingJobOwner() {
            static_cast<void>(handle.RequestCancel());
            static_cast<void>(handle.Wait());
        }
    };
}  // namespace

TEST_CASE("Gameplay package builds reject stale intent lock and competing source selections before child admission",
          "[unit][gameplay][build][packages]") {
    BuildFixture fixture{"^1.0.0"};
    auto request = fixture.Request();
    SECTION("missing lock") {
        std::filesystem::remove(fixture.package.root / ".horo/packages.lock");
    }
    SECTION("malformed intent") {
        Write(fixture.package.root / ".horo/packages.json", "{}");
    }
    SECTION("changed intent") {
        auto intent = nlohmann::json::parse(fixture.package.intent);
        intent["dependencies"][fixture.package.id.Value()]["version"] = "2.0.0";
        Write(fixture.package.root / ".horo/packages.json", intent.dump());
    }
    SECTION("different exact artifact") {
        auto lock = nlohmann::json::parse(fixture.package.lock);
        const std::string replacement{"different archive"};
        lock["packages"][0]["artifactSha256"] = FormatSha256(ComputeSha256(std::as_bytes(std::span{replacement})));
        REQUIRE(ValidatedPackageLockfileV1::Parse(lock.dump()).HasValue());
        Write(fixture.package.root / ".horo/packages.lock", lock.dump());
    }
    SECTION("different compatible resolved version") {
        auto lock = nlohmann::json::parse(fixture.package.lock);
        lock["packages"][0]["version"] = "1.1.0";
        lock["roots"][0]["version"] = "1.1.0";
        REQUIRE(ValidatedPackageLockfileV1::Parse(lock.dump()).HasValue());
        Write(fixture.package.root / ".horo/packages.lock", lock.dump());
    }
    SECTION("competing selection") {
        request.sourceLibraries =
            std::make_shared<const GameplaySourceLibrarySet>(std::vector{fixture.package.Select(), fixture.package.Select()});
    }
    SECTION("library count bound") {
        request.sourceLibraries = std::make_shared<const GameplaySourceLibrarySet>(std::vector(65, fixture.package.Select()));
    }
    REQUIRE(fixture.service.Start(request).HasError());
    CHECK(fixture.runner.calls == 0);
    CHECK_FALSE(std::filesystem::exists(fixture.package.root / ".horo/local/gameplay_module.json"));
}

TEST_CASE("Gameplay source preparation retains exact bytes and rejects cache corruption before configure",
          "[unit][gameplay][build][packages]") {
    BuildFixture fixture;
    const auto request = fixture.Request();
    const auto started = fixture.service.Start(request);
    REQUIRE(started.HasValue());
    CHECK(fixture.Await(started.Value()).state == GameplayBuildState::Failed);  // Inert configure deliberately exits 1.
    REQUIRE(fixture.runner.calls == 1);
    const auto prepared =
        nlohmann::json::parse(Read(fixture.package.root / ".horo/local/build/gameplay-debug/gameplay_source_libraries.json"));
    const auto cache = Utf8Path(prepared["libraries"][0]["includeRoot"].get<std::string>());
    CHECK(cache.parent_path().parent_path().filename().string().find(':') == std::string::npos);
    const auto source = cache / "Answer.cpp";
    CHECK(Read(source).find("return 42") != std::string::npos);
    CHECK(Read(cache / "CMakeLists.txt").find("must never execute") != std::string::npos);
    SECTION("changed content") {
        Write(source, "int untrusted_replacement;\n");
    }
    SECTION("same-sized changed content") {
        auto original = Read(source);
        original[original.find("42")] = '3';
        Write(source, original);
    }
    SECTION("missing content") {
        std::filesystem::remove(source);
    }
    SECTION("undeclared content") {
        Write(cache / "Injected.h", "\n");
    }
    SECTION("exhausted traversal depth") {
        auto deep = cache;
        for (int index = 0; index < 26; ++index)
            deep /= "nested";
        std::filesystem::create_directories(deep);
    }
#if !defined(_WIN32)
    SECTION("redirected file") {
        std::filesystem::remove(source);
        std::filesystem::create_symlink(cache / "Other.cpp", source);
    }
#endif
    const auto retry = fixture.service.Start(request);
    REQUIRE(retry.HasValue());
    CHECK(fixture.Await(retry.Value()).state == GameplayBuildState::Failed);
    CHECK(fixture.runner.calls == 1);
}

TEST_CASE("Gameplay source libraries compile through the exported SDK with a reusable project-owned cache",
          "[integration][gameplay][build][packages]") {
    BuildFixture fixture;
    fixture.runner.native = true;
    WriteProject(fixture.package);
    const auto request = fixture.Request();
    const auto started = fixture.service.Start(request);
    REQUIRE(started.HasValue());
    const auto built = fixture.Await(started.Value());
    INFO((built.error ? built.error->message : "no build error"));
    REQUIRE(built.state == GameplayBuildState::Succeeded);
    RequireLibraryModuleStarts(fixture.package);
    REQUIRE(fixture.service.IsUpToDate(request));
    const auto calls = fixture.runner.calls.load();
    const auto cached = fixture.service.Start(request);
    REQUIRE(cached.HasValue());
    REQUIRE(fixture.Await(cached.Value()).state == GameplayBuildState::Succeeded);
    CHECK(fixture.runner.calls == calls);
    const auto statePath = fixture.package.root / ".horo/local/gameplay_build_state.json";
    const auto success = Read(statePath);
    Write(fixture.package.root / ".horo/packages.json", "{}");
    CHECK_FALSE(fixture.service.IsUpToDate(request));
    CHECK(fixture.service.Start(request).HasError());
    CheckPublishedState(statePath, success);
}

TEST_CASE("Gameplay source configure cannot reuse stale consumption evidence", "[unit][gameplay][build][packages]") {
    BuildFixture fixture;
    const auto request = fixture.Request();
    const auto first = fixture.service.Start(request);
    REQUIRE(first.HasValue());
    REQUIRE(fixture.Await(first.Value()).state == GameplayBuildState::Failed);
    const auto build = fixture.package.root / ".horo/local/build/gameplay-debug";
    const auto manifest = build / "gameplay_source_libraries.json";
    const auto old = Read(manifest);
    Write(manifest.string() + ".consumed", FormatSha256(ComputeSha256(std::as_bytes(std::span{old}))) + '\n');
    const auto prepared = nlohmann::json::parse(old);
    const auto imported = Utf8Path(prepared["libraries"][0]["includeRoot"].get<std::string>()) / "Answer.cpp";
    Write(fixture.package.root / ".horo/local/gameplay_build_inputs.txt",
          imported.lexically_relative(fixture.package.root).generic_string());
    fixture.runner.configureSuccess = true;
    const auto retry = fixture.service.Start(request);
    REQUIRE(retry.HasValue());
    const auto failed = fixture.Await(retry.Value());
    REQUIRE(failed.state == GameplayBuildState::Failed);
    REQUIRE(failed.error);
    CHECK(failed.error->code.Value() == "invalid_input");
    CHECK(fixture.runner.calls == 2);  // No build invocation follows the unconsumed configure.
}

TEST_CASE("Queued gameplay source compilation cancels before package staging or child admission", "[unit][gameplay][build][packages]") {
    BuildFixture fixture;
    const auto entered = std::make_shared<std::atomic_bool>();
    auto blocked = fixture.jobs.SubmitResult({}, [entered](const CancellationToken &token) {
        entered->store(true);
        while (!token.IsCancellationRequested())
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        return Result<void>::Success();
    });
    REQUIRE(blocked.HasValue());
    BlockingJobOwner owner{std::move(blocked).Value()};
    for (int attempt = 0; attempt < 5000 && !entered->load(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    REQUIRE(entered->load());
    const auto started = fixture.service.Start(fixture.Request());
    REQUIRE(started.HasValue());
    REQUIRE(fixture.service.RequestCancel(started.Value()));
    REQUIRE(owner.handle.RequestCancel().HasValue());
    static_cast<void>(owner.handle.Wait());
    CHECK(fixture.Await(started.Value()).state == GameplayBuildState::Cancelled);
    CHECK(fixture.runner.calls == 0);
    CHECK_FALSE(std::filesystem::exists(fixture.package.root / ".horo/local/build/gameplay-debug/imported"));
}
