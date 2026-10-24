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
    f << "[[option]]\nfeature=\"mode\"\nid=\"amount\"\nlabel=\"Amount\"\ntype=\"integer\"\ndefault=1\nmin=1\nmax=2\n";
}
int main() {
    const fs::path root=fs::absolute("feature-constraint-fixtures");
    try {
        package(root,"test.coop","exclusive_group=\"player-controller\"\nexclusive_groups=[\"display-mode\"]");
        package(root,"test.wide","exclusive_group=\"display-mode\"\nexperimental=true");
        package(root,"test.player","exclusive_group=\"player-controller\"\nexperimental=false");
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
        check(p->feature_count(p->ctx)==3,"feature count changed");
        for(int i=0;i<p->feature_count(p->ctx);++i) {
            RecompLauncherCModFeature f{};
            check(p->feature_get(p->ctx,i,&f),"get feature metadata");
            check(f.channel==(std::string(f.package_id)=="test.wide" ? RECOMP_MOD_CHANNEL_EXPERIMENTAL : RECOMP_MOD_CHANNEL_STABLE),"experimental channel not passed to launcher");
        }
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
        nes_mod_lock_feature("test.player","mode");
        check(!p->feature_enable(p->ctx,"test.coop","mode",1),"exclusion bypassed active feature lock");
        check(nes_mod_feature_selected("test.wide","mode"),"failed exclusion partially changed selections");
        check(nes_mod_feature_selected("test.player","mode"),"exclusion disabled locked feature");
        nes_mod_lock_feature("test.coop","mode");
        check(nes_mod_feature_requires_restart("test.player","mode"),"active feature lock lost");
        check(nes_mod_feature_requires_restart("test.coop","mode"),"inactive feature lock lost");
        check(!p->feature_enable(p->ctx,"test.player","mode",0),"locked active feature toggled");
        check(!p->feature_enable(p->ctx,"test.coop","mode",1),"locked inactive feature toggled");
        check(!p->feature_set_option(p->ctx,"test.player","mode","amount","2"),"locked option changed");
        check(nes_mod_feature_selected("test.player","mode"),"failed toggle changed selection");
        check(!nes_mod_feature_selected("test.coop","mode"),"failed enable changed selection");
        check(p->feature_set_option(p->ctx,"test.wide","mode","amount","2"),"unlocked option refused");
        check(NESRecomp::mod_runtime_initialize(root,"test-game","00000000",&error),error.c_str());
        check(!nes_mod_feature_requires_restart("test.player","mode"),"feature lock leaked into next launch");
        package(root,"test.invalid","experimental=\"true\"");
        check(!NESRecomp::mod_runtime_initialize(root,"test-game","00000000",&error),"non-boolean experimental flag accepted");
        package(root,"test.invalid","exclusive_groups=[\"bad group\"]");
        check(!NESRecomp::mod_runtime_initialize(root,"test-game","00000000",&error),"invalid group id accepted");
        // Exact directory created above, never a path supplied by the caller.
        fs::remove_all(root);
        std::cout<<"PASS: multiple exclusion groups, launcher toggles, persisted conflicts and local-only lifecycle\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n';return 1; }
}
