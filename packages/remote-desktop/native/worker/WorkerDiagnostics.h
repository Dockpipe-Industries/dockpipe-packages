#pragma once

#include "WorkerProtocol.h"
#include <mutex>
#include <string_view>

namespace dockpipe::desktop {
enum class WorkerStage {
    Entry, Profile, Instance, ModuleImage, Manifest, ImageBinding, Owner, Notifications,
    Permissions, BridgeStart, BridgeReady, Policy, Directive, Identity, ChannelStart,
    Renewal, ChannelReady, NativeStart, ChannelRead, WriterStop, NativeStop,
    ChildStop, Report, Forget, BridgeStop, Complete
};

enum class WorkerFailure {
    None, InvalidEntry, InvalidProfile, Unexpected, InvalidImage, InvalidManifest,
    InvalidOwner, NativeEnvironment, BridgeStart, BridgeReadiness, PolicyChanged,
    InvalidDirective, IdentityFailure, ChannelStart, RenewalFailure, ChannelAuthentication,
    NativeStart, InvalidChannel, WriterStop, CleanupUnverified, ReportFailed,
    ForgetUnverified, BridgeUnverified
};

inline const char *workerStageName(WorkerStage stage)
{
    switch (stage) {
    case WorkerStage::Entry: return "entry";
    case WorkerStage::Profile: return "profile";
    case WorkerStage::Instance: return "instance";
    case WorkerStage::ModuleImage: return "module-image";
    case WorkerStage::Manifest: return "manifest";
    case WorkerStage::ImageBinding: return "image-binding";
    case WorkerStage::Owner: return "owner";
    case WorkerStage::Notifications: return "notifications";
    case WorkerStage::Permissions: return "permissions";
    case WorkerStage::BridgeStart: return "bridge-start";
    case WorkerStage::BridgeReady: return "bridge-ready";
    case WorkerStage::Policy: return "policy";
    case WorkerStage::Directive: return "directive";
    case WorkerStage::Identity: return "identity";
    case WorkerStage::ChannelStart: return "channel-start";
    case WorkerStage::Renewal: return "renewal";
    case WorkerStage::ChannelReady: return "channel-ready";
    case WorkerStage::NativeStart: return "native-start";
    case WorkerStage::ChannelRead: return "channel-read";
    case WorkerStage::WriterStop: return "writer-stop";
    case WorkerStage::NativeStop: return "native-stop";
    case WorkerStage::ChildStop: return "child-stop";
    case WorkerStage::Report: return "report";
    case WorkerStage::Forget: return "forget";
    case WorkerStage::BridgeStop: return "bridge-stop";
    case WorkerStage::Complete: return "complete";
    }
    return "unexpected";
}

inline WorkerFailure workerStageFailure(WorkerStage stage)
{
    switch (stage) {
    case WorkerStage::Entry: return WorkerFailure::InvalidEntry;
    case WorkerStage::Profile: return WorkerFailure::InvalidProfile;
    case WorkerStage::ModuleImage:
    case WorkerStage::ImageBinding: return WorkerFailure::InvalidImage;
    case WorkerStage::Manifest: return WorkerFailure::InvalidManifest;
    case WorkerStage::Owner: return WorkerFailure::InvalidOwner;
    case WorkerStage::Notifications:
    case WorkerStage::Permissions: return WorkerFailure::NativeEnvironment;
    case WorkerStage::BridgeStart: return WorkerFailure::BridgeStart;
    case WorkerStage::BridgeReady: return WorkerFailure::BridgeReadiness;
    case WorkerStage::Policy: return WorkerFailure::PolicyChanged;
    case WorkerStage::Directive: return WorkerFailure::InvalidDirective;
    case WorkerStage::Identity: return WorkerFailure::IdentityFailure;
    case WorkerStage::ChannelStart: return WorkerFailure::ChannelStart;
    case WorkerStage::Renewal: return WorkerFailure::RenewalFailure;
    case WorkerStage::ChannelReady: return WorkerFailure::ChannelAuthentication;
    case WorkerStage::NativeStart: return WorkerFailure::NativeStart;
    case WorkerStage::ChannelRead: return WorkerFailure::InvalidChannel;
    case WorkerStage::WriterStop: return WorkerFailure::WriterStop;
    case WorkerStage::NativeStop:
    case WorkerStage::ChildStop: return WorkerFailure::CleanupUnverified;
    case WorkerStage::Report: return WorkerFailure::ReportFailed;
    case WorkerStage::Forget: return WorkerFailure::ForgetUnverified;
    case WorkerStage::BridgeStop: return WorkerFailure::BridgeUnverified;
    default: return WorkerFailure::Unexpected;
    }
}

inline const char *workerFailureName(WorkerFailure failure)
{
    switch (failure) {
    case WorkerFailure::None: return "none";
    case WorkerFailure::InvalidEntry: return "invalid-entry";
    case WorkerFailure::InvalidProfile: return "invalid-profile";
    case WorkerFailure::InvalidImage: return "invalid-image";
    case WorkerFailure::InvalidManifest: return "invalid-manifest";
    case WorkerFailure::InvalidOwner: return "invalid-owner";
    case WorkerFailure::NativeEnvironment: return "native-environment";
    case WorkerFailure::BridgeStart: return "bridge-start";
    case WorkerFailure::BridgeReadiness: return "bridge-readiness";
    case WorkerFailure::PolicyChanged: return "policy-changed";
    case WorkerFailure::InvalidDirective: return "invalid-directive";
    case WorkerFailure::IdentityFailure: return "identity-failure";
    case WorkerFailure::ChannelStart: return "channel-start";
    case WorkerFailure::RenewalFailure: return "renewal-failure";
    case WorkerFailure::ChannelAuthentication: return "channel-authentication";
    case WorkerFailure::NativeStart: return "native-start";
    case WorkerFailure::InvalidChannel: return "invalid-channel";
    case WorkerFailure::WriterStop: return "writer-stop";
    case WorkerFailure::CleanupUnverified: return "cleanup-unverified";
    case WorkerFailure::ReportFailed: return "report-failed";
    case WorkerFailure::ForgetUnverified: return "forget-unverified";
    case WorkerFailure::BridgeUnverified: return "bridge-unverified";
    case WorkerFailure::Unexpected: return "unexpected";
    }
    return "unexpected";
}

class WorkerDiagnostics final {
public:
    void stage(WorkerStage value)
    {
        std::lock_guard lock(m_mutex);
        m_stage = value;
    }

    void failure()
    {
        std::lock_guard lock(m_mutex);
        if (m_failure == WorkerFailure::None) {
            m_failureStage = m_stage;
            m_failure = workerStageFailure(m_stage);
        }
    }

    bool unexpected() const
    {
        std::lock_guard lock(m_mutex);
        return m_failure == WorkerFailure::Unexpected;
    }

    void unexpectedHash(std::string_view value)
    {
        require(value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string_view::npos,
            "Invalid diagnostic digest.");
        std::lock_guard lock(m_mutex);
        m_unexpectedHash = value;
    }

    void nativeResult(const Result &result)
    {
        std::lock_guard lock(m_mutex);
        m_nativeCallback = true;
        if (!m_nativeUnknown) {
            m_nativeVerified = result.cleanupVerified;
            m_components = result.cleanup;
        }
        if (!result.cleanupVerified)
            m_nativeUnknown = true;
    }

    void writerJoined()
    {
        set(m_writerJoined, true);
    }

    void childStopped(bool value)
    {
        std::lock_guard lock(m_mutex);
        m_childVerified = m_childAttempted ? m_childVerified && value : value;
        m_childAttempted = true;
    }

    void reportSent()
    {
        set(m_reportSent, true);
    }

    void forgetVerified()
    {
        set(m_forgetVerified, true);
    }

    void bridgeStopped(bool value)
    {
        std::lock_guard lock(m_mutex);
        m_bridgeVerified = m_bridgeAttempted ? m_bridgeVerified && value : value;
        m_bridgeAttempted = true;
    }


    Json snapshot(bool cleanupVerified) const
    {
        std::lock_guard lock(m_mutex);
        const auto component = [](CleanupComponent value) {
            return Json{{"known", value.known}, {"verified", value.verified}};
        };
        Json value{{"schema", "dockpipe.desktop.native.diagnostic/v1"},
            {"stage", workerStageName(m_stage)}, {"failure_stage", workerStageName(m_failureStage)},
            {"reason", workerFailureName(m_failure)}, {"cleanup_verified", cleanupVerified},
            {"writer_joined", m_writerJoined}, {"native_callback", m_nativeCallback},
            {"native_verified", m_nativeVerified && !m_nativeUnknown}, {"native_unknown_observed", m_nativeUnknown},
            {"capture", component(m_components.capture)}, {"encoder", component(m_components.encoder)},
            {"input_release", component(m_components.input)}, {"child_attempted", m_childAttempted},
            {"child_verified", m_childVerified},
            {"report_sent", m_reportSent}, {"forget_verified", m_forgetVerified},
            {"bridge_attempted", m_bridgeAttempted}, {"bridge_verified", m_bridgeVerified}};
        if (!m_unexpectedHash.empty())
            value["unexpected_sha256"] = m_unexpectedHash;
        return value;
    }

private:
    void set(bool &field, bool value)
    {
        std::lock_guard lock(m_mutex);
        field = value;
    }

    mutable std::mutex m_mutex;
    WorkerStage m_stage = WorkerStage::Entry;
    WorkerStage m_failureStage = WorkerStage::Entry;
    WorkerFailure m_failure = WorkerFailure::None;
    CleanupSnapshot m_components;
    bool m_writerJoined = false;
    bool m_nativeCallback = false;
    bool m_nativeVerified = false;
    bool m_nativeUnknown = false;
    bool m_childAttempted = false;
    bool m_childVerified = false;
    bool m_reportSent = false;
    bool m_forgetVerified = false;
    bool m_bridgeAttempted = false;
    bool m_bridgeVerified = false;
    std::string m_unexpectedHash;
};

std::string diagnosticMessageHash(const char *message);
} // namespace dockpipe::desktop
