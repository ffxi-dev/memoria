#include "core/upload/upload_state.h"

#include "core/file.h"
#include "core/units.h"

#include <format>
#include <fstream>
#include <sstream>

namespace memoria::upload
{
namespace
{

auto stateOf(const Stage stage) -> std::string_view
{
    switch (stage)
    {
        case Stage::Closed:
            return "1";
        case Stage::Ended:
            return "ended";
        case Stage::CutOff:
            return "cut_off";
        case Stage::Rescued:
            return "rest";
        case Stage::Open:
            break;
    }
    return "0";
}

auto stageOf(const std::string_view state) -> Stage
{
    for (const auto stage : { Stage::Closed, Stage::Ended, Stage::CutOff, Stage::Rescued })
    {
        if (state == stateOf(stage))
        {
            return stage;
        }
    }
    return Stage::Open;
}

} // namespace

// One line per file, tab separated: name, url, state, refused size, bytes the server holds, and the refusal's reason.
auto readUploads(const std::filesystem::path& path) -> UploadFiles
{
    UploadFiles   files;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);)
    {
        std::istringstream fields(line);
        std::string        name;
        std::string        url;
        std::string        state;
        std::string        refused;
        std::string        sent;
        std::string        reason;
        if (std::getline(fields, name, '\t') && std::getline(fields, url, '\t') && std::getline(fields, state, '\t') && std::getline(fields, refused, '\t') &&
            std::getline(fields, sent, '\t'))
        {
            std::getline(fields, reason); // empty unless refused
            files[name] = {
                .url       = url,
                .stage     = stageOf(state),
                .refusedAt = parseU64(refused).value_or(0),
                .sent      = parseU64(sent).value_or(0),
                .reason    = reason,
            };
        }
    }
    return files;
}

auto watch(UploadFile& f, const uint64_t size, const std::chrono::steady_clock::time_point now) -> void
{
    if (size != f.size || f.seen == std::chrono::steady_clock::time_point{})
    {
        f.size = size;
        f.seen = now;
    }
}

auto writeUploads(const std::filesystem::path& path, const UploadFiles& files) -> std::optional<std::string>
{
    std::string text;
    for (const auto& [name, f] : files)
    {
        text += std::format("{}\t{}\t{}\t{}\t{}\t{}\n", name, f.url, stateOf(f.stage), f.refusedAt, f.sent, f.reason);
    }
    return replaceFile(path, text);
}

} // namespace memoria::upload
