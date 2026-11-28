#include "pw/os/fsize.hpp"

#include <fts.h>
#include <sys/stat.h>

#include <cerrno>

namespace pw {

// fts: physical walk, no chdir; unreadable entries are skipped.
Result<uint64_t> dir_size(const std::string& path) {
    char* paths[2] = {const_cast<char*>(path.c_str()), nullptr};
    FTS* fts = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, nullptr);
    if (fts == nullptr) return Result<uint64_t>::err(errno, "fts_open(" + path + ")");

    uint64_t total = 0;
    while (FTSENT* ent = fts_read(fts)) {
        switch (ent->fts_info) {
            case FTS_F:
                total += static_cast<uint64_t>(ent->fts_statp->st_size);
                break;
            case FTS_DP:
            case FTS_D:
            case FTS_DNR:
            case FTS_ERR:
            case FTS_NS:
            case FTS_SL:
            case FTS_SLNONE:
                break;
            default:
                break;
        }
    }
    fts_close(fts);
    return total;
}

}  // namespace pw
