#include "Horo/Application/Internal/OfflineAudioExport.h"
#include "Horo/Audio/AudioErrors.h"
#include "Horo/Foundation/ImmutableFileStorage.h"
#include "Horo/Platform/NativePublicationFileSystem.h"
#include "NativePublicationFiles.h"
#include "OwnedTestDirectory.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/generators/catch_generators.hpp>
#include <limits>
#include <new>
#include <string_view>

using namespace Horo;
using namespace Horo::Audio;
using namespace Horo::Audio::Backend;
using namespace Horo::Application::Internal;

namespace {
    /** @brief Deterministic finite-headroom signal identifies every channel and frame, including the explicit tail. */
    struct Core final {
        std::uint64_t frames{}, quiescedAt{};
        std::size_t primed{}, quiesced{};
        std::stop_source *cancel{};
        bool fail{}, nonfinite{};

        /** @brief Render an identifiable test signal and acknowledge silent lifecycle phases without native resources. */
        static RenderResult Process(void *context, const RenderInvocation &invocation) noexcept {
            auto &self = *static_cast<Core *>(context);
            if (invocation.phase == RenderPhase::Priming) {
                ++self.primed;
                return {RenderDisposition::Ready, AudioCallbackFaultCode::None};
            }
            if (invocation.phase == RenderPhase::Quiescing) {
                ++self.quiesced;
                self.quiescedAt = invocation.sampleFrame;
                return {RenderDisposition::Quiesced, AudioCallbackFaultCode::None};
            }
            if (self.fail)
                return {};
            for (std::size_t channel = 0; channel < invocation.output.planes.size(); ++channel)
                for (std::uint32_t frame = 0; frame < invocation.output.validFrames; ++frame)
                    invocation.output.planes[channel][frame] =
                        self.nonfinite ? std::numeric_limits<float>::infinity()
                                       : static_cast<float>((channel + 1) * 2) + static_cast<float>(invocation.sampleFrame + frame) / 8.0F;
            self.frames += invocation.output.validFrames;
            if (self.cancel)
                self.cancel->request_stop();
            return {RenderDisposition::Rendered, AudioCallbackFaultCode::None};
        }
    };

    /** @brief Isolated native publication root and writer lease, retained through all receipt observations and cleanup. */
    struct Fixture final {
        Tests::OwnedTestDirectory directory{"audio-export"};
        std::filesystem::path anchor = std::filesystem::canonical(directory.Path());
        std::filesystem::path output = anchor / "exports";
        std::shared_ptr<DurableFileSystem> files;
        std::optional<ExclusiveFileLock> writer;
        OfflineAudioWaveExportRequest request;
        Core core;

        Fixture() {
            auto authority = Platform::CreateNativePublicationFileSystem(anchor, "exports");
            REQUIRE(authority.HasValue());
            files = std::move(authority).Value();
            auto acquired = files->TryAcquireExclusive(output / ".writer.lock", "audio export test");
            REQUIRE(acquired.HasValue());
            writer.emplace(std::move(acquired).Value());
            REQUIRE(files->CreateDirectoryPath(output / OfflineAudioWaveStagingDirectoryName, DirectoryCreationMode::Single).HasValue());
            request = {.render = {.epoch = {{AudioRuntimeId::Create(4).Value(), 1, 1}, 1, 1},
                                  .format = {48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::SevenPointOne)},
                                  .blockFrames = 4,
                                  .contentFrames = 5,
                                  .tailFrames = 2,
                                  .clockDomain = 7},
                       .preparedFile = output / OfflineAudioWaveStagingDirectoryName / "one.partial",
                       .destination = output / std::filesystem::path{u8"çıktı ses.wav"}};
        }

        Result<OfflineAudioWaveExportSummary> Export(OfflineAudioWaveExportReceipt &receipt, DurableFileSystem *injected = nullptr) {
            return ExportOfflineAudioWave(request, {&core, Core::Process}, injected ? *injected : *files, receipt);
        }

        void Existing(const std::filesystem::path &path) {
            constexpr std::array<std::byte, 3> old{std::byte{1}, std::byte{2}, std::byte{3}};
            REQUIRE(files->WriteDurable(path, old).HasValue());
        }

        std::vector<std::uint8_t> Read(const std::filesystem::path &path) const {
            auto read = files->ReadFileBounded(path, 4096);
            REQUIRE(read.HasValue());
            return std::move(read).Value();
        }

        bool Exists(const std::filesystem::path &path) const {
            const auto info = files->InspectEntry(path);
            REQUIRE(info.HasValue());
            return info.Value().has_value();
        }
    };

    /** @brief Fault only declared publication boundaries; the shared adapter performs real native I/O in the owned test root. */
    class FaultFiles final : public TestSupport::NativePublicationFiles {
    public:
        bool writeBefore{}, writeAfter{}, replaceBefore{}, replaceAfter{}, removeFailure{};
        bool emptyLease{}, throwAfterWrite{}, inspectLeaseOnCleanup{}, leaseHeldOnCleanup{};
        bool cancelAfterWrite{}, cancelAfterCommit{};
        std::stop_source *cancellation{};
        std::size_t removes{};

        Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path, const std::string_view owner) override {
            return emptyLease ? Result<ExclusiveFileLock>::Success(ExclusiveFileLock{}) : native.TryAcquireExclusive(path, owner);
        }

        Result<void> WritePrivateDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes, bool &created,
                                         const std::filesystem::perms permissions) override {
            if (writeBefore)
                return Result<void>::Failure(MakeError(FileStorageErrors::IoFailed, "Injected write admission failure"));
            auto written = native.WritePrivateDurable(path, bytes, created, permissions);
            if (written.HasValue() && throwAfterWrite)
                throw std::bad_alloc{};
            if (written.HasValue() && cancellation && cancelAfterWrite)
                cancellation->request_stop();
            if (written.HasValue() && writeAfter)
                return Result<void>::Failure(MakeError(FileStorageErrors::IoFailed, "Injected post-create failure"));
            return written;
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                          AtomicFileReplacementReceipt &receipt) override {
            if (replaceBefore)
                return Result<void>::Failure(MakeError(FileStorageErrors::IoFailed, "Injected pre-commit failure"));
            auto replaced = native.AtomicReplaceTracked(prepared, destination, receipt);
            if (replaced.HasValue() && cancellation && cancelAfterCommit)
                cancellation->request_stop();
            if (replaced.HasValue() && replaceAfter)
                return Result<void>::Failure(MakeError(FileStorageErrors::IoFailed, "Injected post-commit durability failure"));
            return replaced;
        }

        Result<void> RemoveDurable(const std::filesystem::path &path) override {
            ++removes;
            if (inspectLeaseOnCleanup) {
                const auto competing = native.TryAcquireExclusive(path.parent_path() / OfflineAudioWaveWriterLock, "cleanup lease probe");
                leaseHeldOnCleanup = competing.HasError() && competing.ErrorValue().code.Value() == "filesystem.lock_busy";
            }
            return removeFailure ? Result<void>::Failure(MakeError(FileStorageErrors::IoFailed, "Injected cleanup failure"))
                                 : native.RemoveDurable(path);
        }
    };

    /** @brief Parse little-endian RIFF integers independently of the encoder's write helpers. */
    std::uint32_t Little(const std::span<const std::uint8_t> bytes, const std::size_t offset, const std::size_t width = 4) {
        REQUIRE(offset + width <= bytes.size());
        std::uint32_t result{};
        for (std::size_t index = 0; index < width; ++index)
            result |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8);
        return result;
    }

    /** @brief Traverse bounded RIFF chunks, including odd-byte padding, rather than assuming encoder offsets. */
    std::span<const std::uint8_t> Chunk(const std::vector<std::uint8_t> &bytes, const std::string_view name) {
        for (std::size_t offset = 12; offset + 8 <= bytes.size();) {
            const auto count = Little(bytes, offset + 4);
            REQUIRE(count <= bytes.size() - offset - 8);
            if (std::string_view{reinterpret_cast<const char *>(bytes.data() + offset), 4} == name)
                return std::span{bytes}.subspan(offset + 8, count);
            offset += 8 + count + (count & 1);
        }
        FAIL("Required RIFF chunk missing");
        return {};
    }

    TEST_CASE("Offline application exports complete semantic float WAVE through a scoped native authority", "[audio][offline][export]") {
        Fixture fixture;
        fixture.Existing(fixture.request.destination);
        OfflineAudioWaveExportReceipt receipt;
        const auto result = fixture.Export(receipt);
        REQUIRE(result.HasValue());
        REQUIRE(receipt.preparedCreated);
        REQUIRE(receipt.publication.WasCommitted());
        REQUIRE_FALSE(fixture.Exists(fixture.request.preparedFile));
        REQUIRE(fixture.core.frames == 7);
        REQUIRE(fixture.core.quiesced == 1);
        REQUIRE(fixture.core.quiescedAt == 7);
        const auto bytes = fixture.Read(fixture.request.destination);
        REQUIRE(std::string_view{reinterpret_cast<const char *>(bytes.data()), 4} == "RIFF");
        REQUIRE(std::string_view{reinterpret_cast<const char *>(bytes.data() + 8), 4} == "WAVE");
        REQUIRE(Little(bytes, 4) == bytes.size() - 8);
        const auto format = Chunk(bytes, "fmt ");
        REQUIRE(format.size() == 40);
        REQUIRE(Little(format, 0, 2) == 0xfffe);
        REQUIRE(Little(format, 2, 2) == 8);
        REQUIRE(Little(format, 4) == 48'000);
        REQUIRE(Little(format, 8) == 48'000 * 32);
        REQUIRE(Little(format, 12, 2) == 32);
        REQUIRE(Little(format, 14, 2) == 32);
        REQUIRE(Little(format, 16, 2) == 22);
        REQUIRE(Little(format, 18, 2) == 32);
        REQUIRE(Little(format, 20) == 0x63f);
        constexpr std::array<std::uint8_t, 16> FloatGuid{3, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        REQUIRE(std::ranges::equal(format.subspan(24), FloatGuid));
        REQUIRE(Little(Chunk(bytes, "fact"), 0) == 7);
        const auto data = Chunk(bytes, "data");
        REQUIRE(data.size() == 7 * 8 * 4);
        constexpr std::array<float, 8> FirstFrame{2, 4, 6, 8, 14, 16, 10, 12};
        for (std::size_t frame = 0; frame < 7; ++frame)
            for (std::size_t channel = 0; channel < FirstFrame.size(); ++channel)
                REQUIRE(std::bit_cast<float>(Little(data, (frame * 8 + channel) * 4)) ==
                        FirstFrame[channel] + static_cast<float>(frame) / 8.0F);
        REQUIRE(result.Value().encodedBytes == bytes.size());
        REQUIRE(fixture.Export(receipt).HasError());
        REQUIRE(fixture.core.primed == 1);  // A receipt cannot authorize another render or replacement.
        Core partitioned;
        OfflineAudioWaveExportReceipt secondReceipt;
        fixture.request.render.blockFrames = 3;
        fixture.request.destination = fixture.output / "partitioned.wav";
        REQUIRE(ExportOfflineAudioWave(fixture.request, {&partitioned, Core::Process}, *fixture.files, secondReceipt).HasValue());
        REQUIRE(fixture.Read(fixture.request.destination) == bytes);
        REQUIRE(partitioned.quiescedAt == 7);
    }

    TEST_CASE("Offline WAVE preserves every supported speaker role including height channels", "[audio][offline][export]") {
        Fixture fixture;
        fixture.request.render.format.layout = MakeAudioSpeakerLayout(AudioSpeakerPreset::SevenPointOneFour);
        OfflineAudioWaveExportReceipt receipt;
        REQUIRE(fixture.Export(receipt).HasValue());
        const auto bytes = fixture.Read(fixture.request.destination);
        const auto format = Chunk(bytes, "fmt ");
        REQUIRE(Little(format, 2, 2) == 12);
        REQUIRE(Little(format, 20) == 0x2d63f);
        const auto data = Chunk(bytes, "data");
        constexpr std::array<float, 12> FirstFrame{2, 4, 6, 8, 14, 16, 10, 12, 18, 20, 22, 24};
        for (std::size_t channel = 0; channel < FirstFrame.size(); ++channel)
            REQUIRE(std::bit_cast<float>(Little(data, channel * 4)) == FirstFrame[channel]);
    }

    TEST_CASE("Offline file admission rejects invalid bounds and paths before rendering", "[audio][offline][export]") {
        Fixture fixture;
        SECTION("encoded budget") {
            fixture.request.maximumEncodedBytes = 79;
        }
        SECTION("frame arithmetic") {
            fixture.request.render.contentFrames = std::numeric_limits<std::uint64_t>::max();
        }
        SECTION("discrete semantics cannot become speaker metadata") {
            fixture.request.render.format.layout = {AudioLayoutKind::Discrete, {AudioDiscreteChannel{0}}, std::nullopt};
        }
        SECTION("same-directory temporary cannot alias a destination spelling") {
            fixture.request.preparedFile = fixture.output / "one.partial";
        }
        SECTION("unadmitted staging role") {
            fixture.request.preparedFile = fixture.output / "other" / "one.partial";
        }
        SECTION("relative destination") {
            fixture.request.destination = "relative.wav";
        }
        SECTION("outside the supplied authority") {
            fixture.request.destination = fixture.anchor / "outside.wav";
            fixture.request.preparedFile = fixture.anchor / OfflineAudioWaveStagingDirectoryName / "one.partial";
        }
        OfflineAudioWaveExportReceipt receipt;
        REQUIRE(fixture.Export(receipt).HasError());
        REQUIRE(fixture.core.primed == 0);
        REQUIRE_FALSE(receipt.preparedCreated);
        REQUIRE_FALSE(receipt.publication.WasCommitted());
        REQUIRE_FALSE(std::filesystem::exists(fixture.request.destination));
    }

    TEST_CASE("Pre-cancelled offline export never renders or creates a prepared file", "[audio][offline][export]") {
        Fixture fixture;
        std::stop_source cancellation;
        cancellation.request_stop();
        fixture.request.render.cancellation = cancellation.get_token();
        OfflineAudioWaveExportReceipt receipt;
        const auto result = fixture.Export(receipt);
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == AudioErrors::OperationCancelled.code.Value());
        REQUIRE(fixture.core.primed == 0);
        REQUIRE_FALSE(receipt.admitted);
        REQUIRE_FALSE(receipt.preparedCreated);
    }

    TEST_CASE("An admitted failed render cannot reuse its export receipt before private-file creation", "[audio][offline][export]") {
        Fixture fixture;
        fixture.core.fail = true;
        OfflineAudioWaveExportReceipt receipt;
        REQUIRE(fixture.Export(receipt).HasError());
        REQUIRE(receipt.admitted);
        REQUIRE_FALSE(receipt.preparedCreated);
        REQUIRE(fixture.core.primed == 1);
        fixture.core.fail = false;
        const auto retry = fixture.Export(receipt);
        REQUIRE(retry.HasError());
        REQUIRE(retry.ErrorValue().code.Value() == FileStorageErrors::Conflict.code.Value());
        REQUIRE(fixture.core.primed == 1);
        REQUIRE_FALSE(receipt.publication.WasCommitted());
    }

    TEST_CASE("A real occupied export writer lease rejects before consuming the offline core", "[audio][offline][export]") {
        Fixture fixture;
        auto held = fixture.files->TryAcquireExclusive(fixture.request.preparedFile.parent_path() / OfflineAudioWaveWriterLock,
                                                       "other export transaction");
        REQUIRE(held.HasValue());
        OfflineAudioWaveExportReceipt receipt;
        REQUIRE(fixture.Export(receipt).HasError());
        REQUIRE(fixture.core.primed == 0);
        REQUIRE_FALSE(receipt.preparedCreated);
        REQUIRE_FALSE(receipt.publication.WasCommitted());
    }

    TEST_CASE("An empty successful lease result cannot authorize offline rendering", "[audio][offline][export]") {
        Fixture fixture;
        FaultFiles files;
        files.emptyLease = true;
        OfflineAudioWaveExportReceipt receipt;
        REQUIRE(fixture.Export(receipt, &files).HasError());
        REQUIRE(fixture.core.primed == 0);
        REQUIRE_FALSE(receipt.preparedCreated);
    }

    TEST_CASE("Offline allocation failure after private creation cleans while the actual writer lease stays held",
              "[audio][offline][export]") {
        Fixture fixture;
        fixture.Existing(fixture.request.destination);
        FaultFiles files;
        files.throwAfterWrite = true;
        files.inspectLeaseOnCleanup = true;
        OfflineAudioWaveExportReceipt receipt;
        const auto result = fixture.Export(receipt, &files);
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == AudioErrors::MemoryAllocationFailed.code.Value());
        REQUIRE(receipt.preparedCreated);
        REQUIRE(receipt.preparedRemoved);
        REQUIRE_FALSE(receipt.publication.WasCommitted());
        REQUIRE(files.leaseHeldOnCleanup);
        REQUIRE(fixture.Read(fixture.request.destination) == std::vector<std::uint8_t>{1, 2, 3});
    }

    TEST_CASE("Offline export never deletes an existing prepared file or publishes render failure", "[audio][offline][export]") {
        Fixture fixture;
        fixture.Existing(fixture.request.destination);
        SECTION("pre-existing prepared leaf") {
            fixture.Existing(fixture.request.preparedFile);
        }
        SECTION("render fault") {
            fixture.core.fail = true;
        }
        SECTION("nonfinite output") {
            fixture.core.nonfinite = true;
        }
        OfflineAudioWaveExportReceipt receipt;
        REQUIRE(fixture.Export(receipt).HasError());
        REQUIRE_FALSE(receipt.preparedCreated);
        REQUIRE_FALSE(receipt.publication.WasCommitted());
        REQUIRE(fixture.Read(fixture.request.destination) == std::vector<std::uint8_t>{1, 2, 3});
        if (fixture.Exists(fixture.request.preparedFile))
            REQUIRE(fixture.Read(fixture.request.preparedFile) == std::vector<std::uint8_t>{1, 2, 3});
    }

    TEST_CASE("Offline pre-commit faults preserve destination and clean only invocation-created storage", "[audio][offline][export]") {
        Fixture fixture;
        fixture.Existing(fixture.request.destination);
        FaultFiles files;
        SECTION("write rejected before creation") {
            files.writeBefore = true;
        }
        SECTION("write failed after creation") {
            files.writeAfter = true;
        }
        SECTION("replace failed before commit") {
            files.replaceBefore = true;
        }
        SECTION("cleanup also failed") {
            files.writeAfter = true;
            files.removeFailure = true;
        }
        OfflineAudioWaveExportReceipt receipt;
        const auto result = fixture.Export(receipt, &files);
        REQUIRE(result.HasError());
        REQUIRE_FALSE(receipt.publication.WasCommitted());
        REQUIRE(fixture.Read(fixture.request.destination) == std::vector<std::uint8_t>{1, 2, 3});
        REQUIRE(fixture.Exists(fixture.request.preparedFile) == files.removeFailure);
        REQUIRE(receipt.preparedRemoved == (receipt.preparedCreated && !files.removeFailure));
        REQUIRE(files.removes == (receipt.preparedCreated ? std::size_t{1} : std::size_t{0}));
        if (files.removeFailure)
            REQUIRE(result.ErrorValue().cause.Get() != nullptr);
    }

    TEST_CASE("Offline post-commit durability failure retains irreversible publication evidence", "[audio][offline][export]") {
        Fixture fixture;
        fixture.Existing(fixture.request.destination);
        FaultFiles files;
        files.replaceAfter = true;
        OfflineAudioWaveExportReceipt receipt;
        REQUIRE(fixture.Export(receipt, &files).HasError());
        REQUIRE(receipt.publication.WasCommitted());
        REQUIRE_FALSE(receipt.preparedRemoved);
        REQUIRE(files.removes == 0);
        REQUIRE_FALSE(fixture.Exists(fixture.request.preparedFile));
        REQUIRE(Chunk(fixture.Read(fixture.request.destination), "data").size() == 7 * 8 * 4);
    }

    TEST_CASE("Offline cancellation before commit cleans preparation while successful commit wins cancellation",
              "[audio][offline][export]") {
        Fixture fixture;
        fixture.Existing(fixture.request.destination);
        std::stop_source cancellation;
        fixture.request.render.cancellation = cancellation.get_token();
        FaultFiles files;
        files.cancellation = &cancellation;
        bool committed{};
        SECTION("cancel during render") {
            fixture.core.cancel = &cancellation;
        }
        SECTION("cancel after write") {
            files.cancelAfterWrite = true;
        }
        SECTION("cancel after commit") {
            files.cancelAfterCommit = true;
            committed = true;
        }
        OfflineAudioWaveExportReceipt receipt;
        const auto result = fixture.Export(receipt, &files);
        REQUIRE(result.HasValue() == committed);
        REQUIRE(receipt.publication.WasCommitted() == committed);
        REQUIRE_FALSE(fixture.Exists(fixture.request.preparedFile));
        if (!committed)
            REQUIRE(fixture.Read(fixture.request.destination) == std::vector<std::uint8_t>{1, 2, 3});
    }
}  // namespace
