#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace agent {
struct Account {
    std::string name;
    std::uint32_t uid{};
    std::int64_t expires{};
    bool root{};
};
bool valid_name(const std::string& name);
bool valid_ip(const std::string& address);
int number(const std::string& value, int low, int high);
std::string serialize(const Account& account);
Account deserialize(const std::string& line);
std::string utc(std::int64_t epoch, const char* format = "%Y-%m-%d %H:%M:%S UTC");
struct Result { int code; std::string output; };
Result run(const std::vector<std::string>& args);
std::string checked(const std::vector<std::string>& args);
}
