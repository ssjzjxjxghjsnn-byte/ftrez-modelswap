#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace ft::sdk {
struct Symbol {
    std::string namespc,type,member,signature;
    uint32_t typeToken=0,token=0,flags=0;
    std::string assembly,result;
    std::vector<std::string> parameters;
    bool isStatic=false;
};
struct Symbols {
    std::map<std::string,Symbol> types,fields,methods;
    std::vector<std::string> unresolved;
};
}
