#include "admin.hpp"
#include "tui.hpp"
#include <cstdlib>
#include <ctime>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <pwd.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
const char* help = R"(4agent — temporary Linux accounts for AI agents

  sudo 4agent                         Open terminal UI
  sudo 4agent --create --name agent_demo --minutes 60 [--root]
              [--public-ip ADDRESS] [--port 22] [--output DIRECTORY]
  sudo 4agent --list
  sudo 4agent --history agent_demo
  sudo 4agent --revoke agent_demo
  sudo 4agent --reap                   Used by the expiration timer

Default access: non-root. --root grants unrestricted passwordless sudo.
Lifetime: 1–525600 minutes. IP: auto-detected via HTTPS unless supplied.
Port: 22 by default; set it to your externally reachable SSH port.
Export: DIRECTORY/4agent-(hostname).txt, root-owned mode 0600.
Default export directory: /var/lib/4agent/exports (independent of working directory).
Install and enable 4agent-expire.timer before creating accounts.
)";
// Display untrusted history as text, never terminal control sequences.
std::string safe_text(const std::string& s) {
    std::string out;
    for(unsigned char c : s) {
        if(c == '\n' || c == '\t' || (c >= 32 && c < 127)) out += static_cast<char>(c);
        else { const char* hex="0123456789abcdef"; out += "\\x"; out += hex[c>>4]; out += hex[c&15]; }
    }
    return out;
}
void list(agent::Admin& admin) {
    auto accounts = admin.accounts();
    if(accounts.empty()) std::cout << "No active managed accounts.\n";
    for(const auto& a : accounts)
        std::cout << a.name << "  uid=" << a.uid << "  " << (a.root ? "sudo root" : "non-root")
                  << "  expires " << agent::utc(a.expires) << '\n';
}
void session() {
    // This runs as the SSH user, never through sudo, and has no administrative privileges.
    auto pw = getpwuid(getuid());
    if(getuid() == 0 || geteuid() != getuid() || !pw || !agent::valid_name(pw->pw_name))
        throw std::runtime_error("Session helper requires a temporary non-root login UID");
    std::string name = pw->pw_name;
    std::string home = pw->pw_dir;
    if(home != "/home/"+name) throw std::runtime_error("Unexpected session home");
    auto command = std::getenv("SSH_ORIGINAL_COMMAND");
    if(command && *command) {
        int fd = open((home+"/.bash_history").c_str(), O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
        if(fd < 0) throw std::runtime_error("Cannot open SSH command history");
        struct stat st{};
        if(fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_nlink != 1 || flock(fd, LOCK_EX)) {
            close(fd); throw std::runtime_error("Unsafe SSH command history file");
        }
        std::string entry = "#"+std::to_string(std::time(nullptr))+"\n"+command+"\n";
        std::size_t pos=0;
        while(pos < entry.size()) {
            auto n = write(fd, entry.data()+pos, entry.size()-pos);
            if(n < 0 && errno == EINTR) continue;
            if(n <= 0) { close(fd); throw std::runtime_error("Cannot save SSH command history"); }
            pos += static_cast<std::size_t>(n);
        }
        if(fsync(fd)) { close(fd); throw std::runtime_error("Cannot sync SSH command history"); }
        close(fd);
        execl("/bin/bash", "bash", "-c", command, static_cast<char*>(nullptr));
    } else execl("/bin/bash", "-bash", static_cast<char*>(nullptr));
    throw std::runtime_error("Cannot start account shell");
}
}
int main(int argc, char** argv) {
    try {
        // Root helpers must not resolve binaries from caller-controlled PATH entries.
        if(!(argc == 2 && std::string(argv[1]) == "--session")) {
            setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
            setenv("LC_ALL", "C", 1);
        }
        if(argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) { std::cout << help; return 0; }
        if(argc == 1) { agent::tui(); return 0; }
        std::string action = argv[1];
        if(action == "--session" && argc == 2) { session(); }
        else if(action == "--create") {
            agent::Options o;
            for(int i=2; i<argc; ++i) {
                std::string flag = argv[i];
                if(flag == "--root") { o.root = true; continue; }
                if(i+1 >= argc) throw std::runtime_error("Missing value for " + flag);
                std::string value = argv[++i];
                if(flag == "--name") o.name = value;
                else if(flag == "--minutes") o.minutes = agent::number(value,1,525600);
                else if(flag == "--public-ip") o.ip = value;
                else if(flag == "--port") o.port = agent::number(value,1,65535);
                else if(flag == "--output") o.output = value;
                else throw std::runtime_error("Unknown option: " + flag);
            }
            agent::Admin admin;
            auto path = admin.create(o);
            std::cout << "Created " << o.name << ". Credentials: " << safe_text(path.string()) << '\n';
        } else if(action == "--list" && argc == 2) { agent::Admin admin; list(admin); }
        else if(action == "--reap" && argc == 2) { agent::Admin admin; admin.reap(); }
        else if(action == "--revoke" && argc == 3) { agent::Admin admin; admin.revoke(argv[2]); std::cout << "Account revoked.\n"; }
        else if(action == "--history" && argc == 3) { agent::Admin admin; std::cout << safe_text(admin.history(argv[2])); }
        else throw std::runtime_error("Invalid command. Run 4agent --help");
        return 0;
    } catch(const std::exception& e) { std::cerr << "4agent: " << safe_text(e.what()) << '\n'; return 1; }
}
