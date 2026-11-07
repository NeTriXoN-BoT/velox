#include <atomic>

#include "net.hpp"

#include <bcrypt.h>
#include <winhttp.h>

namespace phone::net {
namespace {

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h)
            WinHttpCloseHandle(h);
    }
};

std::wstring systemError(DWORD code) {
    if (code == ERROR_WINHTTP_TIMEOUT)
        return L"the connection timed out";
    if (code == ERROR_WINHTTP_NAME_NOT_RESOLVED || code == ERROR_WINHTTP_CANNOT_CONNECT)
        return L"no internet connection";
    if (code == ERROR_WINHTTP_SECURE_FAILURE)
        return L"the secure connection could not be checked";
    wchar_t buf[256] = L"";
    HMODULE mod = GetModuleHandleW(L"winhttp.dll");
    DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | (mod ? FORMAT_MESSAGE_FROM_HMODULE : 0);
    if (!FormatMessageW(flags, mod, code, 0, buf, static_cast<DWORD>(std::size(buf)), nullptr))
        swprintf(buf, std::size(buf), L"network error %lu", code);
    std::wstring s = buf;
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.'))
        s.pop_back();
    return s;
}

class Hasher {
  public:
    explicit Hasher(Hash kind) : len_(kind == Hash::Sha1 ? 20 : 32) {
        if (BCryptOpenAlgorithmProvider(&alg_, kind == Hash::Sha1 ? BCRYPT_SHA1_ALGORITHM : BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0)
            BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0);
    }
    ~Hasher() {
        if (hash_)
            BCryptDestroyHash(hash_);
        if (alg_)
            BCryptCloseAlgorithmProvider(alg_, 0);
    }
    bool ok() const { return hash_ != nullptr && good_; }
    void add(const void *d, size_t n) {
        auto *bytes = static_cast<const unsigned char *>(d);
        while (n && ok()) {
            ULONG count = static_cast<ULONG>(std::min<size_t>(n, 0x7fffffffu));
            if (BCryptHashData(hash_, const_cast<PUCHAR>(bytes), count, 0) != 0) {
                good_ = false;
                return;
            }
            bytes += count;
            n -= count;
        }
    }
    std::string hex() {
        if (!ok())
            return {};
        std::vector<unsigned char> out(len_);
        if (BCryptFinishHash(hash_, out.data(), static_cast<ULONG>(len_), 0) != 0)
            return {};
        static const char *digits = "0123456789abcdef";
        std::string s;
        for (unsigned char c : out) {
            s += digits[c >> 4];
            s += digits[c & 15];
        }
        return s;
    }

  private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    size_t len_;
    bool good_ = true;
};

} // namespace

bool get(const std::wstring &url, uint64_t maxBytes, uint64_t from, const std::function<bool(const char *, size_t, uint64_t)> &sink,
         std::wstring &error) {
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostName[256] = L"", path[2048] = L"";
    uc.lpszHostName = hostName;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(hostName));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        error = L"only secure (https) addresses are allowed";
        return false;
    }
    std::wstring agent = L"Velox/" GP_VERSION_WSTRING L" (Windows)";
    Handle session{WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) {
        error = systemError(GetLastError());
        return false;
    }
    // TLS 1.2+ only; redirects may not leave https.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS 1.3 */;
    if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
            error = L"secure TLS settings could not be configured";
            return false;
        }
    }
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(session.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect))) {
        error = L"secure redirect settings could not be configured";
        return false;
    }
    WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 60000);
    Handle conn{WinHttpConnect(session.h, hostName, uc.nPort, 0)};
    Handle req{
        conn.h ? WinHttpOpenRequest(conn.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
               : nullptr};
    std::wstring headers;
    if (from > 0)
        headers = L"Range: bytes=" + std::to_wstring(from) + L"-\r\n";
    if (!req.h ||
        !WinHttpSendRequest(req.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        error = systemError(GetLastError());
        return false;
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                        WINHTTP_NO_HEADER_INDEX);
    if (from > 0 && status == 200) {
        error = L"resume not supported"; // the caller starts again from zero
        return false;
    }
    if (status != 200 && status != 206) {
        error = L"the server answered " + std::to_wstring(status);
        return false;
    }
    wchar_t lenText[32] = L"";
    len = sizeof(lenText);
    uint64_t total = 0;
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenText, &len, WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(lenText, nullptr, 10) + (status == 206 ? from : 0);
    if (total > maxBytes) {
        error = L"the file is larger than expected";
        return false;
    }
    std::vector<char> buf(512 * 1024);
    uint64_t got = 0;
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(req.h, buf.data(), static_cast<DWORD>(buf.size()), &read)) {
            error = systemError(GetLastError());
            return false;
        }
        if (read == 0)
            break;
        got += read;
        if (got + from > maxBytes) {
            error = L"the file is larger than expected";
            return false;
        }
        if (!sink(buf.data(), read, total))
            return false;
    }
    return true;
}

bool getText(const std::wstring &url, uint64_t maxBytes, std::string &out, std::wstring &error) {
    out.clear();
    return get(
        url, maxBytes, 0,
        [&](const char *d, size_t n, uint64_t) {
            out.append(d, n);
            return true;
        },
        error);
}

std::string hashBytes(const void *data, size_t n, Hash kind) {
    Hasher h(kind);
    if (!h.ok())
        return {};
    h.add(data, n);
    return h.hex();
}

namespace {

// Files are opened without inheritance (no child process can hold them) and allow delete and
// rename while open, so a download can always be checked and moved into place.
HANDLE openFile(const std::wstring &path, bool write, bool append) {
    HANDLE h = CreateFileW(path.c_str(), write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           write ? (append ? OPEN_ALWAYS : CREATE_ALWAYS) : OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | (write ? 0 : FILE_FLAG_SEQUENTIAL_SCAN), nullptr);
    if (h != INVALID_HANDLE_VALUE && append) {
        LARGE_INTEGER zero{};
        SetFilePointerEx(h, zero, nullptr, FILE_END);
    }
    return h;
}

} // namespace

std::string hashFile(const std::wstring &path, Hash kind, const std::atomic<bool> *cancel) {
    if (cancel && *cancel)
        return {};
    Hasher h(kind);
    HANDLE f = openFile(path, false, false);
    if (f == INVALID_HANDLE_VALUE || !h.ok()) {
        if (f != INVALID_HANDLE_VALUE)
            CloseHandle(f);
        return {};
    }
    std::vector<char> buf(1 << 20);
    DWORD n = 0;
    bool readOk = true;
    for (;;) {
        if (cancel && *cancel) {
            readOk = false;
            break;
        }
        if (!ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr)) {
            readOk = false;
            break;
        }
        if (!n)
            break;
        h.add(buf.data(), n);
    }
    CloseHandle(f);
    if (!readOk)
        return {};
    return h.hex();
}

bool download(const std::wstring &url, const std::wstring &path, uint64_t size, const std::string &hexHash, Hash kind,
              const std::function<void(uint64_t, uint64_t)> &progress, const std::atomic<bool> *cancel, std::wstring &error) {
    if (exists(path) && hashFile(path, kind) == hexHash)
        return true; // already here
    std::wstring part = path + L".part";
    uint64_t have = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(part.c_str(), GetFileExInfoStandard, &fa))
        have = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    if (have >= size) {
        if (!DeleteFileW(part.c_str()) && exists(part)) {
            error = L"an earlier download is still in use";
            return false;
        }
        have = 0;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        HANDLE f = openFile(part, true, have > 0);
        if (f == INVALID_HANDLE_VALUE) {
            error = L"the download folder is not writable";
            return false;
        }
        uint64_t got = have;
        bool ok = get(
            url, size, have,
            [&](const char *d, size_t n, uint64_t total) {
                if ((total && total != size) || got + n > size) {
                    error = L"the file on the server is not the expected one";
                    return false;
                }
                if (cancel && *cancel) {
                    error = L"cancelled";
                    return false;
                }
                DWORD written = 0;
                if (!WriteFile(f, d, static_cast<DWORD>(n), &written, nullptr) || written != n) {
                    error = L"the disk is full";
                    return false;
                }
                got += n;
                if (progress)
                    progress(got, size);
                return true;
            },
            error);
        CloseHandle(f);
        if (!ok && have > 0 && error == L"resume not supported") {
            DeleteFileW(part.c_str());
            have = 0;
            continue;
        }
        if (!ok)
            return false; // keep the .part file so a later try can resume
        if (got != size) {
            error = L"the download is incomplete";
            return false;
        }
        break;
    }
    if (hashFile(part, kind) != hexHash) {
        DeleteFileW(part.c_str());
        error = L"the downloaded file failed its safety check";
        return false;
    }
    if (!MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = L"the downloaded file could not be saved";
        return false;
    }
    return true;
}

} // namespace phone::net
