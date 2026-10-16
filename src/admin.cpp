#include "admin.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <pwd.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace agent {
namespace fs = std::filesystem;
namespace {
const fs::path state = "/var/lib/4agent";
struct FD {
    int n;
    explicit FD(int value) : n(value) { if(n < 0) throw std::runtime_error(std::strerror(errno)); }
    ~FD() { close(n); }
    FD(const FD&) = delete;
    FD& operator=(const FD&) = delete;
};
void write_all(int fd, const std::string& text) {
    std::size_t offset = 0;
    while(offset < text.size()) {
        auto n = write(fd, text.data()+offset, text.size()-offset);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) throw std::runtime_error("File write failed");
        offset += static_cast<std::size_t>(n);
    }
    if(fsync(fd)) throw std::runtime_error("File sync failed");
}
std::string read_file(const fs::path& path, bool missing_ok = false) {
    int raw = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if(raw < 0 && errno == ENOENT && missing_ok) return {};
    FD fd(raw);
    struct stat st{};
    if(fstat(fd.n, &st) || !S_ISREG(st.st_mode)) throw std::runtime_error("Not a regular file: " + path.string());
    std::string out;
    char buffer[4096];
    for(;;) {
        auto n = read(fd.n, buffer, sizeof(buffer));
        if(n < 0 && errno == EINTR) continue;
        if(n < 0) throw std::runtime_error("Cannot read " + path.string());
        if(n == 0) break;
        out.append(buffer, static_cast<std::size_t>(n));
        if(out.size() > 4*1024*1024) throw std::runtime_error("File exceeds 4 MiB: " + path.string());
    }
    return out;
}
void new_file(const fs::path& path, const std::string& text, mode_t mode = 0600) {
    FD fd(open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode));
    write_all(fd.n, text);
}
void directory(const fs::path& path, mode_t mode) {
    if(mkdir(path.c_str(), mode) && errno != EEXIST) throw std::runtime_error("Cannot create " + path.string());
    struct stat st{};
    if(lstat(path.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0022))
        throw std::runtime_error("Unsafe directory: " + path.string());
    if(chmod(path.c_str(), mode)) throw std::runtime_error("Cannot secure " + path.string());
}
fs::path record(const std::string& name) {
    if(!valid_name(name)) throw std::runtime_error("Username must be agent_ followed by 1–24 lowercase letters, digits, or underscores");
    return state / (name + ".account");
}
class Export {
    FD fd_;
    off_t original_size_{};
    static int open_file(const fs::path& path) {
        int fd = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
        if(fd < 0) throw std::runtime_error("Cannot open export " + path.string() + ": " + std::strerror(errno) +
            ". Choose a Linux directory, for example " + default_export_directory);
        return fd;
    }
public:
    explicit Export(const fs::path& path) : fd_(open_file(path)) {
        if(flock(fd_.n, LOCK_EX)) throw std::runtime_error("Cannot lock export " + path.string());
        struct stat st{};
        if(fstat(fd_.n, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || st.st_nlink != 1 || (st.st_mode & 07777) != 0600)
            throw std::runtime_error("Cannot securely save export " + path.string() +
                ": requires root ownership, mode 0600, and one link. Windows-mounted directories may not enforce these permissions. Choose a Linux directory, for example " + default_export_directory);
        original_size_ = st.st_size;
    }
    void append(const std::string& report) {
        try { write_all(fd_.n, report); }
        catch(...) {
            if(ftruncate(fd_.n, original_size_) || fsync(fd_.n))
                throw std::runtime_error("Export write failed and its partial contents could not be rolled back");
            throw;
        }
    }
};
}

std::string hostname() {
    char buffer[256]{};
    if(gethostname(buffer, sizeof(buffer)-1)) throw std::runtime_error("Cannot read hostname");
    std::string value(buffer);
    for(char& c : value) if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '.')) c = '_';
    return value;
}
std::string detect_ip() {
    for(const auto& url : {"https://api.ipify.org", "https://ifconfig.me/ip"}) {
        auto r = run({"curl", "--fail", "--silent", "--show-error", "--connect-timeout", "3", "--max-time", "5", url});
        while(!r.output.empty() && (r.output.back() == '\n' || r.output.back() == '\r')) r.output.pop_back();
        if(r.code == 0 && valid_ip(r.output)) return r.output;
    }
    throw std::runtime_error("Public IP detection failed. Enter an address in the TUI or use --public-ip");
}
Admin::Admin() {
    if(geteuid() != 0) throw std::runtime_error("Run with sudo: sudo 4agent");
    umask(0077);
    directory(state, 0700);
    FD fd(open((state/"lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
    if(flock(fd.n, LOCK_EX)) throw std::runtime_error("Cannot lock account state");
    lock_ = dup(fd.n);
    if(lock_ < 0) throw std::runtime_error("Cannot retain account lock");
    fcntl(lock_, F_SETFD, FD_CLOEXEC);
}
Admin::~Admin() { if(lock_ >= 0) close(lock_); }
std::vector<Account> Admin::accounts() {
    std::vector<Account> out;
    for(const auto& entry : fs::directory_iterator(state)) {
        if(entry.path().extension() != ".account") continue;
        auto a = deserialize(read_file(entry.path()));
        if(entry.path().stem() != a.name) throw std::runtime_error("Account record name mismatch");
        out.push_back(a);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b){ return a.name < b.name; });
    return out;
}
fs::path Admin::create(Options o) {
    auto metadata = record(o.name);
    if(o.minutes < 1 || o.minutes > 525600 || o.port < 1 || o.port > 65535) throw std::runtime_error("Invalid lifetime or SSH port");
    if(o.ip.empty()) o.ip = detect_ip();
    if(!valid_ip(o.ip)) throw std::runtime_error("Invalid public IPv4 or IPv6 address");
    if(getpwnam(o.name.c_str()) || fs::exists(metadata) || fs::exists(state/(o.name+".history")))
        throw std::runtime_error("Account name already exists or has archived history; choose a new name");
    if(run({"systemctl", "is-active", "--quiet", "4agent-expire.timer"}).code)
        throw std::runtime_error("Expiration timer is inactive. Install 4agent and run: sudo systemctl enable --now 4agent-expire.timer");
    fs::path home = fs::path("/home")/o.name;
    if(fs::symlink_status(home).type() != fs::file_type::not_found)
        throw std::runtime_error("Home path already exists");
    auto output = fs::absolute(o.output)/("4agent-" + hostname() + ".txt");
    if(fs::absolute(o.output).lexically_normal() == fs::path(default_export_directory))
        directory(default_export_directory, 0700);
    if(!fs::is_directory(output.parent_path())) throw std::runtime_error("Export directory does not exist");
    // Validate and retain the same file descriptor before provisioning any account or keys.
    Export export_file(output);
    if(o.root) checked({"visudo", "-c"});
    char temp[] = "/var/lib/4agent/key-XXXXXX";
    if(!mkdtemp(temp)) throw std::runtime_error("Cannot create private key staging directory");
    struct Stage { fs::path p; ~Stage() { std::error_code ec; fs::remove_all(p, ec); } } stage{temp};
    auto key = stage.p/"id_ed25519";
    checked({"ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "4agent:"+o.name+"@"+hostname(), "-f", key.string()});
    auto public_key = read_file(key.string()+".pub");
    auto private_key = read_file(key);
    Account a{o.name, 0, std::time(nullptr) + static_cast<std::int64_t>(o.minutes)*60, o.root};
    bool created = false;
    bool policy_installed = false;
    auto sudoers = fs::path("/etc/sudoers.d")/("4agent-"+o.name);
    try {
        // Hosts may have large or sensitive /etc/skel contents. Provision only our own shell files.
        auto skeleton = stage.p/"skel";
        directory(skeleton, 0700);
        new_file(skeleton/".bashrc", "", 0644);
        new_file(skeleton/".profile", "if [ -n \"$BASH_VERSION\" ] && [ -r \"$HOME/.bashrc\" ]; then\n    . \"$HOME/.bashrc\"\nfi\nPATH=\"$HOME/.local/bin:$HOME/bin:$PATH\"\nexport PATH\n", 0644);
        checked({"useradd", "--create-home", "--home-dir", home.string(), "--shell", "/bin/bash",
                 "--skel", skeleton.string(), "--password", "*", "--expiredate", utc(a.expires+86399, "%Y-%m-%d"), "--", o.name});
        created = true;
        auto pw = getpwnam(o.name.c_str());
        if(!pw || pw->pw_uid < 1000) throw std::runtime_error("Cannot verify new account UID");
        a.uid = pw->pw_uid;
        auto gid = pw->pw_gid;
        // Publish only a complete record so the expiration worker never sees a partial write.
        auto staged_record = stage.p/"account";
        new_file(staged_record, serialize(a));
        if(link(staged_record.c_str(), metadata.c_str())) throw std::runtime_error("Cannot publish managed account record");
        directory(home/".ssh", 0700);
        std::string session_path = AGENT_INSTALL_PATH;
        if(session_path.find_first_not_of("/abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos)
            throw std::runtime_error("Install prefix contains characters unsupported in SSH key options");
        new_file(home/".ssh"/"authorized_keys", "command=\"" + session_path + " --session\",restrict,pty,expiry-time=\"" + utc(a.expires,"%Y%m%d%H%M%SZ") + "\" " + public_key);
        new_file(home/".bash_history", "");
        FD bashrc(open((home/".bashrc").c_str(), O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644));
        write_all(bashrc.n, "\n# 4agent interactive command history (user-editable)\nHISTFILE=$HOME/.bash_history\nHISTSIZE=10000\nHISTFILESIZE=20000\nHISTCONTROL=\nHISTIGNORE=\nHISTTIMEFORMAT='%F %T '\nshopt -s histappend\nPROMPT_COMMAND='history -a'\nhistory -a\n");
        for(const auto& p : {home/".ssh"/"authorized_keys", home/".ssh", home/".bash_history"})
            if(chown(p.c_str(), a.uid, gid)) throw std::runtime_error("Cannot set account file ownership");
        if(o.root) {
            auto staged = stage.p/"sudoers";
            new_file(staged, o.name + " ALL=(ALL:ALL) NOPASSWD: ALL\n", 0440);
            checked({"visudo", "-cf", staged.string()});
            // Exclusive creation preserves existing policies, including on a separate /etc filesystem.
            FD policy(open(sudoers.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0440));
            policy_installed = true;
            write_all(policy.n, read_file(staged));
            if(fchmod(policy.n, 0440)) throw std::runtime_error("Cannot set sudo policy permissions");
        }
        std::string target = o.ip.find(':') == std::string::npos ? o.ip : "["+o.ip+"]";
        std::string local_key = "4agent-"+o.name+".key";
        std::string report = "\n========== 4agent account ==========\nHostname: " + hostname() +
            "\nUsername: " + o.name + "\nPermissions: " + (o.root ? "root via passwordless sudo" : "non-root") +
            "\nExpires: " + utc(a.expires) + "\nServer public IP: " + o.ip + "\nSSH port: " + std::to_string(o.port) +
            "\n\nPublic SSH key:\n" + public_key + "\nPrivate SSH key (SECRET):\n" + private_key +
            "\nConnection instructions (on your client):\n1. Copy the entire private key block, including BEGIN/END lines, into " + local_key +
            ". Keep the final newline.\n2. Run: chmod 600 " + local_key +
            "\n3. Verify the server host-key fingerprint with the server administrator.\n4. Connect: ssh -o IdentitiesOnly=yes -i " + local_key + " -p " + std::to_string(o.port) + " " + o.name + "@" + target +
            (o.root ? "\n5. For a root shell after connecting: sudo -i\n" : "\n") +
            "\nThe server must run OpenSSH, allow public-key authentication, and expose the configured SSH port.\nFor NAT/WSL, configure inbound port forwarding; the public IP alone does not make the server reachable.\nKeys expire at the time above. The timer checks every 15 seconds and removes expired accounts and their running user processes. Scheduling or service failures can delay cleanup.\nSSH command requests and completed interactive Bash commands are saved to ~/.bash_history. Commands inside other shells and root shells are not captured.\nRoot access can disable expiry or create persistent access. History is user-editable and is not an audit log.\nThis export contains an unencrypted private key. Transfer it securely and delete it when no longer needed.\n";
        export_file.append(report);
        return output;
    } catch(...) {
        auto original = std::current_exception();
        if(created) {
            auto result = run({"userdel", "--remove", o.name});
            if(result.code == 0) { std::error_code ec; fs::remove(metadata, ec); }
            else std::cerr << "Rollback could not remove " << o.name << ": " << result.output << '\n';
        }
        std::error_code ec;
        if(policy_installed) fs::remove(sudoers, ec);
        std::rethrow_exception(original);
    }
}
void Admin::revoke(const std::string& name) {
    auto path = record(name);
    auto a = deserialize(read_file(path));
    auto pw = getpwnam(name.c_str());
    if(pw) {
        if(pw->pw_uid != a.uid || a.uid < 1000) throw std::runtime_error("UID mismatch: refusing to touch " + name);
        // Expire PAM logins first; key removal also prevents new key-authenticated sessions.
        checked({"usermod", "--lock", "--expiredate", "1970-01-02", name});
        fs::path home = pw->pw_dir;
        // Only operate on the home created by this program.
        if(home != fs::path("/home")/name) throw std::runtime_error("Unexpected managed home directory");
        // Rename the complete .ssh directory without following user-controlled symlinks.
        std::error_code ec;
        fs::rename(home/".ssh", state/(name+".revoked-ssh"), ec);
        run({"loginctl", "terminate-user", name});
        auto killed = run({"pkill", "-KILL", "-u", std::to_string(a.uid)});
        if(killed.code != 0 && killed.code != 1) throw std::runtime_error("Cannot terminate account processes: " + killed.output);
        // History is best effort: a user may remove it or replace it with a symlink.
        try {
            auto content = read_file(home/".bash_history", true);
            auto archive = state/(name+".history");
            if(!fs::exists(archive)) new_file(archive, content);
        } catch(const std::exception& e) { std::cerr << "History archive: " << e.what() << '\n'; }
        checked({"userdel", "--remove", name});
    }
    fs::remove(fs::path("/etc/sudoers.d")/("4agent-"+name));
    fs::remove_all(state/(name+".revoked-ssh"));
    fs::remove(path);
}
void Admin::reap() {
    bool failed = false;
    for(const auto& a : accounts()) {
        if(a.expires > std::time(nullptr)) continue;
        try { revoke(a.name); std::cout << "Expired " << a.name << '\n'; }
        catch(const std::exception& e) { std::cerr << a.name << ": " << e.what() << '\n'; failed = true; }
    }
    if(failed) throw std::runtime_error("Some accounts could not be removed; the timer will retry");
}
std::string Admin::history(const std::string& name) {
    auto path = record(name);
    if(fs::exists(path)) {
        auto a = deserialize(read_file(path));
        auto pw = getpwnam(name.c_str());
        if(!pw || pw->pw_uid != a.uid) throw std::runtime_error("Account UID mismatch");
        return read_file(fs::path("/home")/name/".bash_history", true);
    }
    return read_file(state/(name+".history"));
}
}
