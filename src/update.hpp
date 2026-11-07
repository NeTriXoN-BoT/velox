// Self-update of Velox.exe.
//
// A release publishes a small text manifest (made by gaming-phone/release/make-release.ps1):
//
//     velox-update 1
//     version=1.2.0
//     url=https://github.com/<owner>/<repo>/releases/download/v1.2.0/Velox.exe
//     size=1234567
//     sha256=<64 hex digits>
//     notes=What's new\nOne line per change
//     signature=<128 hex digits>
//
// The signature is ECDSA P-256 over every byte before the "signature=" line, made with the
// publisher's private key; the launcher only holds the public key. A manifest that doesn't verify
// is ignored, and the downloaded exe must match its size and SHA-256 before it replaces anything.
#pragma once

#include "common.hpp"

namespace phone::update {

struct Info {
    std::wstring version, notes, url;
    std::string sha256;
    uint64_t size = 0;
};

bool configured();  // this build knows where to look for updates
bool check(Info& info, std::wstring& error);
bool isNewer(const std::wstring& offered, const std::wstring& current);
// Downloads the new exe next to the running one and verifies it. Applied by applyPending().
bool download(const Info& info, const std::function<void(float)>& progress, std::wstring& error);
bool pendingReady(std::wstring* version = nullptr);
// Swaps in a downloaded update and starts it. Returns true if this process should exit now.
bool applyPending();
void cleanup();  // removes the previous exe left over after an update

}  // namespace phone::update
