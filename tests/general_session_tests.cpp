// Scheduling and failure regressions for general desktop content (#33):
// imported audio ranges, pulse, per-clip MIDI patterns, scenes, deletion and
// missing media, rendered through the same preparation path as the desktop.
#include "authored_project.hpp"
#include "fl_live/controller.hpp"
#include <algorithm>
#include <limits>
#include <iostream>
#include <stdexcept>

using namespace fl_live;
#define CHECK(value) do { if (!(value)) throw std::runtime_error("Failed: " #value); } while (false)
template<class F> void rejects(F f) { bool failed = false; try { f(); } catch (const std::exception&) { failed = true; } CHECK(failed); }

namespace {
constexpr int tempo = 137; // 21021.9 frames per beat: bar boundaries are not integral
struct Project {
    AuthoredProject project = defaultProject();
    TrackId pulse, steps, loop, bass;
    ClipId pulseClip, stepsClip, loopClip, bassClip;
    SceneId full, sparse;
    Project() {
        SessionDocument document(project.identities);
        pulse = project.identities.tracks[0].id; steps = project.identities.tracks[1].id;
        pulseClip = project.identities.clips[0].id; stepsClip = project.identities.clips[1].id;
        loop = document.createTrack("Loop", TrackKind::audio); bass = document.createTrack("Bass", TrackKind::midi);
        loopClip = document.createClip(loop, "Loop"); bassClip = document.createClip(bass, "Bass");
        full = project.identities.scenes[0].id;
        document.setSlot(full, loop, loopClip); document.setSlot(full, bass, bassClip);
        sparse = document.createScene("Sparse"); document.setSlot(sparse, loop, loopClip);
        project.identities = document.snapshot();
        project.content.tempo = tempo;
        project.content.tracks.push_back({loop, 80}); project.content.tracks.push_back({bass, 60});
        // Seven stereo frames; the range plays frames 1..5, looped.
        AudioClip source{{}, true, 2};
        for (int n = 0; n < 7; ++n) { source.samples.push_back(0.01F * static_cast<float>(n + 1)); source.samples.push_back(-0.02F * static_cast<float>(n + 1)); }
        ClipContent wav{loopClip};
        wav.audio = AudioSource::wav; wav.asset = "/fixtures/loop.wav";
        wav.media = std::make_shared<const AudioClip>(source); wav.range_start = 1; wav.range_length = 5;
        ClipContent notes{bassClip};
        for (auto& step : notes.pattern.steps) step.enabled = false;
        notes.pattern.step_count = 3; notes.pattern.steps[0] = {true, 40, 90, 100}; notes.pattern.steps[2] = {true, 47, 60, 30};
        project.content.clips.push_back(wav); project.content.clips.push_back(notes);
        validateContent(project.content, project.identities);
    }
};
std::unique_ptr<DocumentRenderPlan> prepare(const AuthoredProject& project) {
    return std::make_unique<DocumentRenderPlan>(project.identities, contentSources(project.content, project.identities), 48000,
        static_cast<std::uint32_t>(project.content.tempo) * 1000U, PreparationBudget{}, contentGains(project.content, project.identities));
}
// Absolute-frame schedule shared by every block size: full scene at bar 1,
// sparse scene (replacing three tracks with stops) mid-bar, scene stop at bar 3.
struct Timeline { Frame bar1, replace, bar3; };
std::vector<float> render(const DocumentRenderPlan& plan, const Project& p, const Timeline& t, std::size_t block, std::size_t frames,
                          std::vector<std::size_t>* voices = nullptr) {
    Engine engine(plan.plan());
    const auto schedule = [&](SceneId scene, bool stop, Frame at) {
        const auto batch = plan.scene(scene, stop, at); CHECK(batch);
        CHECK(engine.scheduleTogether(std::span(batch->commands).first(batch->count)) == ScheduleResult::accepted);
    };
    schedule(p.full, false, t.bar1); schedule(p.sparse, false, t.replace); schedule(p.full, true, t.bar3);
    engine.start();
    std::vector<float> out(frames * 2);
    for (std::size_t at = 0; at < frames; at += block) {
        const auto count = std::min(block, frames - at);
        CHECK(engine.process(std::span(out).subspan(at * 2, count * 2), 2));
        if (voices) voices->push_back(engine.active_voices());
    }
    return out;
}
void blockInvariance() {
    const Project p;
    const auto plan = prepare(p.project);
    const TempoGrid grid(48000, tempo * 1000);
    const Timeline t{grid.next_bar(1), grid.next_bar(1) + 30001, grid.next_bar(grid.next_bar(grid.next_bar(1) + 1) + 1)};
    CHECK(t.bar1 == 84088 && t.bar3 == 252263); // ceil(k * 48000 * 240 / 137)
    const std::size_t frames = 260000;
    const auto reference = render(*plan, p, t, 64, frames);
    for (auto block : {1U, 128U, 257U, 512U}) CHECK(render(*plan, p, t, block, frames) == reference);
    const auto index = *plan->trackIndex(p.loop);
    const auto gainLeft = 0.8F, gainRight = 0.8F;
    for (Frame f = 0; f < frames; ++f) {
        const float left = reference[2 * f], right = reference[2 * f + 1];
        if (f < t.bar1 || f >= t.bar3) CHECK(left == 0 && right == 0);
        else if (f >= t.replace) {
            // Only the looped range remains: frames 1..5 of the source.
            const auto n = static_cast<float>((f - t.replace) % 5 + 2);
            CHECK(left == gainLeft * (0.01F * n) && right == gainRight * (-0.02F * n));
        }
    }
    CHECK(index == 2);
    std::vector<std::size_t> voices;
    (void)render(*plan, p, t, 257, frames, &voices);
    // After the sparse replacement every MIDI voice is released; after the
    // scene stop nothing sounds.
    CHECK(std::all_of(voices.begin() + static_cast<std::ptrdiff_t>((t.replace + 256) / 257), voices.end(), [](std::size_t v) { return v == 0; }));
    CHECK(std::any_of(voices.begin(), voices.begin() + static_cast<std::ptrdiff_t>(t.replace / 257), [](std::size_t v) { return v > 0; }));
}
void controllerSaturation() {
    const Project p;
    const auto plan = prepare(p.project);
    RenderController controller(plan->plan());
    std::array<float, 2 * 257> block{};
    CHECK(controller.submit({ControlKind::start, {}, 1}));
    const auto batch = plan->scene(p.full, false); CHECK(batch);
    std::uint64_t serial = 2;
    while (controller.submit({ControlKind::scene, {}, serial, *batch, LaunchPolicy::immediate})) ++serial;
    CHECK(serial == 2 + Engine::command_capacity - 1); // the queue reports full instead of dropping
    controller.process(block, 2);
    CHECK(controller.status().running && controller.status().tracks[0].active_clip == 0);
    // Panic is delivered even with a full queue and leaves nothing playing.
    while (controller.submit({ControlKind::scene, {}, serial, *batch, LaunchPolicy::immediate})) ++serial;
    controller.panic(); controller.process(block, 2);
    CHECK(!controller.status().running && std::all_of(block.begin(), block.end(), [](float v) { return v == 0; }));
    for (std::size_t track = 0; track < 4; ++track) CHECK(controller.status().tracks[track].active_clip < 0);
    // Recovery: fresh controls are accepted and applied.
    CHECK(controller.submit({ControlKind::start, {}, serial + 1}));
    CHECK(controller.submit({ControlKind::scene, {}, serial + 2, *batch, LaunchPolicy::immediate}));
    controller.process(block, 2);
    CHECK(controller.status().handled_serial == serial + 2 && controller.status().tracks[3].active_clip == 0);
}
void deletionAndMissingMedia() {
    Project p;
    auto old = prepare(p.project);
    Engine engine(old->plan());
    CHECK(engine.schedule(*old->launch(p.bassClip, 0)) == ScheduleResult::accepted);
    engine.start();
    std::array<float, 2 * 512> block{};
    CHECK(engine.process(block, 2) && engine.active_voices() == 1);
    // Delete the playing track; the old plan and its notes stay valid until
    // the render owner stops, then nothing is left sounding.
    SessionDocument document(p.project.identities);
    document.deleteTrack(p.bass);
    auto next = p.project;
    next.identities = document.snapshot();
    std::erase_if(next.content.tracks, [&](const auto& t) { return t.track == p.bass; });
    std::erase_if(next.content.clips, [&](const auto& c) { return c.clip == p.bassClip; });
    const auto replaced = prepare(next);
    CHECK(!replaced->launch(p.bassClip, 0) && !replaced->trackIndex(p.bass));
    CHECK(std::none_of(replaced->document().scenes.begin(), replaced->document().scenes.end(),
        [&](const auto& s) { return std::any_of(s.entries.begin(), s.entries.end(), [&](auto e) { return e.track == p.bass; }); }));
    CHECK(engine.process(block, 2) && engine.active_voices() == 1);
    engine.stop(); CHECK(engine.active_voices() == 0);
    Engine fresh(replaced->plan()); fresh.start();
    CHECK(fresh.process(block, 2) && fresh.active_voices() == 0 && std::all_of(block.begin(), block.end(), [](float v) { return v == 0; }));
    // Missing media keeps the project valid and renders the track silent.
    auto missing = p.project;
    std::find_if(missing.content.clips.begin(), missing.content.clips.end(), [&](const auto& c) { return c.clip == p.loopClip; })->media.reset();
    const auto silent = prepare(missing);
    Engine quiet(silent->plan());
    CHECK(quiet.schedule(*silent->launch(p.loopClip, 0)) == ScheduleResult::accepted);
    quiet.start(); CHECK(quiet.process(block, 2) && std::all_of(block.begin(), block.end(), [](float v) { return v == 0; }));
    CHECK(quiet.track_statuses()[2].active_clip == 0); // launched, looping silence
    // Invalid content is refused before any plan replaces the current one.
    const auto bad = [&](auto edit) { auto candidate = p.project; edit(candidate); rejects([&] { (void)prepare(candidate); }); };
    bad([&](AuthoredProject& c) { std::find_if(c.content.clips.begin(), c.content.clips.end(), [&](const auto& x) { return x.clip == p.loopClip; })->range_start = 7; });
    bad([&](AuthoredProject& c) { std::find_if(c.content.clips.begin(), c.content.clips.end(), [&](const auto& x) { return x.clip == p.loopClip; })->asset.clear(); });
    bad([&](AuthoredProject& c) { c.content.clips.pop_back(); });
    bad([&](AuthoredProject& c) { c.content.tracks[0].level = 101; });
    bad([&](AuthoredProject& c) {
        auto media = std::make_shared<AudioClip>(*std::find_if(c.content.clips.begin(), c.content.clips.end(), [&](const auto& x) { return x.clip == p.loopClip; })->media);
        media->samples[2] = std::numeric_limits<float>::quiet_NaN(); // inside the played range
        std::find_if(c.content.clips.begin(), c.content.clips.end(), [&](const auto& x) { return x.clip == p.loopClip; })->media = media;
    });
    CHECK(old->launch(p.bassClip, 0)); // the earlier plan is untouched by refusals
}
}
int main() {
    try {
        blockInvariance(); controllerSaturation(); deletionAndMissingMedia();
        std::cout << "PASS: general-session scheduling at blocks 1/64/128/257/512 and 137 BPM, saturation/panic recovery, deletion and missing media\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
