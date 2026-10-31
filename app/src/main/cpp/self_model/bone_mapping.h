#pragma once
#include "self_model/model_format.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <map>

namespace ft::self_model {


inline const char* humanBoneName(int index) {
    static constexpr std::array<const char*,55> names{{
        "pelvis","lthigh","rthigh","lcalf","rcalf","lfoot","rfoot","spine","spine1","neck","head",
        "lclavicle","rclavicle","lupperarm","rupperarm","lforearm","rforearm","lhand","rhand",
        "ltoe0","rtoe0","leye","reye","jaw",
        "lfinger0","lfinger01","lfinger02","lfinger1","lfinger11","lfinger12",
        "lfinger2","lfinger21","lfinger22","lfinger3","lfinger31","lfinger32",
        "lfinger4","lfinger41","lfinger42","rfinger0","rfinger01","rfinger02",
        "rfinger1","rfinger11","rfinger12","rfinger2","rfinger21","rfinger22",
        "rfinger3","rfinger31","rfinger32","rfinger4","rfinger41","rfinger42","spine2"
    }};
    return index>=0 && index<int(names.size())?names[index]:nullptr;
}
inline std::string canonical(std::string s) {
    std::string v;
    for(unsigned char c:s) if(c<128 && std::isalnum(c)) v+=char(std::tolower(c));
    for(auto prefix:{"mixamorig","bip001","bip01"}) if(v.compare(0,std::strlen(prefix),prefix)==0) { v.erase(0,std::strlen(prefix)); break; }
    static const std::map<std::string,std::string> aliases{
        {"hips","pelvis"},{"bnhead","head"},{"chest","spine1"},{"upperchest","spine2"},
        {"leftupperleg","lthigh"},{"rightupperleg","rthigh"},{"leftlowerleg","lcalf"},{"rightlowerleg","rcalf"},
        {"leftfoot","lfoot"},{"rightfoot","rfoot"},{"lefttoes","ltoe0"},{"righttoes","rtoe0"},
        {"leftshoulder","lclavicle"},{"rightshoulder","rclavicle"},{"leftupperarm","lupperarm"},
        {"rightupperarm","rupperarm"},{"leftlowerarm","lforearm"},{"rightlowerarm","rforearm"},
        {"lefthand","lhand"},{"righthand","rhand"}};
    auto it=aliases.find(v); return it==aliases.end()?v:it->second;
}
inline std::string boneKey(const Bone& b) {
    const auto human=humanBoneName(b.human);
    return human?human:canonical(b.name);
}
}
