#include "core.hpp"
#include <iostream>
#include <stdexcept>

void require(bool b) { if(!b) throw std::runtime_error("Test assertion failed"); }
template<class F> void rejects(F f) { bool threw=false; try { f(); } catch(const std::exception&) { threw=true; } require(threw); }
int main() {
    require(agent::valid_name("agent_demo_1"));
    for(auto s : {"root", "agent_", "agent_a;id", "../agent_a", "agent_A", "agent_a\n"}) require(!agent::valid_name(s));
    require(agent::valid_ip("203.0.113.10"));
    require(agent::valid_ip("2001:db8::1"));
    require(!agent::valid_ip("1.2.3.999"));
    require(!agent::valid_ip("$(id)"));
    require(agent::number("65535",1,65535) == 65535);
    for(auto s : {"0", "-1", "12garbage", "99999999999999999", " 1", ""}) rejects([&]{ agent::number(s,1,65535); });
    agent::Account a{"agent_test", 1234, 2000000000, true};
    auto b = agent::deserialize(agent::serialize(a));
    require(b.name == a.name && b.uid == a.uid && b.expires == a.expires && b.root == a.root);
    for(auto s : {"root 0 1 1", "agent_test 0 1 0", "agent_test 1234 -1 0", "agent_test 1234 100 2", "agent_test 1234 100 0 trailing"})
        rejects([&]{ agent::deserialize(s); });
    require(agent::utc(0) == "1970-01-01 00:00:00 UTC");
    auto r = agent::run({"/usr/bin/printf", "%s", "a; $(id) ' b"});
    require(r.code == 0 && r.output == "a; $(id) ' b");
    require(agent::run({"/no/such/program"}).code == 127);
    require(agent::run({"/bin/sh", "-c", "exit 7"}).code == 7);
    std::cout << "Core tests passed\n";
}
