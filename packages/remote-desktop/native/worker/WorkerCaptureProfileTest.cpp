#include "WorkerProtocol.h"

#include <cassert>
#include <iostream>
#include <limits>

using namespace dockpipe::desktop;

namespace {
template<typename Action>
bool rejected(Action action)
{
    try {
        action();
    } catch (const std::exception &) {
        return true;
    }
    return false;
}

WorkerProtocol connecting()
{
    const Json provider{{"id", "dockpipe.desktop.native"}, {"version", "0.1.0"}, {"artifact", std::string(64, 'a')}};
    const Json catalog{{"schema", "dockpipe.remote.desktop/v1"}, {"provider", provider}, {"epoch", std::string(64, 'd')},
        {"target", Json{{"node", "fixture"}, {"fingerprint", std::string(64, 'b')}, {"generation", std::string(64, 'c')}}},
        {"view", true}, {"input", false}};
    const Json open{{"schema", "dockpipe.remote.desktop/v1"}, {"provider", provider}, {"epoch", catalog["epoch"]},
        {"target", catalog["target"]}, {"id", std::string(64, '1')}, {"nonce", std::string(64, '2')},
        {"deadline", "2030-01-01T00:00:00Z"}, {"mode", "view"}};
    WorkerProtocol protocol(catalog, provider);
    auto directive = Json{{"open", open}, {"lease_millis", 10000}, {"close", false}, {"ack", 0}};
    protocol.directive(directive, Clock::now());
    protocol.localPin(std::string(64, '3'));
    directive["ack"] = 1;
    directive["signals"] = Json::array({Json{{"reference", protocol.reference()}, {"sequence", 1},
        {"type", "binding"}, {"fingerprint", std::string(64, '4')}}});
    protocol.directive(directive, Clock::now());
    protocol.connecting();
    return protocol;
}

Json ready(const WorkerProtocol &protocol, const Json &capture)
{
    return {{"type", "ready"}, {"reference", protocol.reference()}, {"role", "worker"}, {"mode", "view"}, {"capture", capture}};
}
}

int main()
{
    assert(!CaptureProfile{}.clipboardText);
    assert(parseCaptureProfile(Json{{"quality", "high"}, {"frame_rate", 30}, {"clipboard_text", true}}).clipboardText);
    for (const auto &invalid : {Json(false), Json(nullptr), Json(1), Json("true")})
        assert(rejected([&] { parseCaptureProfile(Json{{"quality", "high"}, {"frame_rate", 30}, {"clipboard_text", invalid}}); }));
    auto viewOnly = connecting();
    assert(rejected([&] { viewOnly.ready(ready(viewOnly, Json{{"quality", "high"}, {"frame_rate", 30}, {"clipboard_text", true}})); }));
    const auto defaultPreset = capturePreset(CaptureProfile{});
    assert(defaultPreset && defaultPreset->maximumWidth == 1920 && defaultPreset->maximumHeight == 1080
        && defaultPreset->bitsPerSecond == 4000000 && CaptureProfile{}.frameRate == 30
        && CaptureProfile{}.displaySource == DisplaySource::Existing);
    const CaptureQuality qualities[]{CaptureQuality::Low, CaptureQuality::Balanced, CaptureQuality::High, CaptureQuality::Ultra};
    const char *names[]{"low", "balanced", "high", "ultra"};
    const unsigned widths[]{1280, 1600, 1920, 3840};
    const unsigned heights[]{720, 900, 1080, 2160};
    const unsigned bitrates[]{1000000, 2500000, 4000000, 16000000};
    for (unsigned quality = 0; quality < 4; ++quality) {
        for (unsigned rate : {10, 15, 24, 30, 60}) {
            const auto profile = parseCaptureProfile(Json{{"quality", names[quality]}, {"frame_rate", rate}});
            assert(profile.quality == qualities[quality] && profile.frameRate == rate);
            const auto codec = captureCodecBounds(profile);
            unsigned expectedLevel = rate == 60 ? 42 : 40;
            if (quality == 3)
                expectedLevel = rate == 60 ? 52 : 51;
            assert(codec && codec->maximumLevel == expectedLevel && codec->maximumWidth == widths[quality]
                && codec->maximumHeight == heights[quality]);
            const auto preset = capturePreset(profile);
            assert(preset && preset->maximumWidth == widths[quality] && preset->maximumHeight == heights[quality]
                && preset->bitsPerSecond == bitrates[quality]);
            const auto geometry = captureGeometry(profile, 3840, 2160);
            assert(geometry && geometry->width == widths[quality] && geometry->height == heights[quality]);
            const auto small = captureGeometry(profile, 321, 241);
            assert(small && small->width == 320 && small->height == 240);
            const auto portrait = captureGeometry(profile, 2160, 3840);
            assert(portrait && portrait->height == heights[quality] && portrait->width <= widths[quality]
                && portrait->width % 2 == 0 && portrait->height % 2 == 0);
            auto protocol = connecting();
            assert(rejected([&] { protocol.capture(); }));
            protocol.ready(ready(protocol, Json{{"quality", names[quality]}, {"frame_rate", rate}}));
            assert(protocol.capture().quality == qualities[quality] && protocol.capture().frameRate == rate);
            assert(rejected([&] { protocol.ready(ready(protocol, Json{{"quality", "high"}, {"frame_rate", 30}})); }));
            protocol.running();
            const auto renewal = Json{{"open", protocol.open()}, {"lease_millis", 1000}, {"close", false}, {"ack", 1}};
            protocol.directive(renewal, Clock::now());
            assert(protocol.capture().quality == qualities[quality] && protocol.capture().frameRate == rate);
        }
    }
    for (unsigned quality = 0; quality < 4; ++quality) {
        for (unsigned rate : {10, 15, 24, 30, 60}) {
            const Json selected{{"quality", names[quality]}, {"frame_rate", rate}, {"display_source", "virtual"}};
            const auto profile = parseCaptureProfile(selected);
            assert(profile.displaySource == DisplaySource::Virtual && profile.quality == qualities[quality]
                && profile.frameRate == rate);
            const auto geometry = captureGeometry(profile, widths[quality], heights[quality]);
            assert(geometry && geometry->width == widths[quality] && geometry->height == heights[quality]);
            assert(!captureGeometry(profile, 321, 241));
            assert(!captureGeometry(profile, widths[quality] * 2, heights[quality] * 2));
            assert(!captureGeometry(profile, widths[quality] - 2, heights[quality]));
            const auto preset = capturePreset(profile);
            assert(preset && preset->maximumWidth == widths[quality] && preset->maximumHeight == heights[quality]);
            auto protocol = connecting();
            assert(rejected([&] { protocol.capture(); }));
            protocol.ready(ready(protocol, selected));
            assert(protocol.capture().displaySource == DisplaySource::Virtual);
            protocol.running();
            const auto renewal = Json{{"open", protocol.open()}, {"lease_millis", 1000}, {"close", false}, {"ack", 1}};
            protocol.directive(renewal, Clock::now());
            assert(protocol.capture().displaySource == DisplaySource::Virtual);
            assert(rejected([&] { protocol.ready(ready(protocol, Json{{"quality", names[quality]}, {"frame_rate", rate}})); }));
        }
    }
    for (const auto &invalid : std::vector<Json>{nullptr, Json::array(), Json::object(),
             Json{{"quality", "high"}}, Json{{"frame_rate", 30}},
             Json{{"quality", "extreme"}, {"frame_rate", 30}}, Json{{"quality", true}, {"frame_rate", 30}},
             Json{{"quality", "high"}, {"frame_rate", true}}, Json{{"quality", "high"}, {"frame_rate", 30.0}},
             Json{{"quality", "high"}, {"frame_rate", -1}}, Json{{"quality", "high"}, {"frame_rate", 120}},
             Json{{"quality", "high"}, {"frame_rate", std::numeric_limits<std::uint64_t>::max()}},
             Json{{"quality", "low"}, {"frame_rate", 10}, {"bitrate", 4000000}}}) {
        auto protocol = connecting();
        assert(rejected([&] { protocol.ready(ready(protocol, invalid)); }));
        assert(rejected([&] { protocol.capture(); }));
        assert(rejected([&] { protocol.running(); }));
    }
    for (const auto &field : {"type", "reference", "role", "mode", "capture"}) {
        auto protocol = connecting();
        auto value = ready(protocol, Json{{"quality", "high"}, {"frame_rate", 30}});
        value.erase(field);
        assert(rejected([&] { protocol.ready(value); }));
    }
    for (const auto &field : {"reference", "role", "mode", "extra"}) {
        auto protocol = connecting();
        auto value = ready(protocol, Json{{"quality", "high"}, {"frame_rate", 30}});
        value[field] = "foreign";
        assert(rejected([&] { protocol.ready(value); }));
    }
    for (const Json &invalidSource : {Json(nullptr), Json(false), Json(""), Json("existing"), Json("mirror"), Json::object()}) {
        auto protocol = connecting();
        const Json invalid{{"quality", "high"}, {"frame_rate", 30}, {"display_source", invalidSource}};
        assert(rejected([&] { protocol.ready(ready(protocol, invalid)); }));
        assert(rejected([&] { protocol.capture(); }));
    }
    assert(!capturePreset({CaptureQuality::High, 30, DisplaySource(99)}));
    assert(!capturePreset({CaptureQuality(99), 30}));
    assert(!capturePreset({CaptureQuality::High, 120}));
    for (double invalid : {0.0, 1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        assert(!captureGeometry({}, invalid, 1080));
        assert(!captureGeometry({}, 1920, invalid));
    }
    std::cout << "PASS 20 presets/FPS, bounded even aspect geometry, strict Ready/profile rejection, readiness fence and immutable renewal\n";
}
