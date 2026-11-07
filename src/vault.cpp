#include "vault.hpp"
#include "net.hpp"
#include "presets.hpp"
#include "instances.hpp"
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <map>
#include <iomanip>
#include <stdexcept>
namespace phone::vault {
namespace fs = std::filesystem;
Progress progress;
namespace {
struct Prepared {
    std::wstring path;
    std::vector<HANDLE> handles;
    void clear() {
        for (auto h : handles)
            CloseHandle(h);
        handles.clear();
        path.clear();
    }
    ~Prepared() { clear(); }
};
std::mutex preparedMutex;
Prepared pending;
struct File {
    std::wstring name;
    uint64_t bytes = 0;
    std::string hash;
};
struct Manifest {
    std::wstring created, avd, image, imageRelative;
    std::wstring phoneName = L"Velox";
    std::vector<File> files, bases;
    uint64_t bytes = 0;
    bool root = false;
};
struct Job {
    bool acquired = false;
    Job() {
        acquired = !progress.running.exchange(true);
        if (acquired) {
            progress.cancel = false;
            progress.set(0, i18n::text(L"Preparing the vault…"));
        }
    }
    ~Job() {
        if (acquired)
            progress.running = false;
    }
};
void demand(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
void holdPrepared(const std::wstring &file) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    demand(h != INVALID_HANDLE_VALUE, "Recovery files changed or are in use. Prepare the copy again.");
    pending.handles.push_back(h);
}
void checkCancel() {
    if (progress.cancel)
        throw std::runtime_error("Cancelled. Your current phone has not been changed.");
}
std::string checksum(const std::wstring &path) {
    checkCancel();
    auto result = net::hashFile(path, net::Hash::Sha256, &progress.cancel);
    checkCancel();
    return result;
}
std::wstring config() {
    return dataFolder() + L"\\vault.ini";
}
std::wstring stamp() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t s[64];
    swprintf(s, 64, L"%04d-%02d-%02d %02d.%02d.%02d-%03d-%lu", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
             static_cast<unsigned long>(GetCurrentProcessId()));
    return s;
}
std::wstring absolute(const std::wstring &s) {
    return fs::absolute(fs::path(s)).lexically_normal().wstring();
}
std::wstring slash(std::wstring s) {
    std::replace(s.begin(), s.end(), L'\\', L'/');
    return s;
}
std::wstring lower(std::wstring s) {
    for (auto &c : s)
        c = static_cast<wchar_t>(towlower(c));
    return s;
}
bool inside(const std::wstring &child, const std::wstring &root) {
    auto c = lower(slash(absolute(child))), r = lower(slash(absolute(root)));
    if (!r.empty() && r.back() != L'/')
        r += L'/';
    return c.rfind(r, 0) == 0;
}
void noLinks(const std::wstring &path) {
    fs::path built;
    for (const auto &part : fs::path(absolute(path))) {
        built /= part;
        DWORD a = GetFileAttributesW(built.c_str());
        if (a != INVALID_FILE_ATTRIBUTES)
            demand(!(a & FILE_ATTRIBUTE_REPARSE_POINT), "Links and cloud placeholders are not supported here. Choose a local folder.");
    }
}
bool relativeName(const std::wstring &name) {
    if (name.empty() || name.size() > 220 || name.front() == L'/' || name.back() == L'/')
        return false;
    for (auto c : name)
        if (c < 32 || c == L'\\' || c == L':' || c == L'"' || c == L'?' || c == L'*' || c == L'|' || c == L'<' || c == L'>')
            return false;
    std::wistringstream in(name);
    for (std::wstring part; std::getline(in, part, L'/');) {
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ')
            return false;
        std::wstring b = lower(part.substr(0, part.find('.')));
        if (b == L"con" || b == L"nul" || b == L"aux" || b == L"prn" || (b.rfind(L"com", 0) == 0 && b.size() == 4 && iswdigit(b[3])) ||
            (b.rfind(L"lpt", 0) == 0 && b.size() == 4 && iswdigit(b[3])))
            return false;
    }
    return true;
}
bool allowed(const std::wstring &name) {
    if (!relativeName(name))
        return false;
    if (name == L"desktop/settings.ini" || name == L"root/ramdisk-root.img")
        return true;
    if (name.rfind(L"desktop/controls/", 0) == 0 || name.rfind(L"desktop/presets/", 0) == 0)
        return name.find('/', name.rfind(L"desktop/controls/", 0) == 0 ? 17 : 16) == std::wstring::npos &&
               fs::path(name).extension() == L".ini";
    if (name.rfind(L"avd/data/", 0) == 0 || name.rfind(L"avd/modem_simulator/", 0) == 0)
        return true;
    static const std::set<std::wstring> top = {L"config.ini",        L"userdata-qemu.img",       L"userdata-qemu.img.qcow2",
                                               L"encryptionkey.img", L"encryptionkey.img.qcow2", L"sdcard.img",
                                               L"sdcard.img.qcow2",  L"system.img.qcow2",        L"vendor.img.qcow2"};
    return name.rfind(L"avd/", 0) == 0 && top.count(name.substr(4));
}
std::wstring get(const std::wstring &file, const wchar_t *section, const wchar_t *key, const wchar_t *fallback = L"") {
    wchar_t out[4096]{};
    GetPrivateProfileStringW(section, key, fallback, out, 4096, file.c_str());
    return out;
}
void unicodeIni(const std::wstring &file) {
    noLinks(file);
    std::ifstream in(fs::path(file), std::ios::binary);
    std::string bytes;
    if (in) {
        demand(fs::file_size(file) <= (1u << 20), "Recovery settings exceed the supported size.");
        bytes.assign(std::istreambuf_iterator<char>(in), {});
        if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff && static_cast<unsigned char>(bytes[1]) == 0xfe)
            return;
    }
    in.close();
    std::wstring text;
    if (!bytes.empty()) {
        bool utf8 = bytes.rfind("\xef\xbb\xbf", 0) == 0;
        auto raw = utf8 ? bytes.substr(3) : bytes;
        int count = MultiByteToWideChar(utf8 ? CP_UTF8 : CP_ACP, 0, raw.data(), int(raw.size()), nullptr, 0);
        demand(count > 0 || raw.empty(), "Couldn't read recovery settings.");
        text.resize(count);
        if (count)
            MultiByteToWideChar(utf8 ? CP_UTF8 : CP_ACP, 0, raw.data(), int(raw.size()), text.data(), count);
    }
    auto temp = file + L".unicode-new";
    noLinks(temp);
    std::ofstream out(fs::path(temp), std::ios::binary | std::ios::trunc);
    const unsigned char bom[] = {0xff, 0xfe};
    out.write(reinterpret_cast<const char *>(bom), 2);
    out.write(reinterpret_cast<const char *>(text.data()), text.size() * sizeof(wchar_t));
    out.close();
    demand(out.good(), "Couldn't prepare Unicode recovery settings.");
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, file.c_str());
    demand(MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
           "Couldn't save Unicode recovery settings.");
}
void put(const std::wstring &file, const wchar_t *section, const wchar_t *key, const std::wstring &value) {
    // The W profile API still writes ANSI unless the file already has a UTF-16 BOM.
    unicodeIni(file);
    demand(WritePrivateProfileStringW(section, key, value.c_str(), file.c_str()) != 0, "Couldn't save recovery settings.");
}
void flushIni(const std::wstring &file) {
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, file.c_str());
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(h);
        CloseHandle(h);
    }
}
void copy(const std::wstring &from, const std::wstring &to, uint64_t before, uint64_t total, float start, float span) {
    checkCancel();
    noLinks(from);
    noLinks(fs::path(to).parent_path().wstring());
    createDirs(fs::path(to).parent_path().wstring());
    struct C {
        uint64_t before, total;
        float start, span;
        std::wstring name;
    } c{before, total, start, span, fs::path(from).filename().wstring()};
    BOOL cancel = FALSE;
    BOOL ok = CopyFileExW(
        from.c_str(), to.c_str(),
        [](LARGE_INTEGER, LARGE_INTEGER transferred, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID context) -> DWORD {
            auto &c = *static_cast<C *>(context);
            progress.set(c.start + c.span * float(double(c.before + transferred.QuadPart) / std::max<uint64_t>(1, c.total)),
                         L"Copying " + c.name);
            return progress.cancel ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
        },
        &c, &cancel, COPY_FILE_FAIL_IF_EXISTS);
    demand(ok != FALSE, progress.cancel ? "Cancelled. Partial files remain outside your current phone."
                                        : "The copy failed. Check free space and folder access.");
}
void space(const std::wstring &directory, uint64_t needed) {
    createDirs(directory);
    ULARGE_INTEGER available{};
    demand(GetDiskFreeSpaceExW(directory.c_str(), &available, nullptr, nullptr) != FALSE, "Couldn't check free disk space.");
    demand(available.QuadPart >= needed + std::max<uint64_t>(64ull << 20, needed / 20), "Not enough free space for a safe complete copy.");
}
void writeManifest(const std::wstring &dir, const Manifest &m) {
    std::ofstream out(fs::path(dir + L"\\manifest.vlx"), std::ios::binary);
    out << "velox-backup 1\ncreated\t" << narrow(m.created) << "\nsource-avd\t" << narrow(slash(m.avd)) << "\nsource-image\t"
        << narrow(slash(m.image)) << "\nimage-relative\t" << narrow(slash(m.imageRelative)) << "\nphone-name\t" << narrow(m.phoneName)
        << "\nroot\t" << m.root << "\n";
    for (auto &f : m.bases)
        out << "base\t" << f.bytes << '\t' << f.hash << '\t' << narrow(f.name) << '\n';
    for (auto &f : m.files)
        out << "file\t" << f.bytes << '\t' << f.hash << '\t' << narrow(f.name) << '\n';
    out.flush();
    demand(out.good(), "Couldn't finish the backup manifest.");
    out.close();
    HANDLE file = CreateFileW((dir + L"\\manifest.vlx").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(file);
        CloseHandle(file);
    }
}
Manifest readManifest(const std::wstring &dir) {
    noLinks(dir);
    auto path = dir + L"\\manifest.vlx";
    noLinks(path);
    demand(fs::file_size(path) < (4u << 20), "The backup manifest is too large.");
    std::ifstream in{fs::path(path), std::ios::binary};
    std::string line;
    std::getline(in, line);
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    demand(line == "velox-backup 1", "This folder isn't a supported Velox backup.");
    Manifest m;
    std::set<std::wstring> names;
    std::set<std::string> fields;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        demand(line.size() < 8192, "Invalid backup record.");
        std::vector<std::string> f;
        std::istringstream row(line);
        for (std::string s; std::getline(row, s, '\t');)
            f.push_back(s);
        if (f.empty())
            continue;
        if (f[0] == "file" || f[0] == "base") {
            demand(f.size() == 4, "Invalid backup file record.");
            demand(!f[1].empty() && f[1].find_first_not_of("0123456789") == std::string::npos && f[1].size() < 20, "Invalid backup size.");
            uint64_t size = std::stoull(f[1]);
            demand(size <= (512ull << 30), "A backup file exceeds the size limit.");
            demand(f[2].size() == 64 && f[2].find_first_not_of("0123456789abcdef") == std::string::npos, "Invalid backup checksum.");
            auto name = widen(f[3]);
            if (f[0] == "file") {
                demand(allowed(name) && names.insert(lower(name)).second, "Unsafe or duplicate backup path.");
                demand(m.files.size() < 10000 && m.bytes <= (1ull << 40) - size, "Backup exceeds the supported size.");
                m.files.push_back({name, size, f[2]});
                m.bytes += size;
            } else {
                static const std::set<std::wstring> base = {L"system.img", L"vendor.img", L"ramdisk.img", L"kernel-ranchu", L"build.prop"};
                demand(base.count(name) && names.insert(L"base/" + lower(name)).second, "Invalid system-image entry.");
                m.bases.push_back({name, size, f[2]});
            }
        } else {
            demand(f.size() == 2 && fields.insert(f[0]).second, "Invalid backup metadata.");
            auto value = widen(f[1]);
            if (f[0] == "created")
                m.created = value;
            else if (f[0] == "phone-name") {
                demand(!value.empty() && value.size() <= 40 && std::none_of(value.begin(), value.end(), [](wchar_t c) { return c < 32; }),
                       "Invalid phone name in backup.");
                m.phoneName = value;
            } else if (f[0] == "source-avd")
                m.avd = value;
            else if (f[0] == "source-image")
                m.image = value;
            else if (f[0] == "image-relative")
                m.imageRelative = value;
            else if (f[0] == "root") {
                demand(f[1] == "0" || f[1] == "1", "Invalid Root metadata.");
                m.root = f[1] == "1";
            } else
                demand(false, "Unsupported backup metadata.");
        }
    }
    demand(!m.created.empty() && !m.avd.empty() && !m.image.empty() && m.bases.size() == 5 && relativeName(slash(m.imageRelative)) &&
               slash(m.imageRelative).rfind(L"system-images/", 0) == 0,
           "Required backup metadata is missing.");
    demand(names.count(L"avd/config.ini") && names.count(L"avd/userdata-qemu.img") && names.count(L"avd/userdata-qemu.img.qcow2") &&
               names.count(L"desktop/settings.ini"),
           "Required phone files are missing.");
    demand(!m.root || names.count(L"root/ramdisk-root.img"), "This Root backup is missing its boot image.");
    return m;
}
void verifyFiles(const std::wstring &dir, const Manifest &m, float begin, float span) {
    uint64_t done = 0;
    for (auto &f : m.files) {
        checkCancel();
        auto path = dir + L"\\" + f.name;
        noLinks(path);
        demand(fs::is_regular_file(path) && fs::file_size(path) == f.bytes, "A backup file is missing or has the wrong size.");
        progress.set(begin + span * float(double(done) / std::max<uint64_t>(1, m.bytes)),
                     L"Verifying " + fs::path(f.name).filename().wstring());
        demand(checksum(path) == f.hash, "A backup checksum doesn't match. Your current phone was not changed.");
        done += f.bytes;
    }
}
uint32_t be32(const unsigned char *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint64_t be64(const unsigned char *p) {
    return uint64_t(be32(p)) << 32 | be32(p + 4);
}
std::wstring backingTarget(const std::string &ref, const Manifest &m, const std::wstring &avd, const std::wstring &image) {
    demand(ref.size() < 4096 && ref.find('\0') == std::string::npos, "Invalid disk backing path.");
    auto r = slash(widen(ref));
    fs::path original = fs::path(r).is_absolute() ? fs::path(r) : fs::path(m.avd) / r;
    auto normal = slash(original.lexically_normal().wstring());
    auto prefix = [&](const std::wstring &root) {
        auto p = slash(fs::path(root).lexically_normal().wstring());
        if (p.back() != L'/')
            p += L'/';
        return lower(normal).rfind(lower(p), 0) == 0 ? p : std::wstring();
    };
    auto a = prefix(m.avd);
    if (!a.empty()) {
        auto tail = normal.substr(a.size());
        demand(relativeName(tail) &&
                   std::any_of(m.files.begin(), m.files.end(), [&](auto &f) { return lower(f.name) == lower(L"avd/" + tail); }),
               "Disk points outside the backed-up phone.");
        return avd + L"\\" + tail;
    }
    auto i = prefix(m.image);
    if (!i.empty()) {
        auto tail = normal.substr(i.size());
        demand(std::any_of(m.bases.begin(), m.bases.end(), [&](auto &f) { return f.name == tail; }),
               "Disk points outside the expected Android image.");
        return image + L"\\" + tail;
    }
    demand(false, "External disk backing files are not allowed.");
    return {};
}
uint64_t rebase(const std::wstring &path, const Manifest &m, const std::wstring &avd, const std::wstring &image) {
    std::fstream file(fs::path(path), std::ios::binary | std::ios::in | std::ios::out);
    unsigned char h[104]{};
    file.read(reinterpret_cast<char *>(h), 104);
    demand(file.gcount() == 104, "A disk header is truncated.");
    demand(be32(h) == 0x514649fb && (be32(h + 4) == 2 || be32(h + 4) == 3), "Unsupported phone disk format.");
    uint32_t bits = be32(h + 20);
    demand(bits >= 9 && bits <= 21, "Invalid disk cluster size.");
    uint64_t cluster = 1ull << bits, offset = be64(h + 8);
    uint32_t size = be32(h + 16), header = be32(h + 4) == 3 ? be32(h + 100) : 72;
    demand(header >= 72 && header <= cluster, "Invalid disk header size.");
    if (be32(h + 4) == 3)
        demand((be64(h + 72) & ~1ull) == 0, "Unsupported or damaged disk features.");
    demand(be32(h + 32) == 0, "Unsupported QCOW encryption mode.");
    uint64_t virtualSize = be64(h + 24);
    demand(virtualSize > 0 && virtualSize <= (512ull << 30), "Invalid virtual disk size.");
    if (!size) {
        demand(!offset, "Invalid empty backing reference.");
        return virtualSize;
    }
    demand(offset >= header && offset < cluster && size < 4096 && size <= cluster - offset && cluster <= fs::file_size(path),
           "Invalid disk backing reference.");
    std::string format;
    for (uint64_t e = header; e + 8 <= offset;) {
        unsigned char ext[8]{};
        file.seekg(e);
        file.read(reinterpret_cast<char *>(ext), 8);
        demand(file.good(), "Truncated disk extensions.");
        uint32_t kind = be32(ext), n = be32(ext + 4);
        if (!kind)
            break;
        demand(n <= offset - e - 8, "Invalid disk extension length.");
        if (kind == 0xe2792aca) {
            std::string value(n, '\0');
            file.read(value.data(), n);
            demand(value == "raw" || value == "qcow2", "External disk formats are not allowed.");
            format = value;
        }
        demand(kind != 0x44415441, "External data-file disks are not allowed.");
        e += 8 + ((uint64_t(n) + 7) & ~7ull);
    }
    demand(!format.empty(), "Disk backing format must be explicit.");
    std::string old(size, '\0');
    file.seekg(offset);
    file.read(old.data(), size);
    auto target = backingTarget(old, m, avd, image);
    // Velox uses a single overlay over its matching raw image. Reject cycles and
    // alternate overlay chains supplied by an edited manifest before QEMU sees them.
    demand(fs::path(path).extension() == L".qcow2" && fs::path(target).filename() == fs::path(path).stem(),
           "Disk backing image doesn't match its overlay.");
    noLinks(target);
    demand(fs::is_regular_file(target), "A restored backing file is missing.");
    {
        unsigned char magic[4]{};
        std::ifstream base(fs::path(target), std::ios::binary);
        base.read(reinterpret_cast<char *>(magic), 4);
        demand(base.gcount() == 4 && ((be32(magic) == 0x514649fb) == (format == "qcow2")), "Disk backing format doesn't match its image.");
    }
    std::error_code error;
    auto relative = fs::relative(target, fs::path(path).parent_path(), error);
    std::string next = narrow(slash(error || relative.empty() ? absolute(target) : relative.wstring()));
    demand(next.size() < 4096 && next.size() <= cluster - offset, "The recovered disk path is too long.");
    if (next.size() > old.size()) {
        std::string padding(next.size() - old.size(), '\0');
        file.seekg(offset + size);
        file.read(padding.data(), padding.size());
        demand(file.good() && padding.find_first_not_of('\0') == std::string::npos,
               "Disk metadata has no safe space for the recovered path.");
    }
    std::string bytes(std::max(next.size(), old.size()), '\0');
    bytes.replace(0, next.size(), next);
    file.seekp(offset);
    file.write(bytes.data(), bytes.size());
    uint32_t n = static_cast<uint32_t>(next.size());
    unsigned char encoded[] = {static_cast<unsigned char>(n >> 24), static_cast<unsigned char>(n >> 16), static_cast<unsigned char>(n >> 8),
                               static_cast<unsigned char>(n)};
    file.seekp(16);
    file.write(reinterpret_cast<char *>(encoded), 4);
    file.flush();
    demand(file.good(), "Couldn't retarget the recovered disk.");
    return virtualSize;
}
std::wstring transaction() {
    return dataFolder() + L"\\recovery-transaction";
}
void move(const std::wstring &a, const std::wstring &b) {
    noLinks(a);
    noLinks(fs::path(b).parent_path().wstring());
    demand(MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
           "Couldn't switch recovery files. Close other apps accessing Velox data.");
}
void atomicSettings(const std::wstring &from) {
    noLinks(from);
    noLinks(iniPath());
    std::wstring temp = iniPath() + L".recovery-new";
    noLinks(temp);
    demand(CopyFileW(from.c_str(), temp.c_str(), FALSE) != FALSE, "Couldn't prepare launcher settings.");
    flushIni(temp);
    demand(MoveFileExW(temp.c_str(), iniPath().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
           "Couldn't switch launcher settings.");
    flushIni(iniPath());
}
void copyTree(const std::wstring &from, const std::wstring &to) {
    if (!exists(from))
        return;
    noLinks(from);
    createDirs(to);
    for (auto &e : fs::recursive_directory_iterator(from)) {
        noLinks(e.path().wstring());
        auto relative = fs::relative(e.path(), from);
        auto dest = fs::path(to) / relative;
        if (e.is_directory())
            createDirs(dest.wstring());
        else {
            demand(e.is_regular_file(), "Only regular profile files can be restored.");
            demand(CopyFileW(e.path().c_str(), dest.c_str(), TRUE) != FALSE, "Couldn't stage control profiles.");
        }
    }
}
void rollback(const Report &report) {
    auto t = transaction(), journal = t + L"\\journal.ini";
    noLinks(t);
    for (const wchar_t *name : {L"controls", L"presets"}) {
        auto old = t + L"\\old-" + name, now = dataFolder() + L"\\" + name;
        if (exists(old)) {
            if (exists(now))
                move(now, t + L"\\unused-" + name + L"-" + stamp());
            move(old, now);
        } else if (get(journal, L"original", name, L"0") == L"0" && exists(now))
            move(now, t + L"\\unused-" + name + L"-" + stamp());
    }
    atomicSettings(t + L"\\old-settings.ini");
    put(journal, L"restore", L"state", L"rolled-back");
    flushIni(journal);
    presets::invalidate();
    report(i18n::text(L"Previous phone and profiles restored. Both phone-data folders remain intact."), false);
}
} // namespace
std::wstring folder() {
    auto f = get(config(), L"vault", L"folder");
    if (!f.empty())
        return f;
    wchar_t docs[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs);
    return std::wstring(docs) + L"\\Velox Vault";
}
bool setFolder(const std::wstring &f) {
    try {
        noLinks(f);
        createDirs(f);
        put(config(), L"vault", L"folder", absolute(f));
        return true;
    } catch (...) {
        return false;
    }
}
Entry inspect(const std::wstring &path) {
    Entry e;
    e.path = path;
    e.name = fs::path(path).filename().wstring();
    try {
        auto m = readManifest(path);
        e.created = m.created;
        auto date = m.created.substr(0, 19);
        std::replace(date.begin(), date.end(), L'.', L':');
        e.name = m.phoneName + L" · " + date;
        e.bytes = m.bytes;
        e.files = m.files.size();
        e.valid = true;
    } catch (...) {
        e.created = i18n::text(L"Needs attention");
    }
    return e;
}
std::vector<Entry> list() {
    std::vector<Entry> out;
    try {
        auto root = folder();
        if (!isDir(root))
            return out;
        noLinks(root);
        for (auto &e : fs::directory_iterator(root)) {
            if (e.is_directory() && e.path().extension() == L".vbackup") {
                out.push_back(inspect(e.path().wstring()));
                if (out.size() >= 200)
                    break;
            }
        }
        std::sort(out.begin(), out.end(), [](auto &a, auto &b) { return a.valid != b.valid ? a.valid : a.created > b.created; });
    } catch (...) {
    }
    return out;
}
bool create(const Report &report) {
    Job job;
    if (!job.acquired)
        return false;
    try {
        demand(state == State::Stopped && !emulatorRunning(), "Stop the phone before creating a consistent backup.");
        demand(installed(), "Set up your phone before creating a backup.");
        auto p = paths();
        noLinks(p.avdDir);
        auto root = folder();
        noLinks(root);
        createDirs(root);
        demand(!inside(root, p.avdDir) && absolute(root) != absolute(p.avdDir), "The vault must be outside the phone-data folder.");
        Manifest m;
        m.created = stamp();
        m.phoneName = instances::name().substr(0, 40);
        for (auto &c : m.phoneName)
            if (c < 32)
                c = L' ';
        if (m.phoneName.empty())
            m.phoneName = L"Velox";
        m.avd = absolute(p.avdDir);
        m.imageRelative = slash(get(p.avdDir + L"\\config.ini", L"", L"image.sysdir.1"));
        // AVD config is flat; read it directly instead of interpreting it as an INI section.
        {
            std::ifstream in(fs::path(p.avdDir + L"\\config.ini"));
            for (std::string line; std::getline(in, line);)
                if (line.rfind("image.sysdir.1=", 0) == 0) {
                    m.imageRelative = slash(widen(line.substr(15)));
                    while (!m.imageRelative.empty() &&
                           (m.imageRelative.back() == L'\r' || m.imageRelative.back() == L'/' || m.imageRelative.back() == L' '))
                        m.imageRelative.pop_back();
                    break;
                }
        }
        demand(relativeName(m.imageRelative) && m.imageRelative.rfind(L"system-images/", 0) == 0,
               "The phone must use a system image inside the selected SDK.");
        m.image = absolute(p.sdk + L"\\" + m.imageRelative);
        noLinks(m.image);
        std::vector<std::pair<std::wstring, std::wstring>> source;
        for (auto &e : fs::recursive_directory_iterator(p.avdDir)) {
            checkCancel();
            noLinks(e.path().wstring());
            auto relative = slash(fs::relative(e.path(), p.avdDir).wstring());
            if (e.is_directory())
                continue;
            auto name = L"avd/" + relative;
            if (allowed(name)) {
                noLinks(e.path().wstring());
                demand(e.is_regular_file(), "Phone backup contains an unsupported file.");
                source.push_back({name, e.path().wstring()});
            }
        }
        source.push_back({L"desktop/settings.ini", iniPath()});
        for (auto name : {L"controls", L"presets"}) {
            auto d = dataFolder() + L"\\" + name;
            noLinks(d);
            if (isDir(d))
                for (auto &e : fs::directory_iterator(d)) {
                    auto n = L"desktop/" + std::wstring(name) + L"/" + e.path().filename().wstring();
                    if (allowed(n) && e.is_regular_file())
                        source.push_back({n, e.path().wstring()});
                }
        }
        m.root = get(iniPath(), L"launcher", L"root", L"0") == L"1";
        if (m.root) {
            demand(exists(p.rootImage), "The Root boot image is missing. Back up after fixing Root or use stock mode.");
            source.push_back({L"root/ramdisk-root.img", p.rootImage});
        }
        struct Locks {
            std::vector<HANDLE> handles;
            ~Locks() {
                for (auto h : handles)
                    CloseHandle(h);
            }
        } locks;
        uint64_t total = 0;
        for (auto &f : source) {
            noLinks(f.second);
            HANDLE h =
                CreateFileW(f.second.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            demand(h != INVALID_HANDLE_VALUE, "Phone files are in use. Close the phone and other emulator tools first.");
            locks.handles.push_back(h);
            uint64_t n = fs::file_size(f.second);
            demand(n <= (512ull << 30) && total <= (1ull << 40) - n, "The backup is too large.");
            total += n;
        }
        space(root, total);
        std::wstring stage = root + L"\\.partial-" + m.created, finished = root + L"\\Velox " + m.created + L".vbackup";
        demand(!exists(stage) && !exists(finished), "A backup with this timestamp already exists.");
        createDirs(stage);
        uint64_t done = 0;
        for (auto &pair : source) {
            auto size = fs::file_size(pair.second);
            copy(pair.second, stage + L"\\" + pair.first, done, total, 0, .6f);
            checkCancel();
            auto hash = checksum(stage + L"\\" + pair.first);
            demand(hash.size() == 64, "Couldn't checksum a copied file.");
            m.files.push_back({pair.first, size, hash});
            m.bytes += size;
            done += size;
        }
        for (auto name : {L"system.img", L"vendor.img", L"ramdisk.img", L"kernel-ranchu", L"build.prop"}) {
            checkCancel();
            auto path = m.image + L"\\" + name;
            noLinks(path);
            progress.set(.65f, i18n::text(L"Recording Android image identity…"));
            auto hash = checksum(path);
            demand(hash.size() == 64, "Couldn't fingerprint the Android system image.");
            m.bases.push_back({name, fs::file_size(path), hash});
        }
        writeManifest(stage, m);
        auto validated = readManifest(stage);
        verifyFiles(stage, validated, .7f, .29f);
        demand(state == State::Stopped && !emulatorRunning(), "The phone started during backup. The partial copy wasn't published.");
        move(stage, finished);
        progress.set(1, i18n::text(L"Backup complete · file integrity verified"));
        report(i18n::text(L"Private phone backup created and verified."), false);
        return true;
    } catch (const std::exception &e) {
        report(widen(e.what()), true);
        return false;
    }
}
bool verify(const std::wstring &backup, const Report &report) {
    Job job;
    if (!job.acquired)
        return false;
    try {
        auto m = readManifest(backup);
        verifyFiles(backup, m, 0, .99f);
        progress.set(1, i18n::text(L"File integrity verified"));
        report(i18n::text(L"Every backed-up file matches its SHA-256 checksum."), false);
        return true;
    } catch (const std::exception &e) {
        report(widen(e.what()), true);
        return false;
    }
}
std::optional<std::wstring> prepare(const std::wstring &backup, const Report &report) {
    Job job;
    if (!job.acquired)
        return {};
    std::lock_guard<std::mutex> seal(preparedMutex);
    try {
        demand(state == State::Stopped && !emulatorRunning(), "Stop the phone before restoring a backup.");
        pending.clear();
        auto m = readManifest(backup);
        verifyFiles(backup, m, 0, .25f);
        auto image = paths().sdk + L"\\" + m.imageRelative;
        noLinks(image);
        for (auto &b : m.bases) {
            checkCancel();
            auto file = image + L"\\" + b.name;
            noLinks(file);
            holdPrepared(file);
            demand(fs::is_regular_file(file) && fs::file_size(file) == b.bytes && checksum(file) == b.hash,
                   "This backup requires its matching Android system image. Select the matching SDK first.");
        }
        auto root = folder() + L"\\Restored";
        noLinks(root);
        space(root, m.bytes);
        auto target = root + L"\\Phone " + stamp();
        demand(!exists(target), "Recovery folder already exists.");
        createDirs(target);
        uint64_t done = 0;
        for (auto &f : m.files) {
            auto to = f.name.rfind(L"avd/", 0) == 0 ? L"avd/Gaming_Phone.avd/" + f.name.substr(4) : f.name;
            copy(backup + L"\\" + f.name, target + L"\\" + to, done, m.bytes, .3f, .5f);
            demand(checksum(target + L"\\" + to) == f.hash, "A copied recovery file failed its checksum.");
            done += f.bytes;
        }
        auto avd = target + L"\\avd\\Gaming_Phone.avd";
        uint64_t partition = 32ull << 30;
        for (auto &f : m.files)
            if (f.name.rfind(L"avd/", 0) == 0 && (fs::path(f.name).extension() == L".qcow2" || fs::path(f.name).extension() == L".img")) {
                auto file = avd + L"\\" + f.name.substr(4);
                unsigned char magic[4]{};
                std::ifstream disk(fs::path(file), std::ios::binary);
                disk.read(reinterpret_cast<char *>(magic), 4);
                disk.close();
                if (be32(magic) != 0x514649fb) {
                    demand(fs::path(f.name).extension() != L".qcow2", "Invalid QCOW2 disk signature.");
                    continue;
                }
                auto size = rebase(file, m, avd, image);
                if (f.name == L"avd/userdata-qemu.img.qcow2")
                    partition = size;
            }
        std::ofstream cfg(fs::path(avd + L"\\config.ini"), std::ios::binary | std::ios::trunc);
        cfg << "AvdId=Gaming_Phone\navd.ini.displayname=Velox\navd.ini.encoding=UTF-8\nPlayStore.enabled=true\nabi.type=x86_64\nhw.cpu."
               "arch=x86_64\ntag.id=google_apis_playstore\ntag.display=Google Play\nimage.sysdir.1="
            << narrow(m.imageRelative)
            << "/\nhw.device.name=pixel_7\nhw.device.manufacturer=Google\nhw.keyboard=yes\nhw.mainKeys=no\nhw.accelerometer=yes\nhw."
               "gyroscope=yes\nhw.sensors.orientation=yes\nhw.sensors.proximity=yes\nhw.battery=yes\nhw.gps=yes\nhw.camera.back="
               "emulated\nhw.camera.front=emulated\nhw.audioInput=yes\nhw.audioOutput=yes\nhw.sdCard=yes\nsdcard.size=2048M\nvm.heapSize="
               "576\nhw.cpu.ncore=4\nhw.ramSize=4096\nhw.lcd.width=1080\nhw.lcd.height=2400\nhw.lcd.density=420\nhw.gpu.enabled=yes\nhw."
               "gpu.mode=host\ndisk.dataPartition.size="
            << ((partition + (1 << 20) - 1) >> 20) << "M\nfastboot.forceColdBoot=yes\nshowDeviceFrame=no\n";
        cfg.close();
        demand(cfg.good(), "Couldn't prepare recovered phone settings.");
        std::ofstream ini(fs::path(target + L"\\avd\\Gaming_Phone.ini"), std::ios::binary);
        ini << "avd.ini.encoding=UTF-8\npath=" << narrow(avd) << "\npath.rel=avd/Gaming_Phone.avd\ntarget=android-35\n";
        ini.close();
        demand(ini.good(), "Couldn't register the recovered phone.");
        // The selected emulator uses QEMU2. The system-image version marker is
        // essential too: drive-share.cpp recreates every overlay when it is absent.
        std::ofstream engine(fs::path(avd + L"\\qemu-version.txt"), std::ios::binary);
        engine << "2";
        engine.close();
        demand(engine.good(), "Couldn't preserve the recovered disk engine identity.");
        std::string incremental;
        std::ifstream properties(fs::path(image + L"\\build.prop"));
        demand(fs::file_size(image + L"\\build.prop") < (1u << 20), "Android build properties exceed the supported size.");
        for (std::string line; std::getline(properties, line);)
            if (line.rfind("ro.build.version.incremental=", 0) == 0) {
                incremental = line.substr(29);
                while (!incremental.empty() && (incremental.back() == '\r' || incremental.back() == ' '))
                    incremental.pop_back();
                break;
            }
        demand(!incremental.empty() && incremental.size() <= 10 && incremental.find_first_not_of("0123456789") == std::string::npos &&
                   std::stoull(incremental) <= 2147483647,
               "The Android image has no supported incremental build identity.");
        std::ofstream version(fs::path(avd + L"\\version_num.cache"), std::ios::binary);
        version << incremental << '\n';
        version.close();
        demand(version.good(), "Couldn't preserve the recovered system-image identity.");
        // A ready marker is created last; activation never follows an incomplete staging folder.
        auto ready = target + L"\\ready.ini";
        put(ready, L"restore", L"sdk", paths().sdk);
        put(ready, L"restore", L"root", m.root ? L"1" : L"0");
        put(ready, L"restore", L"backup", absolute(backup));
        flushIni(ready);
        for (auto &entry : fs::recursive_directory_iterator(target)) {
            noLinks(entry.path().wstring());
            if (entry.is_regular_file())
                holdPrepared(entry.path().wstring());
        }
        pending.path = absolute(target);
        progress.set(1, i18n::text(L"Recovery copy ready · original phone untouched"));
        return target;
    } catch (const std::exception &e) {
        pending.clear();
        report(widen(e.what()), true);
        return {};
    }
}
bool activate(const std::wstring &target, const Report &report) {
    std::lock_guard<std::mutex> seal(preparedMutex);
    try {
        demand(pending.path == absolute(target) && !pending.handles.empty(), "Prepare and verify this recovery copy before activating it.");
        demand(state == State::Stopped && !emulatorRunning(), "Stop the phone before activating recovery.");
        noLinks(target);
        demand(inside(target, folder() + L"\\Restored") && exists(target + L"\\ready.ini"), "The recovery copy isn't ready.");
        auto sdk = get(target + L"\\ready.ini", L"restore", L"sdk");
        demand(sdk == paths().sdk, "The SDK changed during recovery. Prepare the copy again.");
        auto t = transaction();
        if (exists(t)) {
            auto state = get(t + L"\\journal.ini", L"restore", L"state");
            demand(state == L"committed" || state == L"rolled-back", "An earlier recovery needs attention first.");
            auto history = dataFolder() + L"\\recovery-history";
            createDirs(history);
            move(t, history + L"\\" + stamp());
        }
        createDirs(t);
        put(t + L"\\journal.ini", L"restore", L"state", L"preparing");
        flushIni(t + L"\\journal.ini");
        demand(CopyFileW(iniPath().c_str(), (t + L"\\old-settings.ini").c_str(), TRUE) != FALSE,
               "Couldn't preserve the previous settings.");
        auto fresh = t + L"\\new-settings.ini";
        demand(CopyFileW(iniPath().c_str(), fresh.c_str(), TRUE) != FALSE, "Couldn't stage settings.");
        auto from = target + L"\\desktop\\settings.ini";
        for (auto k : {L"screen", L"hz", L"cpu", L"ram", L"gpu", L"root", L"snappy", L"boost", L"frame", L"bootscreen", L"bootstyle",
                       L"cursor", L"brand", L"gesturenavigation", L"reversebuttons", L"autorotate", L"touches", L"number"})
            put(fresh, L"launcher", k, get(from, L"launcher", k, get(iniPath(), L"launcher", k).c_str()));
        put(fresh, L"launcher", L"quickboot", L"0");
        put(fresh, L"launcher", L"sdk", sdk);
        put(fresh, L"launcher", L"home", target);
        put(fresh, L"launcher", L"rootimage",
            get(target + L"\\ready.ini", L"restore", L"root") == L"1" ? target + L"\\root\\ramdisk-root.img" : L"");
        WritePrivateProfileStringW(L"running", nullptr, nullptr, fresh.c_str());
        flushIni(fresh);
        auto journal = t + L"\\journal.ini";
        for (auto name : {L"controls", L"presets"}) {
            put(journal, L"original", name, isDir(dataFolder() + L"\\" + name) ? L"1" : L"0");
            copyTree(target + L"\\desktop\\" + name, t + L"\\new-" + name);
        }
        put(journal, L"restore", L"state", L"pending");
        flushIni(journal);
        try {
            for (auto name : {L"controls", L"presets"}) {
                auto active = dataFolder() + L"\\" + name;
                if (exists(active))
                    move(active, t + L"\\old-" + name);
                if (exists(t + L"\\new-" + name))
                    move(t + L"\\new-" + name, active);
            }
            atomicSettings(fresh);
            put(journal, L"restore", L"state", L"committed");
            flushIni(journal);
        } catch (...) {
            rollback(report);
            throw;
        }
        pending.clear();
        presets::invalidate();
        report(i18n::text(L"Recovered phone selected. The previous phone is intact; Undo restore returns to it."), false);
        return true;
    } catch (const std::exception &e) {
        pending.clear();
        auto t = transaction();
        if (get(t + L"\\journal.ini", L"restore", L"state") == L"preparing") {
            put(t + L"\\journal.ini", L"restore", L"state", L"rolled-back");
            flushIni(t + L"\\journal.ini");
        }
        report(widen(e.what()), true);
        return false;
    }
}
bool canUndo() {
    auto s = get(transaction() + L"\\journal.ini", L"restore", L"state");
    return s == L"committed" || s == L"undo-pending" || s == L"pending";
}
bool blocked() {
    auto s = get(transaction() + L"\\journal.ini", L"restore", L"state");
    return !s.empty() && s != L"committed" && s != L"rolled-back" && s != L"preparing";
}
bool undo(const Report &report) {
    try {
        demand(state == State::Stopped && !emulatorRunning(), "Stop the phone before undoing recovery.");
        demand(canUndo(), "No completed restore is available to undo.");
        put(transaction() + L"\\journal.ini", L"restore", L"state", L"undo-pending");
        flushIni(transaction() + L"\\journal.ini");
        rollback(report);
        return true;
    } catch (const std::exception &e) {
        report(widen(e.what()), true);
        return false;
    }
}
bool recoverInterrupted(const Report &report) {
    try {
        auto t = transaction();
        if (!exists(t))
            return true;
        auto s = get(t + L"\\journal.ini", L"restore", L"state");
        if (s.empty() || s == L"preparing") {
            put(t + L"\\journal.ini", L"restore", L"state", L"rolled-back");
            flushIni(t + L"\\journal.ini");
        } else if (s == L"pending" || s == L"undo-pending") {
            demand(!emulatorRunning(), "Close the phone to complete interrupted recovery.");
            rollback(report);
        } else
            demand(s == L"committed" || s == L"rolled-back",
                   "The recovery journal needs attention. Keep your phone folders and backups intact.");
        return true;
    } catch (const std::exception &e) {
        report(widen(e.what()), true);
        return false;
    }
}
std::wstring diagnostics() {
    std::wstring result = L"Velox " GP_VERSION_WSTRING L"\r\n" + hostSummary() + L"\r\nPhone state: " +
                          (state == State::Running   ? i18n::text(L"Running")
                           : state == State::Stopped ? L"Stopped"
                                                     : L"Changing state") +
                          L"\r\n\r\n";
    std::ifstream file(fs::path(paths().log), std::ios::binary);
    if (file) {
        file.seekg(0, std::ios::end);
        auto n = file.tellg();
        if (n > 0) {
            auto count = std::min<std::streamoff>(n, 128 << 10);
            file.seekg(n - count);
            std::string data(size_t(count), '\0');
            file.read(data.data(), count);
            data.resize(file.gcount());
            result += widen(data);
        }
    }
    for (auto &path :
         {envVar(L"USERPROFILE"), paths().sdk, paths().avdDir, exeFolder(), envVar(L"ANDROID_USER_HOME"), envVar(L"ANDROID_EMULATOR_HOME")})
        if (!path.empty())
            for (size_t at = 0; (at = result.find(path, at)) != std::wstring::npos; at += 6)
                result.replace(at, path.size(), L"[path]");
    return result;
}
} // namespace phone::vault
