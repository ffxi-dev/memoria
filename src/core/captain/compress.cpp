#include "core/captain/compress.h"

#include <zstd.h>

#include <fstream>
#include <memory>
#include <vector>

namespace memoria::captain
{

auto compressLog(const std::filesystem::path& log, const std::filesystem::path& out, const std::filesystem::path& head, const std::stop_token& stop) -> std::optional<std::string>
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    auto       tmp  = fs::path(out).concat(".tmp");
    const auto fail = [&](std::string why)
    {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return std::optional(std::move(why));
    };

    const std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> cctx(ZSTD_createCCtx(), &ZSTD_freeCCtx);
    ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_compressionLevel, 3);
    std::ifstream     in(log, std::ios::binary);
    std::vector<char> inBuf(ZSTD_CStreamInSize());
    std::vector<char> outBuf(ZSTD_CStreamOutSize());
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!in || !o)
        {
            return fail("cannot open it");
        }
        if (!head.empty())
        {
            const std::ifstream h(head, std::ios::binary);
            if (!h || !(o << h.rdbuf()))
            {
                o.close();
                return fail("cannot copy " + head.string());
            }
        }
        for (bool last = false; !last;)
        {
            if (stop.stop_requested())
            {
                o.close();
                return fail("stopped");
            }
            in.read(inBuf.data(), static_cast<std::streamsize>(inBuf.size()));
            if (in.bad())
            {
                o.close();
                return fail("cannot read it");
            }
            last = in.eof();
            ZSTD_inBuffer input{ .src = inBuf.data(), .size = static_cast<size_t>(in.gcount()), .pos = 0 };
            for (bool done = false; !done;)
            {
                ZSTD_outBuffer output{ .dst = outBuf.data(), .size = outBuf.size(), .pos = 0 };
                const auto     remaining = ZSTD_compressStream2(cctx.get(), &output, &input, last ? ZSTD_e_end : ZSTD_e_continue);
                if (ZSTD_isError(remaining))
                {
                    o.close();
                    return fail(ZSTD_getErrorName(remaining));
                }
                o.write(outBuf.data(), static_cast<std::streamsize>(output.pos));
                done = last ? remaining == 0 : input.pos == input.size;
            }
        }
        o.close();
        if (!o)
        {
            return fail("cannot write " + tmp.string());
        }
    }
    fs::rename(tmp, out, ec);
    if (ec)
    {
        return fail(ec.message());
    }
    return std::nullopt;
}

} // namespace memoria::captain
