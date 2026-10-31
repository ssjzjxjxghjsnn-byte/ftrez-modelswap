#include "self_model/model_hooks.h"
#include <chrono>
#include <iostream>
#include <thread>

namespace ft::sdk {
void* resolveType(const std::vector<void*>&,const Symbol&) {
    static int profileType;
    return &profileType;
}
}
namespace ft::il2cpp::frame_hook {
void install(const std::vector<void*>&,std::function<void()> callback) {
    profile_test::render=std::move(callback);
}
}

using ft::self_model::ModelHooks;
using namespace profile_test;
void require(bool condition,const char* message) {
    if(!condition) throw std::runtime_error(message);
}
void install(ModelHooks& hooks) {
    ft::sdk::Symbols symbols;
    symbols.types["ProfileUI"]={};
    hooks.install({},symbols,[]{return mainThread;},[]{++updates;});
}
void nextFrame() { ++frame;render(); }

void otherProfile() {
    reset();ModelHooks hooks;Object page;
    page.own=false;page.enabled=false;objects={&page};install(hooks);render();
    require(hooks.profileObject()==&page,"other/disabled-component profile must remain tracked");
    require(!hooks.lobbyAllowed(frame),"other user's page must block lobby");
    page.own=true;nextFrame();
    require(hooks.profileObject()==&page,"ownership changes must not alter presence tracking");
    require(!hooks.lobbyAllowed(frame),"own page must block lobby too");
    require(queries==1,"active page must not force a full scene search each frame");
    require(holds==0,"temporary roots must be cleared");
}

void freshAbsenceAndFrameReuse() {
    reset();ModelHooks hooks;install(hooks);render();
    require(hooks.lobbyAllowed(frame),"successful empty search must allow lobby");
    require(hooks.lobbyAllowed(frame),"same-frame donor must share empty result");
    require(queries==1,"same-frame lobby queries must be deduplicated");
    nextFrame();require(hooks.lobbyAllowed(frame),"empty next frame must allow lobby");
    require(queries==2,"lobby absence must be confirmed once each frame");
    Object page;objects={&page};nextFrame();
    require(!hooks.lobbyAllowed(frame),"new page inside polling interval must block lobby");
    require(hooks.profileObject()==&page,"forced search must discover the new page");
    require(queries==3,"new frame must perform exactly one fresh search");
    require(!hooks.lobbyAllowed(frame),"snapshot donor must see same active result");
    require(queries==3,"donor must not repeat active search");
}

void weakLossAndClosing() {
    reset();ModelHooks hooks;Object first,other;
    objects={&first};install(hooks);render();
    handles.clear();objects={&other};render();
    require(!hooks.lobbyAllowed(frame),"same-frame weak loss must stay unknown and block lobby");
    require(!hooks.profileObject(),"lost target must not expose the previous object");
    require(queries==1,"same-frame weak loss must not repeat an earlier query");
    nextFrame();
    require(!hooks.lobbyAllowed(frame),"lost weak handle must not imply no active page");
    require(hooks.profileObject()==&other,"lost weak handle must permit replacement discovery");
    other.active=false;nextFrame();
    require(hooks.lobbyAllowed(frame),"closed page plus fresh empty query must allow lobby");
    require(!hooks.profileObject(),"closed page weak handle must be released");
    other.active=true;nextFrame();
    require(!hooks.lobbyAllowed(frame),"reactivated page must block lobby immediately");
    other.alive=false;objects.clear();nextFrame();
    failQuery=true;
    require(!hooks.lobbyAllowed(frame),"destroyed page and failed replacement search must block lobby");
    const auto before=queries;
    require(!hooks.lobbyAllowed(frame),"failed result must stay unknown in same frame");
    require(queries==before,"failed replacement query must be shared in same frame");
    failQuery=false;nextFrame();
    require(hooks.lobbyAllowed(frame),"next-frame successful empty query must recover");
}

void ambiguousPages() {
    reset();ModelHooks hooks;Object own,other;own.own=true;
    objects={&own,&other};install(hooks);render();
    require(!hooks.profileObject(),"multiple active pages must not select an arbitrary preview");
    require(!hooks.lobbyAllowed(frame),"multiple active pages must block lobby");
    other.active=false;nextFrame();
    require(!hooks.lobbyAllowed(frame),"ambiguity must remain conservative before refresh");
    require(!hooks.profileObject(),"ambiguous selection must wait for a new search");
    std::this_thread::sleep_for(std::chrono::milliseconds(510));nextFrame();
    require(hooks.profileObject()==&own,"periodic refresh must resolve prior ambiguity");
    require(queries==2,"active/ambiguous tracking should use 500ms discovery interval");
}

void queryFailures() {
    for(bool returnNull:{false,true}) {
        reset();ModelHooks hooks;install(hooks);
        failQuery=!returnNull;nullArray=returnNull;render();
        require(!hooks.lobbyAllowed(frame),"query failure/null array must block lobby");
        require(!hooks.profileObject(),"query failure must not expose a preview");
        require(queries==1,"failed normal query must be reused by forced lobby check");
        failQuery=nullArray=false;nextFrame();
        require(hooks.lobbyAllowed(frame),"failed query should recover in next frame");
        require(queries==2,"retry must query only once in next frame");
    }
    reset();ModelHooks hooks;Object page;objects={&page};failWeak=true;install(hooks);render();
    require(!hooks.lobbyAllowed(frame),"weak handle creation failure must block lobby");
    require(!hooks.profileObject(),"weak handle creation failure must not expose raw object");
    require(queries==1,"weak handle failure must share the failed query for the frame");
    failWeak=false;nextFrame();
    require(!hooks.lobbyAllowed(frame),"successful active-page retry must still block lobby");
    require(hooks.profileObject()==&page,"weak handle retry must restore preview candidate");
    require(holds==0,"error paths must clear temporary roots");
}

void mainThreadGuard() {
    reset();ModelHooks hooks;install(hooks);mainThread=false;render();
    require(queries==0 && updates==0 && clears==0,"off-main callback must not call Unity or update");
    mainThread=true;render();
    require(queries==1 && updates==1,"main callback must discover then update");
}

int main() {
    try {
        otherProfile();freshAbsenceAndFrameReuse();weakLossAndClosing();
        ambiguousPages();queryFailures();mainThreadGuard();
        std::cout<<"PASS: 6 profile tracking scenarios\n";
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';return 1;
    }
}
