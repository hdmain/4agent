#pragma once
#include "core.hpp"
#include <filesystem>
namespace agent {
inline constexpr const char* default_export_directory = "/var/lib/4agent/exports";
struct Options {
    std::string name;
    int minutes = 60;
    bool root = false;
    std::string ip;
    int port = 22;
    std::filesystem::path output = default_export_directory;
};
class Admin {
    int lock_ = -1;
public:
    Admin();
    ~Admin();
    Admin(const Admin&) = delete;
    Admin& operator=(const Admin&) = delete;
    std::vector<Account> accounts();
    std::filesystem::path create(Options options);
    void revoke(const std::string& name);
    void reap();
    std::string history(const std::string& name);
};
std::string hostname();
std::string detect_ip();
}
