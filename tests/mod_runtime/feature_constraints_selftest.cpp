#include "mod_runtime.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
static int active[3];
static void coop() { ++active[0];nes_mod_set_local_only("Co-op",1); }
static void wide() { ++active[1]; }
static void player() { ++active[2]; }
static void check(bool value,const char *message) { if(!value) throw std::runtime_error(message); }
static void package(const fs::path& root,const char *id,const char *groups) {
    fs::path dir=root/"packages"/id/"1.0.0";fs::create_directories(dir);
    std::ofstream f(dir/"manifest.toml");
    f << "format_version=1\nid=\"" << id << "\"\nversion=\"1.0.0\"\nname=\"Test\"\nresolver=\"declarative\"\n"
         "[[target]]\ngame_id=\"test-game\"\nrom_crc32=\"00000000\"\n"
         "[[feature]]\nid=\"mode\"\nname=\"Test\"\ndefault_enabled=false\n" << groups <<
         "\n[[plugin]]\nfeature=\"mode\"\nid=\"" << id << "\"\n";
}
int main() {
    const fs::path root=fs::absolute("feature-constraint-fixtures");
    try {
        package(root,"test.coop","exclusive_group=\"player-controller\"\nexclusive_groups=[\"display-mode\"]");
        package(root,"test.wide","exclusive_group=\"display-mode\"");
        package(root,"test.player","exclusive_group=\"player-controller\"");
        check(nes_mod_register_activation_plugin("test.coop",coop),"register co-op");
        check(nes_mod_register_activation_plugin("test.wide",wide),"register widescreen");
        check(nes_mod_register_activation_plugin("test.player",player),"register player");
        {
            std::ofstream f(root/"state.toml");f<<"format_version=1\n";
            for(const char *id:{"test.coop","test.wide","test.player"})
                f<<"[[package]]\nid=\""<<id<<"\"\nversion=\"1.0.0\"\n[[feature]]\npackage_id=\""<<id<<"\"\nid=\"mode\"\nenabled=true\n";
        }
        std::string error;
        check(NESRecomp::mod_runtime_initialize(root,"test-game","00000000",&error),error.c_str());
        check(!NESRecomp::mod_runtime_commit({},&error),"hand-edited conflicting state was accepted");
        auto *p=NESRecomp::mod_runtime_launcher_provider();
        check(p->feature_enable(p->ctx,"test.coop","mode",1),"enable co-op");
        check(NESRecomp::mod_runtime_commit({},&error),"launcher failed to clear both conflict groups");
        NESRecomp::mod_runtime_activate_plugins();
        check(active[0]==1 && active[1]==0 && active[2]==0,"conflicting plugin activated");
        check(nes_mod_local_only_reason()!=nullptr,"local-only request lost");
        check(p->feature_enable(p->ctx,"test.wide","mode",1),"enable wide");
        check(p->feature_enable(p->ctx,"test.player","mode",1),"enable replacement");
        check(NESRecomp::mod_runtime_commit({},&error),"independent groups conflicted");
        NESRecomp::mod_runtime_activate_plugins();
        check(active[0]==1 && active[1]==1 && active[2]==1,"singular group behavior changed");
        check(nes_mod_local_only_reason()==nullptr,"disabled co-op still blocks online launch");
        package(root,"test.invalid","exclusive_groups=[\"bad group\"]");
        check(!NESRecomp::mod_runtime_initialize(root,"test-game","00000000",&error),"invalid group id accepted");
        // Exact directory created above, never a path supplied by the caller.
        fs::remove_all(root);
        std::cout<<"PASS: multiple exclusion groups, launcher toggles, persisted conflicts and local-only lifecycle\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n';return 1; }
}
