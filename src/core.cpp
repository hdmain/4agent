#include "core.hpp"
#include <arpa/inet.h>
#include <array>
#include <charconv>
#include <ctime>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <fcntl.h>

namespace agent {
bool valid_name(const std::string& name) {
    return std::regex_match(name, std::regex("agent_[a-z0-9_]{1,24}"));
}
bool valid_ip(const std::string& address) {
    std::array<unsigned char,16> data{};
    return inet_pton(AF_INET, address.c_str(), data.data()) == 1 ||
           inet_pton(AF_INET6, address.c_str(), data.data()) == 1;
}
int number(const std::string& value, int low, int high) {
    int n = 0;
    auto [p, ec] = std::from_chars(value.data(), value.data()+value.size(), n);
    if(ec != std::errc{} || p != value.data()+value.size() || n < low || n > high)
        throw std::runtime_error("Expected a number from " + std::to_string(low) + " to " + std::to_string(high));
    return n;
}
std::string serialize(const Account& a) {
    return a.name + " " + std::to_string(a.uid) + " " + std::to_string(a.expires) + " " + (a.root ? "1" : "0") + "\n";
}
Account deserialize(const std::string& line) {
    Account a;
    std::uint64_t uid;
    int root;
    std::string extra;
    std::istringstream in(line);
    if(!(in >> a.name >> uid >> a.expires >> root) || (in >> extra) ||
       !valid_name(a.name) || uid < 1000 || uid > std::numeric_limits<std::uint32_t>::max() ||
       a.expires <= 0 || (root != 0 && root != 1))
        throw std::runtime_error("Invalid managed account record");
    a.uid = static_cast<std::uint32_t>(uid);
    a.root = root == 1;
    return a;
}
std::string utc(std::int64_t epoch, const char* format) {
    std::time_t time = epoch;
    std::tm tm{};
    if(!gmtime_r(&time, &tm)) throw std::runtime_error("Invalid expiration time");
    char buffer[100];
    if(!std::strftime(buffer, sizeof(buffer), format, &tm)) throw std::runtime_error("Time formatting failed");
    return buffer;
}
std::string ssh_expiry(std::int64_t epoch) {
    // OpenSSH 8.x accepts only server-local timestamps; the UTC Z suffix needs newer SSH.
    std::time_t time = epoch;
    std::tm tm{};
    if(!localtime_r(&time, &tm)) throw std::runtime_error("Invalid key expiration time");
    char buffer[32];
    if(!std::strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", &tm)) throw std::runtime_error("Key expiration formatting failed");
    return buffer;
}
Result run(const std::vector<std::string>& args) {
    if(args.empty()) throw std::runtime_error("Empty command");
    int pipefd[2];
    if(pipe(pipefd) != 0) throw std::runtime_error("pipe failed");
    auto pid = fork();
    if(pid == -1) { close(pipefd[0]); close(pipefd[1]); throw std::runtime_error("fork failed"); }
    if(pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO); dup2(pipefd[1], STDERR_FILENO); close(pipefd[1]);
        int nullfd = ::open("/dev/null", 0);
        if(nullfd >= 0) { dup2(nullfd, STDIN_FILENO); close(nullfd); }
        std::vector<char*> argv;
        for(const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(pipefd[1]);
    std::string out;
    char buffer[4096];
    for(;;) {
        auto n = read(pipefd[0], buffer, sizeof(buffer));
        if(n > 0) { if(out.size() < 4*1024*1024) out.append(buffer, static_cast<std::size_t>(n)); }
        else if(n < 0 && errno == EINTR) continue;
        else break;
    }
    close(pipefd[0]);
    int status = 0;
    while(waitpid(pid, &status, 0) < 0) { if(errno != EINTR) throw std::runtime_error("waitpid failed"); }
    return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), out};
}
std::string checked(const std::vector<std::string>& args) {
    auto result = run(args);
    if(result.code) throw std::runtime_error(args[0] + " failed (" + std::to_string(result.code) + "): " + result.output);
    return result.output;
}
}
