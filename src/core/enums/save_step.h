#pragma once

#include <cstdint>

namespace memoria
{

// The steps of replaceFile that tests can fail.
enum class SaveStep : uint8_t
{
    Flush,       // the file's bytes to disk
    FolderFlush, // the rename to disk (POSIX; Windows writes it through with the rename)
};

} // namespace memoria
