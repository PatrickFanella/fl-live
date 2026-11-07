#include "fl_live/document_render.hpp"
#include "fl_live/controller.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <type_traits>

using namespace fl_live;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("Failed: " #condition); } while (false)
static_assert(!std::is_move_constructible_v<DocumentRenderPlan>);
static_assert(!std::is_copy_constructible_v<DocumentRenderPlan>);
template<class Action> void rejects(Action action) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}
struct Fixture {
    SessionDocument document{DocumentSnapshot{9007199254740993ULL, {}, {}, {}}};
    TrackId a=document.createTrack("Audio A",TrackKind::audio), m=document.createTrack("MIDI A",TrackKind::midi);
    TrackId b=document.createTrack("Audio B",TrackKind::audio), n=document.createTrack("MIDI B",TrackKind::midi);
    ClipId ca=document.createClip(a,"Pulse"), cm=document.createClip(m,"Loop"), cb=document.createClip(b,"Wave");
    ClipId cn=document.createClip(n,"One shot"), replacement=document.createClip(a,"Replacement");
    std::vector<DocumentClipSource> sources{
        {cn,MidiClip{23,{{1,8,60,0.25F}},false}},
        {ca,AudioClip{{0.25F,-0.125F,0.5F},true}},
        {replacement,AudioClip{{-0.25F},true}},
        {cb,AudioClip{{0.0625F,-0.0625F},true}},
        {cm,MidiClip{17,{{2,5,69,0.5F}},true}}}; // deliberately not document order
};
std::vector<float> render(const DocumentRenderPlan& prepared, const Fixture& f, std::size_t block) {
    Engine engine(prepared.plan());
    for (auto id : {f.ca,f.cm,f.cb,f.cn}) {
        const auto command=prepared.launch(id,17); CHECK(command);
        CHECK(engine.schedule(*command)==ScheduleResult::accepted);
    }
    CHECK(engine.schedule(*prepared.launch(f.replacement,91))==ScheduleResult::accepted);
    CHECK(engine.schedule(*prepared.stop(f.a,150))==ScheduleResult::accepted);
    engine.start();
    std::vector<float> result(500);
    for (std::size_t i=0;i<result.size();i+=block)
        engine.process(std::span(result).subspan(i,std::min(block,result.size()-i)));
    return result;
}
float expected(std::size_t frame) {
    if (frame<17) return 0;
    const auto age=frame-17;
    const float pulse[]={0.25F,-0.125F,0.5F};
    float value=frame<91 ? pulse[age%3] : frame<150 ? -0.25F : 0;
    value += age%2 ? -0.0625F : 0.0625F;
    const auto local=age%17;
    if (local>=2 && local<7)
        value+=static_cast<float>(std::sin(2*std::numbers::pi*440*static_cast<double>(local-2)/48000))*0.05F;
    if (age>=1 && age<9)
        value+=static_cast<float>(std::sin(2*std::numbers::pi*440*std::exp2(-9.0/12)*static_cast<double>(age-1)/48000))*0.025F;
    return value;
}
// Independent per-track oracle for the fixture schedule in render().
std::array<float,4> perTrack(std::size_t frame) {
    if (frame<17) return {};
    const auto age=frame-17;
    const float pulse[]={0.25F,-0.125F,0.5F};
    const float a=frame<91 ? pulse[age%3] : frame<150 ? -0.25F : 0;
    const float b=age%2 ? -0.0625F : 0.0625F;
    const auto local=age%17;
    const float m=local>=2 && local<7 ? static_cast<float>(std::sin(2*std::numbers::pi*440*static_cast<double>(local-2)/48000))*0.05F : 0;
    const float n=age>=1 && age<9 ? static_cast<float>(std::sin(2*std::numbers::pi*440*std::exp2(-9.0/12)*static_cast<double>(age-1)/48000))*0.025F : 0;
    return {a,m,b,n}; // document track order
}
void mixerPolicy() {
    struct Case { const char* name; std::array<TrackMix,4> mix; std::array<float,4> left, right; };
    // Gains per track for each channel under the balance law and mute/solo policy.
    const Case cases[]={
        {"pan",{TrackMix{-100},TrackMix{25},TrackMix{50},TrackMix{}},{1,0.75F,0.5F,1},{0,1,1,1}},
        {"mute",{TrackMix{},TrackMix{},TrackMix{0,true},TrackMix{}},{1,1,0,1},{1,1,0,1}},
        {"solo",{TrackMix{},TrackMix{-50,false,true},TrackMix{},TrackMix{0,false,true}},{0,1,0,1},{0,0.5F,0,1}},
        {"mute beats solo",{TrackMix{0,false,true},TrackMix{0,true,true},TrackMix{},TrackMix{}},{1,0,0,0},{1,0,0,0}},
    };
    for (const auto& c : cases) {
        Fixture f;
        const TrackId ids[]={f.a,f.m,f.b,f.n};
        for (std::size_t t=0;t<4;++t) f.document.setTrackMix(ids[t],c.mix[t]);
        const DocumentRenderPlan prepared(f.document.snapshot(),f.sources,48000,137125);
        for (auto block : {1U,257U}) {
            Engine engine(prepared.plan());
            for (auto id : {f.ca,f.cm,f.cb,f.cn}) CHECK(engine.schedule(*prepared.launch(id,17))==ScheduleResult::accepted);
            CHECK(engine.schedule(*prepared.launch(f.replacement,91))==ScheduleResult::accepted);
            CHECK(engine.schedule(*prepared.stop(f.a,150))==ScheduleResult::accepted);
            engine.start();
            std::vector<float> stereo(1000);
            for (std::size_t i=0;i<stereo.size();i+=2*block)
                CHECK(engine.process(std::span(stereo).subspan(i,std::min<std::size_t>(2*block,stereo.size()-i)),2));
            for (std::size_t frame=0;frame<500;++frame) {
                const auto values=perTrack(frame);
                float left=0,right=0;
                for (std::size_t t=0;t<4;++t) { left+=c.left[t]*values[t]; right+=c.right[t]*values[t]; }
                if (std::abs(stereo[2*frame]-left)>0.000001F || std::abs(stereo[2*frame+1]-right)>0.000001F)
                    throw std::runtime_error(std::string("mixer policy mismatch: ")+c.name);
            }
        }
    }
    Fixture f;
    CHECK(SessionDocument::audible(f.document.snapshot(),f.a));
    f.document.setTrackMix(f.b,TrackMix{0,false,true});
    CHECK(!SessionDocument::audible(f.document.snapshot(),f.a) && SessionDocument::audible(f.document.snapshot(),f.b));
    const auto before=f.document.snapshot();
    rejects([&]{f.document.setTrackMix(f.a,TrackMix{101});});
    rejects([&]{f.document.setTrackMix(f.a,TrackMix{-101});});
    rejects([&]{f.document.setTrackMix(TrackId{1},TrackMix{});});
    CHECK(f.document.snapshot()==before);
    // Duplicates keep mixer settings; deleting the only soloed track releases solo.
    const auto copy=f.document.duplicateTrack(f.b);
    CHECK(std::find_if(f.document.snapshot().tracks.begin(),f.document.snapshot().tracks.end(),[&](const auto& t){return t.id==copy;})->mix.solo);
    f.document.deleteTrack(f.b);f.document.deleteTrack(copy);
    CHECK(SessionDocument::audible(f.document.snapshot(),f.a));
}
void fourTracksAndReordering() {
    Fixture f;
    const DocumentRenderPlan original(f.document.snapshot(),f.sources,48000,137125);
    CHECK(original.plan().tracks.size()==4);
    for (auto block : {1U,64U,257U}) {
        const auto samples=render(original,f,block);
        for (std::size_t i=0;i<samples.size();++i) CHECK(std::abs(samples[i]-expected(i))<0.000001F);
    }
    auto reordered=f.document.snapshot();
    std::reverse(reordered.tracks.begin(),reordered.tracks.end());
    std::reverse(reordered.clips.begin(),reordered.clips.end());
    const DocumentRenderPlan changed(reordered,f.sources,48000,137125);
    CHECK(original.trackIndex(f.a)!=changed.trackIndex(f.a));
    CHECK(original.launch(f.ca,0)->clip!=changed.launch(f.ca,0)->clip);
    const auto samples=render(changed,f,257);
    for (std::size_t i=0;i<samples.size();++i) CHECK(std::abs(samples[i]-expected(i))<0.000001F);
    for (const auto& clip:reordered.clips) {
        const auto command=changed.launch(clip.id,99); CHECK(command);
        CHECK(changed.trackId(command->track)==clip.track);
        CHECK(changed.clipId(command->track,static_cast<int>(command->clip))==clip.id);
    }
    CHECK(!changed.trackId(99)); CHECK(!changed.clipId(0,-1)); CHECK(!changed.clipId(0,99));
    CHECK(!changed.trackIndex(TrackId{})); CHECK(!changed.launch(ClipId{1},0)); CHECK(!changed.stop(TrackId{1},0));
    // Prepared assets and metadata do not borrow mutable input storage.
    f.sources.clear(); f.document.deleteTrack(f.a);
    CHECK(original.plan().tracks[0].clips.size()==2);
    CHECK(original.launch(f.ca,0));
    Engine engine(original.plan());
    CHECK(engine.schedule(*original.launch(f.ca,0))==ScheduleResult::accepted);
    engine.start(); float sample=0; engine.process(std::span(&sample,1)); CHECK(sample==0.25F);
}
void quantizedFeedback() {
    Fixture f; const DocumentRenderPlan prepared(f.document.snapshot(),f.sources,48000,137125);
    RenderController controller(prepared.plan()); std::array<float,257> block{};
    CHECK(controller.submit({ControlKind::start,{},1})); controller.process(std::span(block).first(7));
    CHECK(controller.submit({ControlKind::launch_next_bar,*prepared.launch(f.ca,0),2}));
    const Frame boundary=(48000ULL*240000+137125-1)/137125;
    while(controller.status().position<boundary+1) {
        const auto begin=controller.status().position;
        controller.process(block);
        for(std::size_t i=0;i<block.size();++i) {
            const auto frame=begin+i;
            const float wave[]={0.25F,-0.125F,0.5F};
            CHECK(block[i]==(frame<boundary?0:wave[(frame-boundary)%3]));
        }
    }
    const auto status=controller.status(); const auto index=*prepared.trackIndex(f.a);
    CHECK(status.handled_serial==2 && status.rejected==0);
    CHECK(prepared.clipId(index,status.tracks[index].active_clip)==f.ca);
    controller.panic(); controller.process(block);
    CHECK(!controller.status().running);
    CHECK(!prepared.clipId(index,controller.status().tracks[index].active_clip));
}
void rejectionAndBudgets() {
    Fixture f; const auto before=f.document.snapshot();
    const auto bad=[&](auto edit) { auto sources=f.sources;edit(sources);rejects([&]{DocumentRenderPlan invalid(before,sources);});CHECK(f.document.snapshot()==before); };
    bad([](auto& s){s.pop_back();});
    bad([](auto& s){s.push_back(s[0]);});
    bad([](auto& s){s[0].id=s[1].id;});
    bad([](auto& s){s[0].id=ClipId{999};});
    bad([](auto& s){s[0].content=AudioClip{{0.1F},true};});
    bad([](auto& s){std::get<AudioClip>(s[1].content).samples.clear();});
    bad([](auto& s){std::get<AudioClip>(s[1].content).samples[0]=std::numeric_limits<float>::quiet_NaN();});
    bad([](auto& s){std::get<MidiClip>(s[0].content).notes[0].duration=0;});
    bad([](auto& s){std::get<MidiClip>(s[0].content).notes.assign(17,Note{0,8,60,1});});
    rejects([&]{DocumentRenderPlan invalid(before,f.sources,0);});
    rejects([&]{DocumentRenderPlan invalid(before,f.sources,48000,0);});
    rejects([&]{DocumentRenderPlan invalid(before,f.sources,48000,120000,{5,2});});
    rejects([&]{DocumentRenderPlan invalid(before,f.sources,48000,120000,{6,1});});
    const DocumentRenderPlan exact(before,f.sources,48000,120000,{6,2});
    std::vector<DocumentTrackGain> gains{{f.n,2},{f.b,1},{f.m,0},{f.a,0.5F}};
    const DocumentRenderPlan mixed(before,f.sources,48000,120000,{},gains);
    CHECK(mixed.plan().tracks[0].gain==0.5F && mixed.plan().tracks[1].gain==0);
    auto badGains=gains;badGains[0].id=f.m;
    rejects([&]{DocumentRenderPlan invalid(before,f.sources,48000,120000,{},badGains);});
    badGains=gains;badGains[0].gain=std::numeric_limits<float>::quiet_NaN();
    rejects([&]{DocumentRenderPlan invalid(before,f.sources,48000,120000,{},badGains);});
    badGains=gains;badGains.pop_back();
    rejects([&]{DocumentRenderPlan invalid(before,f.sources,48000,120000,{},badGains);});
    badGains=gains;badGains[0].id=TrackId{1};
    rejects([&]{DocumentRenderPlan invalid(before,f.sources,48000,120000,{},badGains);});
    rejects([&]{DocumentRenderPlan invalid(before,f.sources,48000,120000,{PreparationBudget::audio_frame_limit+1,2});});
    // Valid metadata may exceed the current renderer's separate clip bound.
    SessionDocument many; const auto track=many.createTrack("Audio",TrackKind::audio);
    std::vector<DocumentClipSource> sources;
    for(std::size_t i=0;i<=Engine::clips_per_track_capacity;++i)
        sources.push_back({many.createClip(track,"Clip"),AudioClip{{0},true}});
    rejects([&]{DocumentRenderPlan invalid(many.snapshot(),sources);});
    auto broken=before;broken.clips[0].track=TrackId{};
    rejects([&]{DocumentRenderPlan invalid(broken,f.sources);});
    const DocumentRenderPlan empty(DocumentSnapshot{},std::span<const DocumentClipSource>{});
    CHECK(empty.plan().tracks.empty());
}
void stoppedDeletionAndOwnership() {
    SessionDocument document;const auto track=document.createTrack("MIDI",TrackKind::midi);
    const auto clip=document.createClip(track,"Held");const auto scene=document.createScene("Scene");document.setSlot(scene,track,clip);
    std::vector<DocumentClipSource> sources{{clip,MidiClip{100000,{{0,90000,69,1}},true}}};
    const DocumentRenderPlan old(document.snapshot(),sources);
    Engine engine(old.plan());CHECK(engine.schedule(*old.launch(clip,0))==ScheduleResult::accepted);
    engine.start();std::array<float,257> block{};engine.process(block);CHECK(engine.active_voices()==1);
    document.deleteClip(clip); sources.clear();
    const DocumentRenderPlan next(document.snapshot(),sources);
    CHECK(!next.launch(clip,0)); CHECK(next.document().scenes[0].entries.empty());
    // Old immutable assets stay valid until their render owner is stopped.
    engine.process(block);CHECK(engine.active_voices()==1);
    engine.stop();CHECK(engine.active_voices()==0);engine.process(block);
    CHECK(std::all_of(block.begin(),block.end(),[](float v){return v==0;}));
    Engine restarted(next.plan());restarted.start();restarted.process(block);
    CHECK(restarted.active_voices()==0);
    CHECK(std::all_of(block.begin(),block.end(),[](float v){return v==0;}));
}
int main() {
    try {
        fourTracksAndReordering();mixerPolicy();quantizedFeedback();rejectionAndBudgets();stoppedDeletionAndOwnership();
        std::cout<<"PASS: stable document/render mapping, four-track audio, mixer pan/mute/solo, budgets and stopped cleanup\n";return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
