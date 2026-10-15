#include "DestructionSaveFixture.h"
#include "Horo/Destruction/DestructionSaveCapture.h"

int main() {
    using namespace Horo::Destruction;
    auto source = std::make_shared<SaveTest::Source>();
    auto adapter = DestructionSaveAdapter::Create(SaveTest::Binding(), source);
    if (adapter.HasError())
        return 1;
    Horo::Runtime::CanonicalStateParticipantRegistry registry;
    if (registry.Register(adapter.Value()->Descriptor(), adapter.Value()).HasError())
        return 2;
    auto participants = registry.Snapshot();
    if (participants.HasError())
        return 3;
    auto created =
        Horo::Runtime::RuntimeSaveCaptureBuilder::Create(SaveTest::Provenance(participants.Value().Generation()), participants.Value());
    if (created.HasError())
        return 4;
    auto builder = std::move(created).Value();
    if (builder.CaptureParticipants().HasError())
        return 4;
    auto captured = builder.Seal();
    if (captured.HasError() || captured.Value().Records().size() != 1)
        return 5;
    auto reader = Horo::Runtime::CanonicalValueReader::Create(captured.Value().Records()[0].Segment(0));
    if (reader.HasError())
        return 6;
    auto decoded = DecodeDestructionSaveState(std::move(reader).Value(), {source->runtime.Snapshot().content, source->chunks});
    return decoded.HasValue() && decoded.Value().state == source->State() ? 0 : 7;
}
