#include <atomic>
#include <cstddef>
#include <filesystem>

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

bool regularFile(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileType(file) == FILE_TYPE_DISK && GetFileInformationByHandle(file, &info) && info.nNumberOfLinks == 1 &&
           !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
}

bool safeParent(const std::wstring &path) {
    std::error_code error;
    auto parent = std::filesystem::absolute(path, error).parent_path();
    if (error || parent.empty())
        return false;
    for (;;) {
        DWORD attributes = GetFileAttributesW(parent.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
        auto next = parent.parent_path();
        if (next == parent)
            return true;
        parent = next;
    }
}

// Deny other writers/renames until verification and publication are complete. OPEN_ALWAYS
// leaves existing bytes untouched until the handle's type and link count have been checked.
HANDLE openFile(const std::wstring &path, bool write) {
    if (write && !safeParent(path))
        return INVALID_HANDLE_VALUE;
    HANDLE file = CreateFileW(path.c_str(), write ? GENERIC_READ | GENERIC_WRITE | DELETE : GENERIC_READ, FILE_SHARE_READ,
                              nullptr, write ? OPEN_ALWAYS : OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file != INVALID_HANDLE_VALUE && !regularFile(file)) {
        CloseHandle(file);
        return INVALID_HANDLE_VALUE;
    }
    return file;
}

bool publish(HANDLE file, const std::wstring &path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error).wstring();
    if (error || absolute.size() > 32760 || !safeParent(path) || !regularFile(file) || !FlushFileBuffers(file))
        return false;
    // Rename the verified open file, not a pathname that could have been substituted.
    std::vector<BYTE> buffer(sizeof(FILE_RENAME_INFO) + absolute.size() * sizeof(wchar_t));
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(buffer.data());
    rename->ReplaceIfExists = TRUE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = DWORD(absolute.size() * sizeof(wchar_t));
    memcpy(rename->FileName, absolute.c_str(), rename->FileNameLength + sizeof(wchar_t));
    return SetFileInformationByHandle(file, FileRenameInfo, rename, DWORD(buffer.size())) != 0;
}

std::string hashHandle(HANDLE file, Hash kind, const std::atomic<bool> *cancel) {
    Hasher hash(kind);
    LARGE_INTEGER zero{};
    if (!hash.ok() || !SetFilePointerEx(file, zero, nullptr, FILE_BEGIN))
        return {};
    std::vector<char> buffer(1 << 20);
    DWORD count = 0;
    for (;;) {
        if ((cancel && *cancel) || !ReadFile(file, buffer.data(), DWORD(buffer.size()), &count, nullptr))
            return {};
        if (!count)
            return hash.hex();
        hash.add(buffer.data(), count);
    }
}

} // namespace

std::string hashFile(const std::wstring &path, Hash kind, const std::atomic<bool> *cancel) {
    if (cancel && *cancel)
        return {};
    HANDLE f = openFile(path, false);
    if (f == INVALID_HANDLE_VALUE)
        return {};
    auto hash = hashHandle(f, kind, cancel);
    CloseHandle(f);
    return hash;
}

bool writeFile(const std::wstring &path, const void *data, size_t size) {
    if (!safeParent(path) || size > MAXDWORD)
        return false;
    GUID id{};
    wchar_t name[40]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, name, 40))
        return false;
    auto temporary = path + L"." + name;
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    bool ok = regularFile(file) && WriteFile(file, data, DWORD(size), &written, nullptr) && written == size && publish(file, path);
    if (!ok) {
        FILE_DISPOSITION_INFO remove{TRUE};
        SetFileInformationByHandle(file, FileDispositionInfo, &remove, sizeof(remove));
    }
    CloseHandle(file);
    return ok;
}

bool moveFile(const std::wstring &source, const std::wstring &path) {
    HANDLE file = CreateFileW(source.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    bool ok = regularFile(file) && publish(file, path);
    CloseHandle(file);
    return ok;
}

bool download(const std::wstring &url, const std::wstring &path, uint64_t size, const std::string &hexHash, Hash kind,
              const std::function<void(uint64_t, uint64_t)> &progress, const std::atomic<bool> *cancel, std::wstring &error) {
    if (cancel && *cancel) {
        error = L"cancelled";
        return false;
    }
    if (exists(path) && hashFile(path, kind, cancel) == hexHash)
        return true; // already here
    std::wstring part = path + L".part";
    HANDLE f = openFile(part, true);
    if (f == INVALID_HANDLE_VALUE) {
        error = L"the download file is busy, linked or not writable";
        return false;
    }
    struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{f};
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(f, &length) || length.QuadPart < 0)
        return false;
    uint64_t have = uint64_t(length.QuadPart);
    if (have == size && hashHandle(f, kind, cancel) == hexHash) {
        if (publish(f, path))
            return true;
        error = L"the downloaded file could not be saved";
        return false;
    }
    if (cancel && *cancel) {
        error = L"cancelled";
        return false;
    }
    if (have >= size)
        have = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        LARGE_INTEGER offset{};
        offset.QuadPart = have;
        if (!SetFilePointerEx(f, offset, nullptr, FILE_BEGIN) || (!have && !SetEndOfFile(f))) {
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
        if (!ok && have > 0 && error == L"resume not supported") {
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
    if (hashHandle(f, kind, cancel) != hexHash) {
        if (!(cancel && *cancel)) {
            FILE_DISPOSITION_INFO remove{TRUE};
            SetFileInformationByHandle(f, FileDispositionInfo, &remove, sizeof(remove));
        }
        error = cancel && *cancel ? L"cancelled" : L"the downloaded file failed its safety check";
        return false;
    }
    if (!publish(f, path)) {
        error = L"the downloaded file could not be saved";
        return false;
    }
    return true;
}

} // namespace phone::net
