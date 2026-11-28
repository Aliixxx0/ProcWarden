#include <sys/stat.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cli.hpp"
#include "pw/config.hpp"

namespace pw {
namespace {

bool write_file(const std::string& path, const std::string& content, bool overwrite) {
    if (!overwrite && std::filesystem::exists(path)) return true;
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << content;
    return out.good();
}

void ensure_gitignore(const std::string& project, bool shared) {
    const std::string path = project + "/.gitignore";
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }

    const std::string wanted = shared ? ".procwarden/history/" : ".procwarden/";
    for (const auto& line : lines)
        if (line == wanted) return;

    std::ofstream out(path, std::ios::app);
    if (!out) return;
    if (!lines.empty() && !lines.back().empty()) out << "\n";
    out << "# ProcWarden\n" << wanted << "\n";
}

nlohmann::json default_rules() {
    nlohmann::json j;
    j["phase_rules"] = nlohmann::json::array(
        {{{"pattern", "opencode.*(plan|Plan)|--phase[= ]?Plan"}, {"phase", "Plan"}},
         {{"pattern", "tsc|vite build|next build|webpack|esbuild|--phase[= ]?Build"},
          {"phase", "Build"}},
         {{"pattern", "bun test|vitest|jest|pytest|pw-sim|--phase[= ]?Test"},
          {"phase", "Test"}},
         {{"pattern", "review|git diff|--phase[= ]?Review"}, {"phase", "Review"}}});
    j["caches"] = nlohmann::json::array(
        {{{"label", "npm"}, {"path", "~/.npm/_cacache"}, {"command", "npm cache clean --force"}},
         {{"label", "pnpm"},
          {"path", "~/Library/pnpm/store"},
          {"command", "pnpm store prune"}},
         {{"label", "bun"}, {"path", "~/.bun/install/cache"}, {"command", "bun pm cache rm"}},
         {{"label", "biome"}, {"path", "~/Library/Caches/com.biomejs.biome"}, {"command", ""}}});
    return j;
}

}  // namespace

int cmd_init(const std::vector<std::string>& args) {
    namespace fs = std::filesystem;

    bool shared = false;
    bool force = false;
    bool no_gitignore = false;
    std::string project = fs::current_path().string();

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--shared") {
            shared = true;
        } else if (arg == "--force") {
            force = true;
        } else if (arg == "--no-gitignore") {
            no_gitignore = true;
        } else if (arg == "--project" && i + 1 < args.size()) {
            project = args[++i];
        } else {
            std::fprintf(stderr, "procwarden init: unknown option %s\n", arg.c_str());
            return 2;
        }
    }

    const fs::path dir = fs::path(project) / ".procwarden";
    std::error_code ec;
    fs::create_directories(dir / "history", ec);
    if (ec) {
        std::fprintf(stderr, "procwarden init: %s\n", ec.message().c_str());
        return 1;
    }

    Config cfg = Config::defaults();
    if (auto loaded = Config::load(project); loaded.ok()) cfg = loaded.value();
    cfg.storage = shared ? Storage::Shared : Storage::Local;

    if (!write_file((dir / "settings.json").string(), cfg.to_json().dump(2) + "\n", force)) {
        std::fprintf(stderr, "procwarden init: cannot write settings.json\n");
        return 1;
    }
    if (!write_file((dir / "sessions.json").string(),
                    nlohmann::json{{"version", 1}, {"sessions", nlohmann::json::array()}}
                            .dump(2) +
                        "\n",
                    force)) {
        std::fprintf(stderr, "procwarden init: cannot write sessions.json\n");
        return 1;
    }
    if (!write_file((dir / "cleanup-rules.json").string(), default_rules().dump(2) + "\n",
                    force)) {
        std::fprintf(stderr, "procwarden init: cannot write cleanup-rules.json\n");
        return 1;
    }

    if (!no_gitignore) ensure_gitignore(project, shared);

    std::printf("initialized %s (%s storage)\n", dir.string().c_str(),
                shared ? "shared" : "local");
    std::printf(
        "next:\n"
        "  procwarden install\n"
        "  procwarden run -- your-agent ...\n"
        "  procwarden watch\n");
    return 0;
}

}  // namespace pw
