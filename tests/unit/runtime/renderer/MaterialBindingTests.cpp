#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/MaterialBindingErrors.h"
#include "MaterialBindingTestFixture.h"
#include "support/AllocationProbe.h"
#include "support/TypedIdentityTestSupport.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Tests::MaterialBindingFixture;

    struct Backend final : IMaterialBindingBackend {
        struct Lease final : IResidentMaterialBinding {
            explicit Lease(Backend &owner) : backend(&owner) {
                ++backend->live;
            }

            ~Lease() override {
                --backend->live;
            }

            Backend *backend;
        };

        std::size_t calls{0};
        std::size_t live{0};
        std::optional<Error> failure;
        bool throws{false};
        bool empty{false};

        Result<std::unique_ptr<IResidentMaterialBinding>> Realize(const MaterialBindingDescriptor &) override {
            ++calls;
            if (throws)
                throw 1;
            if (failure)
                return Result<std::unique_ptr<IResidentMaterialBinding>>::Failure(*failure);
            if (empty)
                return Result<std::unique_ptr<IResidentMaterialBinding>>::Success({});
            return Result<std::unique_ptr<IResidentMaterialBinding>>::Success(std::make_unique<Lease>(*this));
        }
    };

    [[nodiscard]] std::unique_ptr<MaterialBindingTable> Table(Backend &backend, const MaterialBindingLimits &limits = {}) {
        auto table = MaterialBindingTable::Create(Owner, backend, limits);
        REQUIRE(table.HasValue());
        return std::move(table).Value();
    }

}  // namespace

TEST_CASE("Generic material layouts preserve exact final target packing", "[renderer][material]") {
    auto descriptor = Descriptor();
    NormalizedShaderReflection reflection{ShaderTargetBackend::Metal,   1,  descriptor.layout.shaderInterface, descriptor.layout.resources,
                                          descriptor.layout.parameters, {}, descriptor.layout.targetBindings,  {}};
    auto layout = PrepareMaterialBindingLayout(reflection);
    REQUIRE(layout.HasValue());
    descriptor.layout = std::move(layout).Value();
    REQUIRE(ValidateMaterialBindingDescriptor(descriptor, Owner).Value() == 12);
    reflection.bindings.push_back(reflection.bindings.front());
    ErrorIs(PrepareMaterialBindingLayout(reflection), MaterialBindingErrors::InvalidDescriptor);
}

TEST_CASE("Material admission rejects malformed resource membership before backend work", "[renderer][material]") {
    Backend backend;
    auto table = Table(backend);
    auto descriptor = Descriptor();
    SECTION("duplicate element") {
        descriptor.resources.push_back(descriptor.resources.front());
    }
    SECTION("missing element") {
        descriptor.resources.clear();
    }
    SECTION("wrong kind") {
        descriptor.resources[0].value = RenderSamplerHandle{Owner, 3, 1};
    }
    SECTION("array overflow") {
        descriptor.resources[0].arrayElement = 1;
    }
    SECTION("foreign pipeline") {
        descriptor.pipeline.owner = {99};
    }
    SECTION("short packed block") {
        std::get<MaterialParameterBinding>(descriptor.resources[0].value).bytes.resize(8);
    }
    SECTION("oversized reflected array") {
        descriptor.layout.resources[0].arrayCount = 129;
    }
    SECTION("missing target map") {
        descriptor.layout.targetBindings.clear();
    }
    SECTION("invalid shape") {
        descriptor.layout.parameters[0].rows = 5;
    }
    SECTION("overflow packing") {
        descriptor.layout.parameters[0].byteOffset = std::numeric_limits<std::uint32_t>::max();
    }
    ErrorIs(table->Publish(std::move(descriptor)), MaterialBindingErrors::InvalidDescriptor);
    REQUIRE(backend.calls == 0);
    REQUIRE(table->Snapshot().retainedGenerations == 0);
}

TEST_CASE("Generic material resource arrays require every exact owned element", "[renderer][material]") {
    auto descriptor = Descriptor();
    descriptor.layout.parameters.clear();
    descriptor.layout.resources[0].kind = ShaderResourceKind::SampledTexture;
    descriptor.layout.resources[0].arrayCount = 2;
    descriptor.resources = {{{1}, 0, RenderTextureViewHandle{Owner, 3, 1}}, {{1}, 1, RenderTextureViewHandle{Owner, 4, 2}}};
    REQUIRE(ValidateMaterialBindingDescriptor(descriptor, Owner).Value() == 0);
    std::get<RenderTextureViewHandle>(descriptor.resources[1].value).owner = {99};
    ErrorIs(ValidateMaterialBindingDescriptor(descriptor, Owner), MaterialBindingErrors::InvalidDescriptor);
}

TEST_CASE("Reflected matrix and array extents bound parameter blocks", "[renderer][material]") {
    auto descriptor = Descriptor();
    auto &parameter = descriptor.layout.parameters[0];
    parameter.rows = 3;
    parameter.columns = 3;
    parameter.arrayCount = 2;
    parameter.matrixStride = 16;
    parameter.arrayStride = 48;
    auto &bytes = std::get<MaterialParameterBinding>(descriptor.resources[0].value).bytes;
    bytes.resize(92);
    REQUIRE(ValidateMaterialBindingDescriptor(descriptor, Owner).HasValue());
    bytes.resize(91);
    ErrorIs(ValidateMaterialBindingDescriptor(descriptor, Owner), MaterialBindingErrors::InvalidDescriptor);
}

TEST_CASE("Published generation owns immutable parameters and exact backend resource lease", "[renderer][material]") {
    Backend backend;
    auto table = Table(backend);
    auto descriptor = Descriptor();
    auto selection = table->Publish(descriptor);
    REQUIRE(selection.HasValue());
    REQUIRE_FALSE(selection.Value().usedFallback);
    auto lease = table->Acquire(selection.Value().binding);
    REQUIRE(lease.HasValue());
    std::get<MaterialParameterBinding>(descriptor.resources[0].value).bytes[0] = std::byte{9};
    REQUIRE(std::get<MaterialParameterBinding>(lease.Value()->Descriptor().resources[0].value).bytes[0] == std::byte{0});
    REQUIRE(table->Release(selection.Value().binding).HasValue());
    ErrorIs(table->Acquire(selection.Value().binding), MaterialBindingErrors::StaleBinding);
    REQUIRE(backend.live == 1);
    REQUIRE(table->Snapshot().retainedParameterBytes == 12);
    std::move(lease).Value().reset();
    REQUIRE(backend.live == 0);
    REQUIRE(table->Snapshot().retainedGenerations == 0);
}

TEST_CASE("Released consumer generations retain bounded table capacity", "[renderer][material]") {
    Backend backend;
    MaterialBindingLimits limits;
    limits.maximumGenerations = 1;
    auto table = Table(backend, limits);
    const auto first = table->Publish(Descriptor());
    REQUIRE(first.HasValue());
    auto lease = table->Acquire(first.Value().binding);
    REQUIRE(table->Release(first.Value().binding).HasValue());
    ErrorIs(table->Publish(Descriptor()), MaterialBindingErrors::CapacityExceeded);
    REQUIRE(backend.calls == 1);
    std::move(lease).Value().reset();
    const auto next = table->Publish(Descriptor());
    REQUIRE(next.HasValue());
    REQUIRE(next.Value().binding != first.Value().binding);
}

TEST_CASE("Retained parameter bytes independently apply backpressure", "[renderer][material]") {
    Backend backend;
    MaterialBindingLimits limits;
    limits.maximumParameterBytes = 12;
    limits.maximumRetainedParameterBytes = 12;
    auto table = Table(backend, limits);
    REQUIRE(table->Publish(Descriptor()).HasValue());
    ErrorIs(table->Publish(Descriptor()), MaterialBindingErrors::CapacityExceeded);
    REQUIRE(backend.calls == 1);
}

TEST_CASE("Only an authored compatible optional fallback handles unsupported realization", "[renderer][material]") {
    Backend backend;
    auto table = Table(backend);
    auto good = table->Publish(Descriptor());
    REQUIRE(good.HasValue());
    auto descriptor = Descriptor();
    descriptor.required = false;
    descriptor.authoredFallback = good.Value().binding;
    backend.failure = MakeError(MaterialBindingErrors::Unsupported);
    auto fallback = table->Publish(descriptor);
    REQUIRE(fallback.HasValue());
    REQUIRE(fallback.Value().usedFallback);
    REQUIRE(fallback.Value().binding == good.Value().binding);
    REQUIRE(fallback.Value().fallbackReason.has_value());
    REQUIRE(table->Snapshot().retainedGenerations == 1);
    descriptor.required = true;
    ErrorIs(table->Publish(descriptor), MaterialBindingErrors::InvalidDescriptor);
}

TEST_CASE("Fallback never hides stale incompatible or non-unsupported failures", "[renderer][material]") {
    Backend backend;
    auto table = Table(backend);
    auto good = table->Publish(Descriptor());
    REQUIRE(good.HasValue());
    auto descriptor = Descriptor();
    descriptor.required = false;
    descriptor.authoredFallback = good.Value().binding;
    SECTION("stale") {
        REQUIRE(table->Release(good.Value().binding).HasValue());
        ErrorIs(table->Publish(descriptor), MaterialBindingErrors::StaleBinding);
    }
    SECTION("layout mismatch") {
        descriptor.layout.targetBindings[0].nativeBinding = 7;
        ErrorIs(table->Publish(descriptor), MaterialBindingErrors::InvalidDescriptor);
    }
    SECTION("original backend failure") {
        backend.failure = MakeError(MaterialBindingErrors::CapacityExceeded);
        ErrorIs(table->Publish(descriptor), MaterialBindingErrors::CapacityExceeded);
    }
}

TEST_CASE("Backend failure preserves existing generation without partial publication", "[renderer][material]") {
    Backend backend;
    auto table = Table(backend);
    auto good = table->Publish(Descriptor());
    REQUIRE(good.HasValue());
    SECTION("typed failure") {
        backend.failure = MakeError(MaterialBindingErrors::Unsupported);
        ErrorIs(table->Publish(Descriptor()), MaterialBindingErrors::Unsupported);
    }
    SECTION("exception") {
        backend.throws = true;
        ErrorIs(table->Publish(Descriptor()), MaterialBindingErrors::BackendFailure);
    }
    SECTION("empty lease") {
        backend.empty = true;
        ErrorIs(table->Publish(Descriptor()), MaterialBindingErrors::BackendFailure);
    }
    REQUIRE(table->Acquire(good.Value().binding).HasValue());
    REQUIRE(backend.live == 1);
    REQUIRE(table->Snapshot().retainedGenerations == 1);
}

TEST_CASE("Shutdown stops discovery but consumer generations outlive their table", "[renderer][material]") {
    Backend backend;
    auto table = Table(backend);
    auto good = table->Publish(Descriptor());
    REQUIRE(good.HasValue());
    auto lease = table->Acquire(good.Value().binding);
    REQUIRE(table->Shutdown().HasValue());
    REQUIRE(table->Shutdown().HasValue());
    ErrorIs(table->Publish(Descriptor()), MaterialBindingErrors::Closed);
    ErrorIs(table->Acquire(good.Value().binding), MaterialBindingErrors::Closed);
    table.reset();
    REQUIRE(lease.Value()->Descriptor().material.value == 4);
    REQUIRE(backend.live == 1);
    std::move(lease).Value().reset();
    REQUIRE(backend.live == 0);
}

TEST_CASE("Material table rejects cross-thread publication before touching backend", "[renderer][material]") {
    Backend backend;
    auto table = Table(backend);
    std::optional<Result<MaterialBindingSelection>> outcome;
    std::thread worker([&] {
        outcome = table->Publish(Descriptor());
    });
    worker.join();
    REQUIRE(outcome.has_value());
    ErrorIs(*outcome, MaterialBindingErrors::WrongThread);
    REQUIRE(backend.calls == 0);
}

TEST_CASE("Unsupported adapter and invalid finite bounds have typed outcomes", "[renderer][material]") {
    IMaterialBindingBackend unsupported;
    ErrorIs(unsupported.Realize(Descriptor()), MaterialBindingErrors::Unsupported);
    Backend backend;
    MaterialBindingLimits limits;
    limits.maximumGenerations = 0;
    ErrorIs(MaterialBindingTable::Create(Owner, backend, limits), MaterialBindingErrors::InvalidDescriptor);
    ErrorIs(MaterialBindingTable::Create({}, backend), MaterialBindingErrors::InvalidDescriptor);
}

TEST_CASE("Resource slices and sampler bindings preserve typed ownership", "[renderer][material]") {
    auto descriptor = Descriptor();
    descriptor.layout.parameters.clear();
    descriptor.resources[0].value = MaterialBufferBinding{{Owner, 7, 2}, 8, 12};
    REQUIRE(ValidateMaterialBindingDescriptor(descriptor, Owner).HasValue());
    std::get<MaterialBufferBinding>(descriptor.resources[0].value).offset = std::numeric_limits<std::size_t>::max();
    ErrorIs(ValidateMaterialBindingDescriptor(descriptor, Owner), MaterialBindingErrors::InvalidDescriptor);
    descriptor.layout.resources[0].kind = ShaderResourceKind::Sampler;
    descriptor.resources[0].value = RenderSamplerHandle{Owner, 8, 3};
    REQUIRE(ValidateMaterialBindingDescriptor(descriptor, Owner).HasValue());
}

TEST_CASE("Stale foreign and shutdown cross-thread identities never mutate resident leases", "[renderer][material]") {
    Backend backend;
    auto table = Table(backend);
    auto good = table->Publish(Descriptor());
    REQUIRE(good.HasValue());
    auto foreign = good.Value().binding;
    foreign.owner = {99};
    ErrorIs(table->Acquire(foreign), MaterialBindingErrors::StaleBinding);
    ErrorIs(table->Release(foreign), MaterialBindingErrors::StaleBinding);
    std::optional<Result<void>> stopped;
    std::thread worker([&] {
        stopped = table->Shutdown();
    });
    worker.join();
    REQUIRE(stopped.has_value());
    ErrorIs(*stopped, MaterialBindingErrors::WrongThread);
    REQUIRE(table->Acquire(good.Value().binding).HasValue());
}

TEST_CASE("Canonical material packing transposes matrix arrays from typed row-major components", "[renderer][material]") {
    auto layout = Descriptor().layout;
    layout.parameters = {{{1}, {1}, ShaderValueType::Float32, 2, 3, 2, 16, 64, 16, true, true}};
    const std::array<float, 12> components{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const std::array values{MaterialParameterValue{{1}, std::span<const float>{components}}};
    REQUIRE(ComputeMaterialParameterBufferByteSize(layout, {1}).Value() == 120);
    auto packed = PackMaterialParameterBinding(layout, {1}, values);
    REQUIRE(packed.HasValue());
    const auto floatAt = [&](std::size_t offset) {
        REQUIRE(offset <= packed.Value().bytes.size() - 4);
        std::array<std::byte, 4> bytes;
        std::copy_n(packed.Value().bytes.begin() + offset, 4, bytes.begin());
        return std::bit_cast<float>(bytes);
    };
    REQUIRE(floatAt(16) == 1);
    REQUIRE(floatAt(20) == 4);
    REQUIRE(floatAt(32) == 2);
    REQUIRE(floatAt(52) == 6);
    REQUIRE(floatAt(80) == 7);
    REQUIRE(floatAt(116) == 12);
    REQUIRE(floatAt(24) == 0);
    SECTION("row-major reflected target") {
        layout.parameters[0].columnMajor = false;
        auto rowMajor = PackMaterialParameterBinding(layout, {1}, values);
        REQUIRE(rowMajor.HasValue());
        REQUIRE(rowMajor.Value().bytes.size() == 108);
        const auto second = std::bit_cast<std::array<std::byte, 4>>(2.0F);
        REQUIRE(std::equal(second.begin(), second.end(), rowMajor.Value().bytes.begin() + 20));
    }
}

TEST_CASE("Canonical material packing rejects incomplete, aliased and mismatched values", "[renderer][material]") {
    auto layout = Descriptor().layout;
    const std::array<float, 3> components{1, 2, 3};
    std::vector values{MaterialParameterValue{{1}, std::span<const float>{components}}};
    SECTION("missing") {
        values.clear();
    }
    SECTION("duplicate") {
        values.push_back(values.front());
    }
    SECTION("unknown identity") {
        values[0].parameter = {2};
    }
    SECTION("short components") {
        values[0].components = std::span<const float>{components}.first(2);
    }
    SECTION("wrong scalar type") {
        layout.parameters[0].type = ShaderValueType::Int32;
    }
    SECTION("overlapping parameters") {
        auto overlap = layout.parameters[0];
        overlap.id = {2};
        layout.parameters.push_back(overlap);
        values.push_back({{2}, std::span<const float>{components}});
    }
    SECTION("overflowing array stride") {
        layout.parameters[0].arrayCount = 2;
        layout.parameters[0].arrayStride = std::numeric_limits<std::uint32_t>::max();
    }
    ErrorIs(PackMaterialParameterBinding(layout, {1}, values), MaterialBindingErrors::InvalidDescriptor);
}

TEST_CASE("Canonical material packing preserves integer and Bool32 scalar representations", "[renderer][material]") {
    auto layout = Descriptor().layout;
    layout.parameters[0].columns = 1;
    const std::array<std::int32_t, 1> integer{-17};
    const std::array<std::uint32_t, 1> unsignedInteger{0xFEDCBA98};
    const std::array<bool, 1> boolean{true};
    MaterialParameterValue value{{1}, std::span<const std::int32_t>{integer}};
    std::uint32_t expected = std::bit_cast<std::uint32_t>(integer[0]);
    layout.parameters[0].type = ShaderValueType::Int32;
    SECTION("unsigned") {
        value.components = std::span<const std::uint32_t>{unsignedInteger};
        expected = unsignedInteger[0];
        layout.parameters[0].type = ShaderValueType::Uint32;
    }
    SECTION("boolean") {
        value.components = std::span<const bool>{boolean};
        expected = 1;
        layout.parameters[0].type = ShaderValueType::Bool32;
    }
    auto packed = PackMaterialParameterBinding(layout, {1}, std::span{&value, 1});
    REQUIRE(packed.HasValue());
    const auto bytes = std::bit_cast<std::array<std::byte, 4>>(expected);
    REQUIRE(std::equal(bytes.begin(), bytes.end(), packed.Value().bytes.begin()));
}

TEST_CASE("Canonical matrix packing covers the last of 32 views independently of object transform and view control",
          "[renderer][material][matrix]") {
    auto layout = Descriptor().layout;
    layout.parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 32, 0, 64, 16, true, true},
                         {{2}, {1}, ShaderValueType::Float32, 4, 4, 1, 2048, 0, 16, true, true},
                         {{3}, {1}, ShaderValueType::Uint32, 1, 1, 32, 2112, 16, 0, true, true},
                         {{4}, {1}, ShaderValueType::Uint32, 1, 1, 1, 2624, 0, 0, true, true}};
    std::array<float, 512> worldToClip{};
    for (std::size_t index = 0; index < worldToClip.size(); ++index)
        worldToClip[index] = static_cast<float>(index + 100);
    const std::array<float, 16> localToWorld{2, 0, 0, 1, 0, 3, 0, 2, 0, 0, 4, 3, 0, 0, 0, 1};
    std::array<std::uint32_t, 32> viewLayers{};
    for (std::size_t index = 0; index < viewLayers.size(); ++index)
        viewLayers[index] = static_cast<std::uint32_t>((index + 7) % 32);
    const std::array<std::uint32_t, 1> viewControl{17};
    const std::array values{MaterialParameterValue{{1}, std::span<const float>{worldToClip}},
                            MaterialParameterValue{{2}, std::span<const float>{localToWorld}},
                            MaterialParameterValue{{3}, std::span<const std::uint32_t>{viewLayers}},
                            MaterialParameterValue{{4}, std::span<const std::uint32_t>{viewControl}}};
    auto packed = PackMaterialParameterBinding(layout, {1}, values);
    REQUIRE(packed.HasValue());
    REQUIRE(packed.Value().bytes.size() == 2628);
    const auto wordAt = [&](std::size_t offset) {
        REQUIRE(offset <= packed.Value().bytes.size() - 4);
        std::array<std::byte, 4> bytes;
        std::copy_n(packed.Value().bytes.begin() + offset, 4, bytes.begin());
        return std::bit_cast<std::uint32_t>(bytes);
    };
    CHECK(std::bit_cast<float>(wordAt(2040)) == worldToClip[31 * 16 + 2 * 4 + 3]);
    CHECK(std::bit_cast<float>(wordAt(2044)) == worldToClip[511]);
    CHECK(std::bit_cast<float>(wordAt(2096)) == localToWorld[3]);
    CHECK(wordAt(2112) == 7);
    CHECK(wordAt(2116) == 0);
    CHECK(wordAt(2608) == 6);
    CHECK(wordAt(2624) == 17);
    CHECK(wordAt(2612) == 0);
}

TEST_CASE("Canonical matrix array packing rejects malformed strides and truncated tail components", "[renderer][material][matrix]") {
    auto layout = Descriptor().layout;
    layout.parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 32, 0, 64, 16, true, true}};
    const std::array<float, 512> components{};
    MaterialParameterValue value{{1}, std::span<const float>{components}};
    SECTION("matrix stride overlaps columns") {
        layout.parameters[0].matrixStride = 15;
    }
    SECTION("array stride overlaps matrices") {
        layout.parameters[0].arrayStride = 63;
    }
    SECTION("array stride overflows bounded block") {
        layout.parameters[0].arrayStride = std::numeric_limits<std::uint32_t>::max();
    }
    SECTION("last matrix component missing") {
        value.components = std::span<const float>{components}.first(511);
    }
    SECTION("block exceeds canonical byte limit") {
        layout.parameters[0].byteOffset = 65536 - 2047;
    }
    ErrorIs(PackMaterialParameterBinding(layout, {1}, std::span{&value, 1}), MaterialBindingErrors::InvalidDescriptor);
}
