#include "core/upload/upload_state.h"

#include "core/file.h"
#include "core/tus.h"

#include <format>
#include <fstream>
#include <sstream>

namespace memoria::upload
{
namespace
{

// The state column: 1 closed, 0 open, cut_off, rest once a cut-off tail went up on its own, or ended when refused.
auto stateOf(const UploadFile& f) -> std::string_view
{
    if (f.closed)
    {
        return "1";
    }
    if (f.rescued)
    {
        return "rest";
    }
    if (f.cutOff)
    {
        return "cut_off";
    }
    if (f.ended)
    {
        return "ended";
    }
    return "0";
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
        if (std::getline(fields, name, '\t') && std::getline(fields, url, '\t') && std::getline(fields, state, '\t') && std::getline(fields, refused, '\t'))
        {
            // 0.5 wrote neither of the last two.
            std::getline(fields, sent, '\t');
            std::getline(fields, reason);
            files[name] = {
                .url     = url,
                .closed  = state == "1",
                .ended   = state == "ended" || state == "cut_off" || state == "rest",
                .cutOff  = state == "cut_off" || state == "rest",
                .rescued = state == "rest",
                .refused = parseU64(refused).value_or(0),
                .sent    = parseU64(sent).value_or(0),
                .reason  = reason,
            };
        }
    }
    return files;
}

auto writeUploads(const std::filesystem::path& path, const UploadFiles& files) -> std::optional<std::string>
{
    std::string text;
    for (const auto& [name, f] : files)
    {
        text += std::format("{}\t{}\t{}\t{}\t{}\t{}\n", name, f.url, stateOf(f), f.refused, f.sent, f.reason);
    }
    return replaceFile(path, text);
}

} // namespace memoria::upload
