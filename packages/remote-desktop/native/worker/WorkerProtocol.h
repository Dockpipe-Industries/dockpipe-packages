#pragma once

#include "NativeDesktopBackend.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace dockpipe::desktop {
using Json = nlohmann::json;

inline bool digest(const Json &value)
{
    if (!value.is_string())
        return false;
    const auto text = value.get<std::string>();
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}
inline void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
inline CaptureProfile parseCaptureProfile(const Json &value)
{
    const bool hasSource = value.contains("display_source");
    const bool hasClipboard = value.contains("clipboard_text");
    const bool hasImages = value.contains("clipboard_images");
    const bool hasFiles = value.contains("clipboard_files");
    const auto expectedFields = 2U + unsigned(hasSource) + unsigned(hasClipboard) + unsigned(hasImages) + unsigned(hasFiles);
    require(value.is_object() && value.size() == expectedFields
        && value.contains("quality") && value["quality"].is_string()
        && value.contains("frame_rate") && value["frame_rate"].is_number_integer(), "Invalid negotiated capture profile.");
    CaptureProfile profile;
    if (hasClipboard) {
        require(value["clipboard_text"].is_boolean() && value["clipboard_text"] == true,
            "Noncanonical negotiated clipboard capability.");
        profile.clipboardText = true;
    }
    if (hasImages) {
        require(value["clipboard_images"].is_boolean() && value["clipboard_images"] == true,
            "Noncanonical negotiated image clipboard capability.");
        profile.clipboardImages = true;
    }
    if (hasFiles) {
        require(value["clipboard_files"].is_boolean() && value["clipboard_files"] == true,
            "Noncanonical negotiated file clipboard capability.");
        profile.clipboardFiles = true;
    }
    const auto quality = value["quality"].get<std::string>();
    if (quality == "low")
        profile.quality = CaptureQuality::Low;
    else if (quality == "balanced")
        profile.quality = CaptureQuality::Balanced;
    else if (quality == "high")
        profile.quality = CaptureQuality::High;
    else if (quality == "ultra")
        profile.quality = CaptureQuality::Ultra;
    else
        throw std::runtime_error("Unsupported negotiated capture quality.");
    const auto &rate = value["frame_rate"];
    require(rate == 10 || rate == 15 || rate == 24 || rate == 30 || rate == 60, "Unsupported negotiated capture frame rate.");
    profile.frameRate = rate.get<unsigned>();
    if (hasSource) {
        require(value["display_source"].is_string() && value["display_source"] == "virtual",
            "Unsupported negotiated display source.");
        profile.displaySource = DisplaySource::Virtual;
    }
    return profile;
}

inline Json referenceOf(const Json &open)
{
    return Json{{"schema", open.at("schema")}, {"id", open.at("id")}, {"target", open.at("target")}, {"epoch", open.at("epoch")}};
}
inline bool validReference(const Json &reference)
{
    if (!reference.is_object() || reference.size() != 4 || reference.value("schema", "") != "dockpipe.remote.desktop/v1"
        || !reference.contains("id") || !digest(reference["id"]) || !reference.contains("epoch") || !digest(reference["epoch"]) || !reference.contains("target"))
        return false;
    const auto &target = reference["target"];
    if (!target.is_object() || target.size() != 3 || !target.contains("node") || !target["node"].is_string()
        || !target.contains("fingerprint") || !digest(target["fingerprint"]) || !target.contains("generation") || !digest(target["generation"]))
        return false;
    const auto node = target["node"].get<std::string>();
    return !node.empty() && node.size() <= 64 && node.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos
        && node.front() != '_' && node.front() != '-';
}

// This pure state owns exactly one original intent. Unknown cleanup never
// releases it or permits a new reference under the same bridge lifetime.
class WorkerProtocol final {
public:
    enum class Phase { Idle, Opening, Connecting, Running, Stopping, Closed, Unknown };
    WorkerProtocol(Json catalog, Json provider) : m_catalog(std::move(catalog)), m_provider(std::move(provider))
    {
        require(m_catalog.at("schema") == "dockpipe.remote.desktop/v1" && m_catalog.at("provider") == m_provider
            && digest(m_catalog.at("epoch")), "Invalid native worker catalog.");
        const Json probe{{"schema", "dockpipe.remote.desktop/v1"}, {"id", std::string(64, '0')}, {"target", m_catalog.at("target")}, {"epoch", m_catalog.at("epoch")}};
        require(validReference(probe), "Invalid worker target identity.");
    }
    bool directive(const Json &value, Clock::time_point now)
    {
        const auto &open = value.at("open");
        const auto reference = referenceOf(open);
        require(validReference(reference) && reference.at("target") == m_catalog.at("target") && reference.at("epoch") == m_catalog.at("epoch")
            && open.at("provider") == m_provider && digest(open.at("nonce")), "Desktop directive changed frozen authority.");
        const auto mode = open.at("mode").get<std::string>();
        require((mode == "view" || mode == "control") && m_catalog.at("view").get<bool>()
            && (mode != "control" || m_catalog.at("input").get<bool>()), "Desktop mode exceeds worker capability.");
        require(value.at("lease_millis").is_number_integer(), "Native lease must be an integer.");
        const auto remaining = value.at("lease_millis").get<std::int64_t>();
        require(remaining >= 0 && remaining <= 10000, "Invalid native lease interval.");
        const bool close = value.at("close").get<bool>();
        const auto acknowledgement = value.at("ack").get<std::uint64_t>();
        require(acknowledgement >= m_outgoingAck && acknowledgement <= (m_localPin.empty() ? 0U : 1U), "Desktop binding acknowledgment changed.");
        bool created = false;
        if (m_phase == Phase::Idle) {
            m_open = open;
            m_reference = reference;
            m_phase = Phase::Opening;
            created = true;
        } else {
            require(m_open == open && m_reference == reference, "Desktop original intent changed or was replayed.");
        }
        m_outgoingAck = acknowledgement;
        if (close || remaining == 0) {
            if (m_phase != Phase::Closed && m_phase != Phase::Unknown)
                m_phase = Phase::Stopping;
            return created;
        }
        require(m_phase != Phase::Closed && m_phase != Phase::Unknown && m_phase != Phase::Stopping, "Closed desktop cannot be renewed.");
        m_lease = now + std::chrono::milliseconds(remaining);
        if (value.contains("signals")) {
            require(value["signals"].is_array() && value["signals"].size() <= 1, "Unsupported desktop signaling set.");
            for (const auto &signal : value["signals"]) {
                require(signal.at("reference") == m_reference && signal.at("sequence") == 1 && signal.at("type") == "binding"
                    && digest(signal.at("fingerprint")), "Invalid viewer binding.");
                const auto peerPin = signal.at("fingerprint").get<std::string>();
                require(m_peerPin.empty() || m_peerPin == peerPin, "Viewer identity changed within one session.");
                m_peerPin = peerPin;
                m_incomingAck = 1;
            }
        }
        return created;
    }
    void localPin(std::string pin)
    {
        require(m_phase == Phase::Opening && m_localPin.empty() && digest(Json(pin)), "Local identity was reused.");
        m_localPin = std::move(pin);
    }
    bool canConnect() const { return m_phase == Phase::Opening && m_outgoingAck == 1 && m_incomingAck == 1 && !m_localPin.empty() && !m_peerPin.empty() && m_localPin != m_peerPin; }
    void connecting() { require(canConnect(), "Both authenticated bindings are required."); m_phase = Phase::Connecting; }
    void ready(const Json &value)
    {
        require(m_phase == Phase::Connecting && !m_encryptedReady && value.is_object() && value.size() == 5
            && value.at("type") == "ready" && value.at("reference") == m_reference
            && value.at("role") == "worker" && value.at("mode") == m_open.at("mode"), "Encrypted channel readiness changed authority.");
        m_capture = parseCaptureProfile(value.at("capture"));
        require(!(m_capture.clipboardText || m_capture.clipboardImages || m_capture.clipboardFiles)
            || m_open.at("mode") == "control",
            "Clipboard requires control authority.");
        m_encryptedReady = true;
    }
    void running() { require(m_phase == Phase::Connecting && m_encryptedReady, "Capture cannot precede channel readiness."); m_phase = Phase::Running; }
    void stop() { if (m_phase != Phase::Closed && m_phase != Phase::Unknown) m_phase = Phase::Stopping; }
    void cleanup(bool nativeVerified, bool childReaped) { m_phase = nativeVerified && childReaped ? Phase::Closed : Phase::Unknown; }
    bool expired(Clock::time_point now) const { return m_phase != Phase::Idle && now >= m_lease; }
    Json report() const
    {
        const char *state = "opening";
        switch (m_phase) {
        case Phase::Running: state = "running"; break;
        case Phase::Closed: state = "closed"; break;
        case Phase::Unknown: state = "unknown"; break;
        default: break;
        }
        Json value{{"reference", m_reference}, {"state", state}, {"ack", m_incomingAck}, {"signals", Json::array()}};
        if (m_phase != Phase::Closed && m_phase != Phase::Unknown && m_outgoingAck == 0 && !m_localPin.empty())
            value["signals"] = Json::array({Json{{"reference", m_reference}, {"sequence", 1}, {"type", "binding"}, {"fingerprint", m_localPin}}});
        return Json{{"type", "report"}, {"report", value}};
    }
    Phase phase() const { return m_phase; }
    const Json &reference() const { return m_reference; }
    const Json &open() const { return m_open; }
    const std::string &localPin() const { return m_localPin; }
    const std::string &peerPin() const { return m_peerPin; }
    Clock::time_point lease() const { return m_lease; }
    CaptureProfile capture() const
    {
        require(m_encryptedReady, "Capture profile is unavailable before authenticated readiness.");
        return m_capture;
    }

private:
    bool m_encryptedReady = false;
    CaptureProfile m_capture;
    Json m_catalog;
    Json m_provider;
    Json m_open;
    Json m_reference;
    Phase m_phase = Phase::Idle;
    std::string m_localPin;
    std::string m_peerPin;
    std::uint64_t m_incomingAck = 0;
    std::uint64_t m_outgoingAck = 0;
    Clock::time_point m_lease{};
};

inline std::int64_t boundedInteger(const Json &value, std::int64_t minimum, std::int64_t maximum)
{
    require(value.is_number_integer(), "Native input integer required.");
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        require(number <= static_cast<std::uint64_t>(maximum), "Native input integer exceeds range.");
        return static_cast<std::int64_t>(number);
    }
    const auto number = value.get<std::int64_t>();
    require(number >= minimum && number <= maximum, "Native input integer exceeds range.");
    return number;
}

inline InputEvent parseInput(const Json &value)
{
    require(value.is_object() && value.size() <= 9 && value.at("sequence").is_number_unsigned(), "Invalid input envelope.");
    InputEvent event;
    event.sequence = value.at("sequence").get<std::uint64_t>();
    const auto kind = value.at("kind").get<std::string>();
    if (kind == "key") {
        require(value.size() == 4, "Unexpected physical key fields.");
        const auto usage = boundedInteger(value.at("hid_usage"), 0, 255);
        require(usage <= 255, "Unsupported physical key.");
        event.kind = InputKind::Key;
        event.hidUsage = static_cast<std::uint16_t>(usage);
        event.down = value.at("down").get<bool>();
    } else {
        event.x = value.at("x").get<double>();
        event.y = value.at("y").get<double>();
        if (kind == "move") {
            require(value.size() == 4, "Unexpected pointer fields.");
            event.kind = InputKind::PointerMove;
        } else if (kind == "button") {
            require(value.size() == 6, "Unexpected button fields.");
            const auto button = boundedInteger(value.at("button"), 0, 2);
            require(button <= 2, "Unsupported physical button.");
            event.kind = InputKind::Button;
            event.button = static_cast<std::uint8_t>(button);
            event.down = value.at("down").get<bool>();
        } else if (kind == "scroll") {
            require(value.size() == 6, "Unexpected scroll fields.");
            const auto x = boundedInteger(value.at("scroll_x"), -32768, 32767);
            const auto y = boundedInteger(value.at("scroll_y"), -32768, 32767);
            require(x >= -32768 && x <= 32767 && y >= -32768 && y <= 32767, "Scroll exceeds native range.");
            event.kind = InputKind::Scroll;
            event.scrollX = static_cast<std::int16_t>(x);
            event.scrollY = static_cast<std::int16_t>(y);
        } else {
            throw std::runtime_error("Unsupported native input kind.");
        }
    }
    return event;
}
} // namespace dockpipe::desktop
