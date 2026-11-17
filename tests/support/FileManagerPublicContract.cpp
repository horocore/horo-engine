#include "Horo/Platform/FileManager.h"

#include <type_traits>

static_assert(std::is_same_v<decltype(Horo::RevealInFileManager(std::filesystem::path{})), Horo::Result<void>>);
