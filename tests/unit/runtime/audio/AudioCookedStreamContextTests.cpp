#include "Horo/Audio/AudioCooker.h"
#include "Horo/Audio/AudioErrors.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Audio {
    namespace {
        /** @brief Counts real provider loads to prove context rejection precedes package I/O. */
        class RecordingProvider final : public Assets::IAssetProvider {
        public:
            mutable std::atomic<std::uint32_t> loads{};

            Result<bool> Exists(Assets::AssetId, const CancellationToken &) const override {
                return Result<bool>::Success(false);
            }

            Result<std::vector<std::uint8_t>> Load(Assets::AssetId, const CancellationToken &) const override {
                loads.fetch_add(1);
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(AudioErrors::StreamReadFailed));
            }
        };
    }  // namespace

    TEST_CASE("Cooked streaming rejects foreign or empty source state before opening media", "[unit][audio][streaming][cook]") {
        auto provider = std::make_shared<RecordingProvider>();
        const auto target = AssetCookTargetId::Parse("linux-desktop").Value();
        const auto type = Assets::AssetTypeId::Parse("audio.clip").Value();
        const auto asset = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000544").Value();
        auto made = MakeCookedAudioStreamSource(provider, target, type, 4096);
        REQUIRE(made.HasValue());
        const auto &source = made.Value();
        std::uint32_t foreignState{91};
        for (const BorrowedCallbackContext &context : {BorrowedCallbackContext{}, BorrowedCallbackContext{&foreignState}}) {
            const auto opened = source.open(context, asset, {}, 12288, CancellationToken{});
            REQUIRE(opened.HasError());
            CHECK(opened.ErrorValue().code.Value() == AudioErrors::StreamReadFailed.code.Value());
            CHECK(foreignState == 91);
            CHECK(provider->loads.load() == 0);
        }
        const auto opened = source.open(source.context, asset, {}, 12288, CancellationToken{});
        REQUIRE(opened.HasError());
        CHECK(opened.ErrorValue().code.Value() == AudioErrors::StreamReadFailed.code.Value());
        CHECK(provider->loads.load() == 1);
    }
}  // namespace Horo::Audio
