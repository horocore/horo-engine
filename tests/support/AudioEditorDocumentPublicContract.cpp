#include "Horo/Editor/AudioComponentPreview.h"
#include "Horo/Editor/AudioEditorDocument.h"

#include <type_traits>

static_assert(std::is_move_constructible_v<Horo::Editor::AudioEditorDocument>);
static_assert(!std::is_copy_constructible_v<Horo::Editor::AudioEditorDocument>);
static_assert(std::is_copy_constructible_v<Horo::Editor::AudioEditorDocumentSnapshot>);

static_assert(std::is_abstract_v<Horo::Editor::IAudioComponentPreview>);
static_assert(std::is_copy_constructible_v<Horo::Editor::AudioComponentPreviewRequest>);
static_assert(std::is_copy_constructible_v<Horo::Editor::AudioComponentPreviewSnapshot>);
