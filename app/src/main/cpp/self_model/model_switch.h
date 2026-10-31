#pragma once
#include <utility>

namespace ft::self_model {


template<class Load, class Replace, class Update>
void switchModel(int current, int requested, Load load, Replace replace, Update update) {
    if(current==requested) { update(false); return; }
    auto model=load();
    if(!model) {
        if(current>=0) update(true);
        return;
    }
    replace(std::move(model));
    update(false);
}
}
