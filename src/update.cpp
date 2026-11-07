#include "update.hpp"
#include "instances.hpp"

#include <bcrypt.h>

#include <cstdio>

#include "net.hpp"
#include "phone.hpp"
#include "update_config.h"
#include "update_key.h"

namespace phone::update {
namespace {

constexpr uint64_t MAX_MANIFEST = 64 * 1024, MAX_EXE = 64ull << 20;

bool unhex(const std::string &s, std::vector<unsigned char> &out) {
    if (s.size() % 2)
        return false;
    out.clear();
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = nib(s[i]), b = nib(s[i + 1]);
        if (a < 0 || b < 0)
            return false;
        out.push_back(static_cast<unsigned char>(a * 16 + b));
    }
    return true;
}

bool signatureValid(const std::string &data, const std::vector<unsigned char> &sig) {
    if (kUpdatePublicKeySize == 0 || sig.size() != 64)
        return false;
    std::vector<unsigned char> digest;
    if (!unhex(net::hashBytes(data.data(), data.size(), net::Hash::Sha256), digest) || digest.size() != 32)
        return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, const_cast<PUCHAR>(kUpdatePublicKey), kUpdatePublicKeySize, 0) ==
            0) {
            ok = BCryptVerifySignature(key, nullptr, digest.data(), 32, const_cast<PUCHAR>(sig.data()), 64, 0) == 0;
            BCryptDestroyKey(key);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return ok;
}

std::vector<int> parts(const std::wstring &v) {
    std::vector<int> p;
    int cur = 0;
    bool any = false;
    for (wchar_t c : v) {
        if (c >= L'0' && c <= L'9') {
            if (cur > (65535 - (c - L'0')) / 10)
                return {};
            cur = cur * 10 + (c - L'0');
            any = true;
        } else if (c == L'.' && any && p.size() < 2) {
            p.push_back(cur);
            cur = 0;
            any = false;
        } else {
            return {};
        }
    }
    if (any && p.size() == 2)
        p.push_back(cur);
    else
        return {};
    return p;
}

std::wstring newPath() {
    return exePath() + L".new";
}
std::wstring oldPath() {
    return exePath() + L".old";
}

} // namespace

bool configured() {
    return wcslen(GP_UPDATE_URL) > 0 && kUpdatePublicKeySize > 0;
}

bool isNewer(const std::wstring &offered, const std::wstring &current) {
    std::vector<int> a = parts(offered), b = parts(current);
    if (a.size() != 3 || b.size() != 3)
        return false;
    for (size_t i = 0; i < std::max(a.size(), b.size()); i++) {
        int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y)
            return x > y;
    }
    return false;
}

bool check(Info &info, std::wstring &error) {
    if (!configured()) {
        error = L"updates aren't set up for this copy";
        return false;
    }
    std::string text;
    if (!net::getText(GP_UPDATE_URL, MAX_MANIFEST, text, error))
        return false;
    const std::string head = "velox-update 1\n";
    size_t sigAt = text.find("\nsignature=");
    if (text.compare(0, head.size(), head) != 0 || sigAt == std::string::npos) {
        error = L"the update information isn't in the expected format";
        return false;
    }
    std::string sigHex = text.substr(sigAt + 11);
    while (!sigHex.empty() && (sigHex.back() == '\n' || sigHex.back() == '\r' || sigHex.back() == ' '))
        sigHex.pop_back();
    std::vector<unsigned char> sig;
    if (!unhex(sigHex, sig) || !signatureValid(text.substr(0, sigAt + 1), sig)) {
        error = L"the update isn't signed by the publisher";
        return false;
    }
    Info in;
    size_t pos = head.size();
    while (pos < sigAt + 1) {
        size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "version")
            in.version = widen(v);
        else if (k == "url")
            in.url = widen(v);
        else if (k == "size")
            in.size = std::strtoull(v.c_str(), nullptr, 10);
        else if (k == "sha256")
            in.sha256 = v;
        else if (k == "notes") {
            for (size_t at; (at = v.find("\\n")) != std::string::npos;)
                v.replace(at, 2, "\n");
            in.notes = widen(v);
        }
    }
    std::vector<unsigned char> digest;
    for (char &c : in.sha256)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (parts(in.version).empty() || in.url.rfind(L"https://", 0) != 0 || in.size == 0 || in.size > MAX_EXE || !unhex(in.sha256, digest) ||
        digest.size() != 32) {
        error = L"the update information is incomplete";
        return false;
    }
    info = std::move(in);
    return true;
}

bool download(const Info &info, const std::function<void(float)> &progress, std::wstring &error) {
    std::wstring target = newPath();
    bool ok = net::download(
        info.url, target, info.size, info.sha256, net::Hash::Sha256,
        [&](uint64_t d, uint64_t t) {
            if (progress && t)
                progress(static_cast<float>(d) / static_cast<float>(t));
        },
        nullptr, error);
    if (!ok) {
        if (error == L"the download folder is not writable")
            error = L"this folder can't be written to, so updates can't be installed here";
        return false;
    }
    WritePrivateProfileStringW(L"update", L"pending", info.version.c_str(), iniPath().c_str());
    WritePrivateProfileStringW(L"update", L"sha256", widen(info.sha256).c_str(), iniPath().c_str());
    return true;
}

bool pendingReady(std::wstring *version) {
    wchar_t v[32] = L"", sha[80] = L"";
    GetPrivateProfileStringW(L"update", L"pending", L"", v, 32, iniPath().c_str());
    GetPrivateProfileStringW(L"update", L"sha256", L"", sha, 80, iniPath().c_str());
    if (!*v || !exists(newPath()) || !isNewer(v, GP_VERSION_WSTRING))
        return false;
    if (version)
        *version = v;
    return true;
}

bool applyPending() {
    if (!instances::id().empty() || instances::otherLaunchers())
        return false;
    wchar_t sha[80] = L"";
    GetPrivateProfileStringW(L"update", L"sha256", L"", sha, 80, iniPath().c_str());
    std::wstring v;
    if (!pendingReady(&v))
        return false;
    // Check the file once more right before it's used.
    if (widen(net::hashFile(newPath(), net::Hash::Sha256)) != sha) {
        DeleteFileW(newPath().c_str());
        WritePrivateProfileStringW(L"update", nullptr, nullptr, iniPath().c_str());
        return false;
    }
    DeleteFileW(oldPath().c_str());
    if (!MoveFileExW(exePath().c_str(), oldPath().c_str(), MOVEFILE_REPLACE_EXISTING))
        return false;
    if (!MoveFileExW(newPath().c_str(), exePath().c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(oldPath().c_str(), exePath().c_str(), MOVEFILE_REPLACE_EXISTING); // put the old one back
        return false;
    }
    WritePrivateProfileStringW(L"update", nullptr, nullptr, iniPath().c_str());
    WritePrivateProfileStringW(L"update", L"justupdated", v.c_str(), iniPath().c_str());
    std::wstring cmd = L"\"" + exePath() + L"\" --after-update";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, exeFolder().c_str(), &si, &pi)) {
        // Couldn't start the new version: go back to this one.
        MoveFileExW(exePath().c_str(), newPath().c_str(), MOVEFILE_REPLACE_EXISTING);
        MoveFileExW(oldPath().c_str(), exePath().c_str(), MOVEFILE_REPLACE_EXISTING);
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void cleanup() {
    for (int i = 0; i < 20 && exists(oldPath()) && !DeleteFileW(oldPath().c_str()); i++)
        Sleep(250);
}

} // namespace phone::update
