#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>

namespace ft::self_model {
inline bool rabbitName(std::string name, bool root=false) {
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});
    return name.find(root?"item_rabbit":"rabbit")!=std::string::npos;
}


inline bool keepRabbit(float distance) { return std::isfinite(distance) && distance<3.f; }
inline float rabbitSearchLimit(float currentDistance) {
    return std::isfinite(currentDistance)?1.6f:std::numeric_limits<float>::infinity();
}
enum class RabbitSize { Normal, Hidden, Rebuild };
inline RabbitSize rabbitSize(float current,float baseline) {
    if(!std::isfinite(current) || current<0 || !std::isfinite(baseline) || baseline<=0)
        return RabbitSize::Hidden;
    if(current<baseline*.5f) return RabbitSize::Hidden;
    return current>baseline*1.5f?RabbitSize::Rebuild:RabbitSize::Normal;
}
}
