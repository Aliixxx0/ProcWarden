#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cli.hpp"
#include "pw/config.hpp"

namespace pw {
namespace {

std::string which(const std::string& name) {
    const std::string cmd = "command -v " + name + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe == nullptr) return std::string();
    char buf[1024] = {};
    std::string out;
    if (std::fgets(buf, sizeof(buf), pipe) != nullptr) out = buf;
    pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

std::string xml_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else
            out += c;
    }
    return out;
}

}  // namespace

int cmd_install(const std::vector<std::string>& args, const std::string& self_path) {
    namespace fs = std::filesystem;

    bool no_start = false;
    for (const auto& arg : args) {
        if (arg == "--no-start")
            no_start = true;
        else {
            std::fprintf(stderr, "procwarden install: unknown option %s\n", arg.c_str());
            return 2;
        }
    }

    std::string self = self_path;
    if (self.find('/') == std::string::npos) self = which(self_path);
    fs::path daemon;
    if (!self.empty()) daemon = fs::path(self).parent_path() / "procwardend";
    if (daemon.empty() || !fs::exists(daemon)) {
        const std::string found = which("procwardend");
        if (!found.empty()) daemon = found;
    }
    if (daemon.empty() || !fs::exists(daemon)) {
        std::fprintf(stderr,
                     "procwarden install: cannot find procwardend next to procwarden\n"
                     "build first: cmake --build --preset release\n");
        return 1;
    }

    const std::string home = procwarden_home();
    std::error_code ec;
    fs::create_directories(home, ec);
    const fs::path agents = fs::path(home_dir()) / "Library" / "LaunchAgents";
    fs::create_directories(agents, ec);
    if (ec) {
        std::fprintf(stderr, "procwarden install: %s\n", ec.message().c_str());
        return 1;
    }

    const fs::path plist = agents / "com.procwarden.daemon.plist";
    const std::string log_path = home + "/warden.log";
    const std::string out_path = home + "/daemon.out";
    const std::string err_path = home + "/daemon.err";

    std::string xml;
    xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml += "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    xml += "<plist version=\"1.0\"><dict>\n";
    xml += "  <key>Label</key><string>com.procwarden.daemon</string>\n";
    xml += "  <key>ProgramArguments</key><array>\n";
    xml += "    <string>" + xml_escape(daemon.string()) + "</string>\n";
    xml += "    <string>--log</string>\n";
    xml += "    <string>" + xml_escape(log_path) + "</string>\n";
    xml += "  </array>\n";
    xml += "  <key>RunAtLoad</key><true/>\n";
    xml += "  <key>KeepAlive</key><true/>\n";
    xml += "  <key>StandardOutPath</key><string>" + xml_escape(out_path) + "</string>\n";
    xml += "  <key>StandardErrorPath</key><string>" + xml_escape(err_path) + "</string>\n";
    xml += "</dict></plist>\n";

    {
        std::ofstream out(plist, std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "procwarden install: cannot write %s\n", plist.c_str());
            return 1;
        }
        out << xml;
    }
    chmod(plist.c_str(), 0644);

    if (!no_start) {
        const std::string uid = std::to_string(getuid());
        const std::string bootstrap = "launchctl bootstrap gui/" + uid + " '" + plist.string() +
                                      "' 2>/dev/null";
        if (std::system(bootstrap.c_str()) != 0) {
            const std::string load =
                "launchctl load -w '" + plist.string() + "' 2>/dev/null";
            if (std::system(load.c_str()) != 0)
                std::fprintf(stderr,
                             "procwarden install: launchctl failed; load manually with:\n"
                             "  launchctl bootstrap gui/%s '%s'\n",
                             uid.c_str(), plist.c_str());
        }
    }

    nlohmann::json editors;
    for (const char* name : {"code", "cursor"}) {
        const std::string found = which(name);
        if (!found.empty()) editors[name] = found;
    }

    nlohmann::json record;
    record["daemon"] = daemon.string();
    record["plist"] = plist.string();
    record["editors"] = editors;
    {
        std::ofstream out(home + "/install.json", std::ios::trunc);
        if (out) out << record.dump(2) << "\n";
    }

    std::printf("installed LaunchAgent %s\n", plist.c_str());
    std::printf("daemon: %s\n", daemon.c_str());
    if (editors.empty()) {
        std::printf("editors: none detected (code/cursor CLI not on PATH)\n");
    } else {
        std::printf("editors: %s\n", editors.dump().c_str());
    }
    std::printf("next: procwarden init && procwarden run -- your-agent ...\n");
    return 0;
}

}  // namespace pw
