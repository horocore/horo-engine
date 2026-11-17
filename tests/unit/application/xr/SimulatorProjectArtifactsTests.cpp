#include "Horo/Foundation/Platform.h"
#include "Horo/Platform/NativeProjectIoOwners.h"
#include "SimulatorProjectArtifacts.h"
#include "support/OwnedTestDirectory.h"

#include <algorithm>
#include <catch2/generators/catch_generators.hpp>
#include <fstream>
#include <functional>
#include <iterator>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Application::Internal;
    using namespace Horo::Platform;

    /** @brief Actual admitted simulator interaction, not a manually relabeled native sample. */
    std::vector<std::byte> Recording() {
        auto created = XR::XRSimulatorRuntime::Create();
        REQUIRE(created.HasValue());
        auto runtime = std::move(created).Value();
        REQUIRE(runtime->ArmCapture().HasValue());
        const auto simulation = XR::XRSimulationTime::Create(1);
        REQUIRE(simulation.HasValue());
        REQUIRE(runtime->Step({1, simulation.Value()}).HasValue());
        REQUIRE(runtime->StopCapture().HasValue());
        const auto recorded = runtime->RecordedScenario();
        REQUIRE(recorded.HasValue());
        auto encoded = XR::EncodeXRSimulatorScenario(recorded.Value());
        REQUIRE(encoded.HasValue());
        return std::move(encoded).Value();
    }

    /** @brief Inject publication failures while preserving actual native leases and retained-root operations. */
    struct Publication final : DurableFileSystem {
        std::shared_ptr<DurableFileSystem> files;
        bool failWrite{};
        bool failBeforeReplace{};
        bool failAfterReplace{};
        std::function<void()> afterWrite;
        std::size_t writes{};

        explicit Publication(std::shared_ptr<DurableFileSystem> owner) : files(std::move(owner)) {}

        Result<bool> CreateDirectoryPath(const std::filesystem::path &path, DirectoryCreationMode mode) override {
            return files->CreateDirectoryPath(path, mode);
        }

        Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path, std::string_view metadata) override {
            return files->TryAcquireExclusive(path, metadata);
        }

        Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
            return files->AvailableBytes(path);
        }

        Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override {
            return files->WriteDurable(path, bytes);
        }

        Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                         std::filesystem::perms permissions) override {
            ++writes;
            auto result = files->WritePrivateDurable(path, bytes, created, permissions);
            if (result.HasError())
                return result;
            if (afterWrite)
                afterWrite();
            return failWrite ? Result<void>::Failure(MakeError(ProjectReadErrors::ReadFailed)) : result;
        }

        Result<void> CopyDurable(const std::filesystem::path &from, const std::filesystem::path &to) override {
            return files->CopyDurable(from, to);
        }

        Result<void> AtomicReplace(const std::filesystem::path &from, const std::filesystem::path &to) override {
            return files->AtomicReplace(from, to);
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &from, const std::filesystem::path &to,
                                          AtomicFileReplacementReceipt &receipt) override {
            if (failBeforeReplace)
                return Result<void>::Failure(MakeError(ProjectReadErrors::ReadFailed));
            auto result = files->AtomicReplaceTracked(from, to, receipt);
            if (result.HasError())
                return result;
            return failAfterReplace ? Result<void>::Failure(MakeError(ProjectReadErrors::ReadFailed)) : result;
        }

        Result<void> RemoveDurable(const std::filesystem::path &path) override {
            return files->RemoveDurable(path);
        }

        Result<void> SyncDirectory(const std::filesystem::path &path) override {
            return files->SyncDirectory(path);
        }
    };

    struct Fixture final {
        Tests::OwnedTestDirectory directory{"simulator-artifacts"};
        std::filesystem::path anchor{std::filesystem::canonical(directory.Path())};
        std::filesystem::path root{anchor / "project"};
        NativeProjectIoOwners owners;
        std::shared_ptr<Publication> files;
        std::shared_ptr<SimulatorProjectArtifacts> artifacts;
        CancellationSource cancellation;
        bool stopped{};
        std::function<void()> duringCheck;
        std::vector<std::byte> bytes{Recording()};

        Fixture() {
            REQUIRE(std::filesystem::create_directory(root));
            auto admitted = CreateNativeProjectIoOwners(root, "simulator-fixture", 7, ".horo/xr-simulator/cache",
                                                        SimulatorProjectArtifacts::PublicationDirectory);
            REQUIRE(admitted.HasValue());
            owners = std::move(admitted).Value();
            files = std::make_shared<Publication>(owners.publication);
            auto created = SimulatorProjectArtifacts::Create({owners.source,
                                                              files,
                                                              {.projectIdentity = "simulator-fixture",
                                                               .projectGeneration = 7,
                                                               .cancellation = cancellation.Token(),
                                                               .authorityStopped =
                                                                   [this] {
                if (duringCheck)
                    duringCheck();
                return stopped;
            }},
                                                              root});
            REQUIRE(created.HasValue());
            artifacts = std::move(created).Value();
        }

        std::filesystem::path Destination() const {
            return root / SimulatorProjectArtifacts::PublicationDirectory / "capture.json";
        }

        void Source(std::string_view path = "scenario.json") const {
            std::ofstream stream{root / path, std::ios::binary};
            stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            REQUIRE(stream.good());
        }

        void OldDestination() const {
            std::filesystem::create_directories(Destination().parent_path());
            std::ofstream{Destination(), std::ios::binary} << "previous capture";
        }

        std::string Contents(const std::filesystem::path &path) const {
            std::ifstream input{path, std::ios::binary};
            REQUIRE(input.good());
            return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        }
    };
}  // namespace

TEST_CASE("Simulator artifacts round trip real bounded capture through admitted project owners", "[xr][simulator][artifacts]") {
    Fixture fixture;
    REQUIRE_FALSE(std::filesystem::exists(fixture.root / ".horo"));
    fixture.Source();
    REQUIRE(fixture.artifacts->ReadScenario("scenario.json").Value() == fixture.bytes);
    AtomicFileReplacementReceipt receipt;
    REQUIRE(fixture.artifacts->WriteCapture("capture.json", fixture.bytes, receipt).HasValue());
    REQUIRE(receipt.WasCommitted());
    const auto loaded = fixture.artifacts->ReadScenario(".horo/xr-simulator/captures/capture.json");
    REQUIRE(loaded.HasValue());
    REQUIRE(loaded.Value() == fixture.bytes);
    REQUIRE(fixture.artifacts->PublicationNamespace() == ".horo/xr-simulator/captures");
}

TEST_CASE("Simulator artifact failures preserve the previous destination and clean only our stage", "[xr][simulator][artifacts]") {
    Fixture fixture;
    fixture.OldDestination();
    SECTION("private write fails after creation") {
        fixture.files->failWrite = true;
    }
    SECTION("replacement fails before commit") {
        fixture.files->failBeforeReplace = true;
    }
    SECTION("original cancellation after stage") {
        fixture.files->afterWrite = [&] {
            fixture.cancellation.RequestCancellation();
        };
    }
    SECTION("authority revoked after stage") {
        fixture.files->afterWrite = [&] {
            fixture.artifacts->Revoke();
        };
    }
    AtomicFileReplacementReceipt receipt;
    REQUIRE(fixture.artifacts->WriteCapture("capture.json", fixture.bytes, receipt).HasError());
    REQUIRE_FALSE(receipt.WasCommitted());
    REQUIRE(fixture.Contents(fixture.Destination()) == "previous capture");
    REQUIRE_FALSE(std::filesystem::exists(fixture.Destination().string() + ".pending"));
}

TEST_CASE("Simulator artifact durability failure reports the irreversible replacement truthfully", "[xr][simulator][artifacts]") {
    Fixture fixture;
    fixture.OldDestination();
    fixture.files->failAfterReplace = true;
    AtomicFileReplacementReceipt receipt;
    const auto written = fixture.artifacts->WriteCapture("capture.json", fixture.bytes, receipt);
    REQUIRE(written.HasError());
    REQUIRE(written.ErrorValue().code.Value() == ProjectReadErrors::ReadFailed.code.Value());
    REQUIRE(receipt.WasCommitted());
    const auto stored = fixture.Contents(fixture.Destination());
    REQUIRE(std::ranges::equal(std::as_bytes(std::span{stored.data(), stored.size()}), fixture.bytes));
}

TEST_CASE("Simulator artifact existing private stages are never overwritten or removed", "[xr][simulator][artifacts]") {
    Fixture fixture;
    fixture.OldDestination();
    const auto pending = fixture.Destination().string() + ".pending";
    std::ofstream{pending, std::ios::binary} << "unrelated private stage";
    AtomicFileReplacementReceipt receipt;
    REQUIRE(fixture.artifacts->WriteCapture("capture.json", fixture.bytes, receipt).HasError());
    REQUIRE_FALSE(receipt.WasCommitted());
    REQUIRE(fixture.Contents(pending) == "unrelated private stage");
    REQUIRE(fixture.Contents(fixture.Destination()) == "previous capture");
}

TEST_CASE("Simulator artifact names cannot escape or collide with writer-stage namespaces", "[xr][simulator][artifacts]") {
    Fixture fixture;
    const auto name = GENERATE("../escape.json", "nested/file.json", "capture.json.lock", "CON.json", "capture.pending", "/file.json");
    AtomicFileReplacementReceipt receipt;
    REQUIRE(fixture.artifacts->WriteCapture(name, fixture.bytes, receipt).HasError());
    REQUIRE_FALSE(receipt.WasCommitted());
    REQUIRE(fixture.files->writes == 0);
    REQUIRE_FALSE(std::filesystem::exists(fixture.root / ".horo"));
}

TEST_CASE("Simulator artifact cancelled or retired project owners deny access before I/O", "[xr][simulator][artifacts]") {
    Fixture fixture;
    fixture.Source();
    SECTION("original project cancellation") {
        fixture.cancellation.RequestCancellation();
    }
    SECTION("original live authority stopped") {
        fixture.stopped = true;
    }
    SECTION("any-thread terminal revocation") {
        std::thread revoke{[port = fixture.artifacts] {
            port->Revoke();
        }};
        revoke.join();
    }
    REQUIRE(fixture.artifacts->ReadScenario("scenario.json").HasError());
    AtomicFileReplacementReceipt receipt;
    REQUIRE(fixture.artifacts->WriteCapture("capture.json", fixture.bytes, receipt).HasError());
    REQUIRE(fixture.files->writes == 0);
    REQUIRE_FALSE(std::filesystem::exists(fixture.root / ".horo"));
}

TEST_CASE("Simulator artifact capabilities keep the admitted root after pathname replacement", "[xr][simulator][artifacts]") {
    Fixture fixture;
    fixture.Source();
    const auto retained = fixture.anchor / "retained-project";
    std::filesystem::rename(fixture.root, retained);
    REQUIRE(std::filesystem::create_directory(fixture.root));
    std::ofstream{fixture.root / "scenario.json"} << "replacement root";
    REQUIRE(fixture.artifacts->ReadScenario("scenario.json").Value() == fixture.bytes);
    AtomicFileReplacementReceipt receipt;
    REQUIRE(fixture.artifacts->WriteCapture("capture.json", fixture.bytes, receipt).HasValue());
    REQUIRE(receipt.WasCommitted());
    REQUIRE(std::filesystem::exists(retained / SimulatorProjectArtifacts::PublicationDirectory / "capture.json"));
    REQUIRE_FALSE(std::filesystem::exists(fixture.root / ".horo"));
}

TEST_CASE("Simulator artifact authority callbacks cannot reenter or issue writes", "[xr][simulator][artifacts]") {
    Fixture fixture;
    fixture.Source();
    bool readDenied{};
    bool writeDenied{};
    fixture.duringCheck = [&] {
        readDenied = fixture.artifacts->ReadScenario("scenario.json").HasError();
        AtomicFileReplacementReceipt nested;
        writeDenied = fixture.artifacts->WriteCapture("nested.json", fixture.bytes, nested).HasError();
    };
    REQUIRE(fixture.artifacts->ReadScenario("scenario.json").HasValue());
    REQUIRE(readDenied);
    REQUIRE(writeDenied);
    REQUIRE(fixture.files->writes == 0);
    REQUIRE_FALSE(std::filesystem::exists(fixture.root / ".horo"));
}

TEST_CASE("Simulator artifact wrong-thread operations fail before accessing project capabilities", "[xr][simulator][artifacts]") {
    Fixture fixture;
    fixture.Source();
    bool readDenied{};
    bool writeDenied{};
    std::thread other{[&] {
        readDenied = fixture.artifacts->ReadScenario("scenario.json").HasError();
        AtomicFileReplacementReceipt receipt;
        writeDenied = fixture.artifacts->WriteCapture("capture.json", fixture.bytes, receipt).HasError();
    }};
    other.join();
    REQUIRE(readDenied);
    REQUIRE(writeDenied);
    REQUIRE(fixture.files->writes == 0);
    REQUIRE_FALSE(std::filesystem::exists(fixture.root / ".horo"));
}

TEST_CASE("Simulator artifact invalid capture bytes never create output", "[xr][simulator][artifacts]") {
    Fixture fixture;
    const std::string invalid = "not a simulator capture";
    AtomicFileReplacementReceipt receipt;
    REQUIRE(fixture.artifacts->WriteCapture("capture.json", std::as_bytes(std::span{invalid.data(), invalid.size()}), receipt).HasError());
    REQUIRE(fixture.files->writes == 0);
    REQUIRE_FALSE(std::filesystem::exists(fixture.root / ".horo"));
}
