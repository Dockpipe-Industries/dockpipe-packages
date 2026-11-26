#pragma once
#include "../AssetClipboardTransfer.h"

namespace dockpipe::desktop {
void validateNativeAssetNames(const std::vector<AssetEntry> &entries);
std::unique_ptr<AssetDigest> makeNativeAssetDigest();
std::array<std::uint8_t, 16> nativeAssetTransferIdentifier();
PreparedClipboardPng normalizeNativeClipboardPng(const std::vector<std::uint8_t> &bytes);
// Main-thread operations only; construction does not inspect the pasteboard.
std::int64_t nativeAssetClipboardGeneration();
LocalAssetSelection readNativeAssetClipboard(bool images, bool files);
bool publishNativeAssetClipboard(const AssetClipboardRecord &offer, const AssetSnapshot &snapshot,
    const std::vector<std::uint8_t> &verifiedPng, std::int64_t expectedGeneration,
    const std::function<bool()> &admitted);
}
