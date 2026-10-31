#include "fl_live/document_render.hpp"
#include "fl_live/controller.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

using namespace fl_live;
#define CHECK(value) do { if (!(value)) throw std::runtime_error("Failed: " #value); } while (false)
struct Fixture {
    SessionDocument document{DocumentSnapshot{9007199254740993ULL,{},{},{}}};
    TrackId a=document.createTrack("Audio A",TrackKind::audio), m=document.createTrack("MIDI A",TrackKind::midi);
    TrackId b=document.createTrack("Audio B",TrackKind::audio), n=document.createTrack("MIDI B",TrackKind::midi);
    ClipId ca=document.createClip(a,"A"), cm=document.createClip(m,"M"), cb=document.createClip(b,"B"), cn=document.createClip(n,"N");
    ClipId replacement=document.createClip(a,"Replacement");
    SceneId full=document.createScene("Full"), sparse=document.createScene("Sparse");
    std::vector<DocumentClipSource> sources{{ca,AudioClip{{0.25F},true}},{cm,MidiClip{500000,{{0,400000,69,1}},true}},
        {cb,AudioClip{{0.125F},true}},{cn,MidiClip{500000,{{0,400000,72,1}},true}},{replacement,AudioClip{{-0.5F},true}}};
    Fixture() {
        document.setSlot(full,a,ca);document.setSlot(full,m,cm);document.setSlot(full,b,cb);document.setSlot(full,n,cn);
        document.setSlot(sparse,a,replacement);
    }
};
Control sceneControl(const DocumentRenderPlan& prepared,SceneId scene,bool stop,LaunchPolicy policy,std::uint64_t serial) {
    const auto batch=prepared.scene(scene,stop);CHECK(batch);
    return {ControlKind::scene,{},serial,*batch,policy};
}
void mapping() {
    Fixture f;auto snapshot=f.document.snapshot();
    std::reverse(snapshot.tracks.begin(),snapshot.tracks.end());std::reverse(snapshot.clips.begin(),snapshot.clips.end());
    std::reverse(snapshot.scenes[0].entries.begin(),snapshot.scenes[0].entries.end());
    DocumentRenderPlan prepared(snapshot,f.sources);
    const auto batch=prepared.scene(f.sparse,false,23);CHECK(batch && batch->count==4);
    for(std::size_t i=0;i<batch->count;++i) {
        const auto command=batch->commands[i];CHECK(command.track==i && command.at==23);
        if(prepared.trackId(i)==f.a) {CHECK(command.kind==CommandKind::launch);CHECK(prepared.clipId(i,static_cast<int>(command.clip))==f.replacement);}
        else CHECK(command.kind==CommandKind::stop_track);
    }
    const auto stop=prepared.scene(f.full,true);CHECK(stop && stop->count==4);
    for(std::size_t i=0;i<stop->count;++i)CHECK(stop->commands[i].kind==CommandKind::stop_track);
    CHECK(!prepared.scene(SceneId{},false));CHECK(!prepared.scene(SceneId{1},true));
}
void timing(std::size_t block,LaunchPolicy policy) {
    Fixture f;DocumentRenderPlan prepared(f.document.snapshot(),f.sources,48000,137125);
    RenderController controller(prepared.plan());std::vector<float> buffer(block);
    CHECK(controller.submit({ControlKind::start,{},1}));controller.process(std::span(buffer).first(std::min(block,7UL)));
    // Every workload submits at exactly frame 7, independent of callback size.
    while(controller.status().position<7)controller.process(std::span(buffer).first(std::min<Frame>(block,7-controller.status().position)));
    const TempoGrid grid(48000,137125);const Frame at=policy==LaunchPolicy::immediate?7:grid.next_bar(7);
    if(policy==LaunchPolicy::next_bar)CHECK(at==(48000ULL*240000+137125-1)/137125);
    CHECK(controller.submit(sceneControl(prepared,f.full,false,policy,2)));controller.process({});
    auto state=controller.status();CHECK(state.handled_serial==2 && !state.rejected);
    for(std::size_t i=0;i<4;++i)CHECK(state.tracks[i].active_clip==-1 && state.tracks[i].pending==PendingAction::launch && state.tracks[i].pending_at==at);
    const auto end=at+31;
    while(controller.status().position<end) {
        const auto begin=controller.status().position;
        const auto count=static_cast<std::size_t>(std::min<Frame>(block,end-begin));controller.process(std::span(buffer).first(count));
        for(std::size_t i=0;i<count;++i) {
            const auto frame=begin+i;float expected=0;
            if(frame>=at) {
                const auto age=static_cast<double>(frame-at);
                expected=0.375F+static_cast<float>(std::sin(2*std::numbers::pi*440*age/48000))*0.1F+
                    static_cast<float>(std::sin(2*std::numbers::pi*440*std::exp2(3.0/12)*age/48000))*0.1F;
            }
            CHECK(std::abs(buffer[i]-expected)<0.000001F);
        }
    }
    state=controller.status();for(std::size_t i=0;i<4;++i)CHECK(state.tracks[i].active_clip>=0 && state.tracks[i].pending==PendingAction::none);
    CHECK(controller.submit(sceneControl(prepared,f.full,true,policy,3)));controller.process({});
    const Frame stop=policy==LaunchPolicy::immediate?end:grid.next_bar(end);
    if(policy==LaunchPolicy::next_bar)CHECK(stop==(2*48000ULL*240000+137125-1)/137125);
    for(std::size_t i=0;i<4;++i)CHECK(controller.status().tracks[i].pending_at==stop && controller.status().tracks[i].pending==PendingAction::stop);
    while(controller.status().position<stop+37) {
        const auto begin=controller.status().position;
        const auto count=static_cast<std::size_t>(std::min<Frame>(block,stop+37-begin));controller.process(std::span(buffer).first(count));
        for(std::size_t i=0;i<count;++i)if(begin+i>=stop)CHECK(buffer[i]==0);
    }
    for(std::size_t i=0;i<4;++i)CHECK(controller.status().tracks[i].active_clip==-1 && controller.status().tracks[i].pending==PendingAction::none);
    CHECK(controller.status().running); // Scene stop does not stop transport.
}
void orderingAndCleanup() {
    Fixture f;DocumentRenderPlan prepared(f.document.snapshot(),f.sources,48000,137125);
    RenderController controller(prepared.plan());std::array<float,257> buffer{};
    CHECK(controller.submit({ControlKind::start,{},1}));
    CHECK(controller.submit(sceneControl(prepared,f.full,false,LaunchPolicy::immediate,2)));controller.process(buffer);
    CHECK(controller.submit(sceneControl(prepared,f.full,false,LaunchPolicy::next_bar,3)));controller.process({});
    CHECK(controller.submit(sceneControl(prepared,f.sparse,false,LaunchPolicy::immediate,4)));controller.process(buffer);
    // Immediate inserts ahead of the already queued future scene. Empty slots
    // clear MIDI voices before this callback's first sample.
    for(auto sample:buffer)CHECK(sample==-0.5F);
    auto state=controller.status();CHECK(state.tracks[0].active_clip==1 && state.tracks[1].active_clip==-1 && state.tracks[3].active_clip==-1);
    CHECK(state.tracks[0].pending==PendingAction::launch);
    // Arrival order wins at ties: full launch, stop, then sparse launch.
    CHECK(controller.submit(sceneControl(prepared,f.full,true,LaunchPolicy::next_bar,5)));
    CHECK(controller.submit(sceneControl(prepared,f.sparse,false,LaunchPolicy::next_bar,6)));
    const auto boundary=TempoGrid(48000,137125).next_bar(state.position);
    while(controller.status().position<boundary+17) {
        const auto begin=controller.status().position;
        const auto count=static_cast<std::size_t>(std::min<Frame>(buffer.size(),boundary+17-begin));
        controller.process(std::span(buffer).first(count));for(std::size_t i=0;i<count;++i)CHECK(buffer[i]==-0.5F);
    }
    controller.panic();controller.process(buffer);for(auto sample:buffer)CHECK(sample==0);
    state=controller.status();CHECK(!state.running && state.handled_serial==6 && !state.rejected);
    for(std::size_t i=0;i<4;++i)CHECK(state.tracks[i].active_clip==-1 && state.tracks[i].pending==PendingAction::none);
    Engine engine(prepared.plan());engine.start();auto full=*prepared.scene(f.full,false);
    CHECK(engine.scheduleTogether(std::span(full.commands).first(full.count))==ScheduleResult::accepted);engine.process(buffer);CHECK(engine.active_voices()==2);
    auto sparse=*prepared.scene(f.sparse,false,engine.position());
    CHECK(engine.scheduleTogether(std::span(sparse.commands).first(sparse.count))==ScheduleResult::accepted);engine.process(buffer);CHECK(engine.active_voices()==0);
}
void atomicRejectionAndWrap() {
    Fixture f;DocumentRenderPlan prepared(f.document.snapshot(),f.sources);Engine engine(prepared.plan());
    auto batch=*prepared.scene(f.full,false,19);auto commands=std::span(batch.commands).first(batch.count);
    auto invalid=batch;invalid.commands[3].clip=99;
    CHECK(engine.scheduleTogether(std::span(invalid.commands).first(invalid.count))==ScheduleResult::invalid);
    for(std::size_t i=0;i<4;++i)CHECK(engine.track_statuses()[i].pending==PendingAction::none);
    // Fill all but three places; a four-track scene must leave no partial launch.
    for(std::size_t i=0;i<61;++i)CHECK(engine.schedule({CommandKind::stop_track,30,0})==ScheduleResult::accepted);
    CHECK(engine.scheduleTogether(commands)==ScheduleResult::full);
    CHECK(engine.track_statuses()[0].pending==PendingAction::stop && engine.track_statuses()[1].pending==PendingAction::none);
    std::array<float,31> output{};engine.start();engine.process(output);
    CHECK(engine.scheduleTogether(commands)==ScheduleResult::in_past);
    // Wrap the ring, insert an earlier group, then verify stable same-frame order.
    for(auto& command:commands)command.at=40;
    CHECK(engine.schedule({CommandKind::stop_track,50,0})==ScheduleResult::accepted);
    CHECK(engine.scheduleTogether(commands)==ScheduleResult::accepted);
    engine.process(output);CHECK(engine.track_statuses()[0].active_clip==-1 && engine.track_statuses()[1].active_clip==0);
    CHECK(engine.scheduleTogether({})==ScheduleResult::invalid);
    commands[3].at=100;CHECK(engine.scheduleTogether(commands)==ScheduleResult::invalid);

    RenderController controller(prepared.plan());
    for(std::uint64_t i=1;i<=61;++i)CHECK(controller.submit({ControlKind::schedule,{CommandKind::stop_track,30,0},i}));
    CHECK(controller.submit(sceneControl(prepared,f.full,false,LaunchPolicy::immediate,62)));controller.process({});
    CHECK(controller.status().rejected==1 && controller.status().handled_serial==62);
    for(std::size_t i=1;i<4;++i)CHECK(controller.status().tracks[i].pending==PendingAction::none);
    auto bad=sceneControl(prepared,f.full,false,LaunchPolicy::immediate,63);bad.batch.count=65;
    CHECK(controller.submit(bad));controller.process({});CHECK(controller.status().rejected==2);
    bad.batch.count=4;bad.policy=static_cast<LaunchPolicy>(99);CHECK(controller.submit(bad));controller.process({});CHECK(controller.status().rejected==3);
    for(std::uint64_t i=64;i<128;++i)CHECK(controller.submit({ControlKind::start,{},i}));
    CHECK(!controller.submit(sceneControl(prepared,f.full,false,LaunchPolicy::immediate,128)));
    controller.panic();controller.process(output);CHECK(!controller.status().running && controller.status().handled_serial==127);
    CHECK(controller.submit(sceneControl(prepared,f.full,false,LaunchPolicy::immediate,129)));
    CHECK(controller.submit({ControlKind::start,{},130}));controller.process(output);CHECK(controller.status().tracks[3].active_clip==0);
}
void capacity() {
    SessionPlan plan;plan.tracks.resize(64);for(auto& track:plan.tracks)track.clips.push_back(AudioClip{{0.001F},true});
    RenderController controller(plan);Control scene{ControlKind::scene,{},1};scene.policy=LaunchPolicy::immediate;scene.batch.count=64;
    for(std::size_t i=0;i<64;++i)scene.batch.commands[i]={CommandKind::launch,999,i,0};
    CHECK(controller.submit(scene));CHECK(controller.submit({ControlKind::start,{},2}));float output=0;controller.process(std::span(&output,1));
    CHECK(std::abs(output-0.064F)<0.000001F);for(std::size_t i=0;i<64;++i)CHECK(controller.status().tracks[i].active_clip==0);
}
int main() {
    try {
        mapping();for(auto block:{1U,64U,257U})for(auto policy:{LaunchPolicy::immediate,LaunchPolicy::next_bar})timing(block,policy);
        orderingAndCleanup();atomicRejectionAndWrap();capacity();
        std::cout<<"PASS: four-track fractional-tempo scene boundaries, empty slots, ordering, MIDI cleanup, atomic overflow, panic and 64-track capacity\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
