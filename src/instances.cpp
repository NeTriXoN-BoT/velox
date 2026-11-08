#include "instances.hpp"
#include "setup.hpp"
#include "vault.hpp"
#include <winsock2.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <iomanip>
#include <stdexcept>
namespace phone::instances {
namespace fs = std::filesystem;
namespace {
std::wstring selected;
std::atomic<int> selectedPort{5580};
Command intent = Command::Open;
std::mutex runtimeLock;
struct Runtime {
    DWORD owner = 0, wrapper = 0;
    uint64_t ownerBorn = 0, wrapperBorn = 0;
    int cores = 0, memory = 0, port = 5580;
    std::wstring sdk;
} runtime;
void need(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
bool valid(const std::wstring &s) {
    return s.size() == 32 && s.find_first_not_of(L"0123456789abcdef") == std::wstring::npos;
}
std::wstring root() {
    return baseDataFolder() + L"\\instances";
}
std::wstring folder(const std::wstring &key) {
    return key.empty() ? baseDataFolder() : root() + L"\\" + key + L"\\data";
}
std::wstring meta(const std::wstring &key) {
    return root() + L"\\" + key + L"\\instance.ini";
}
std::wstring get(const std::wstring &file, const wchar_t *section, const wchar_t *key, const wchar_t *fallback = L"") {
    wchar_t out[4096]{};
    GetPrivateProfileStringW(section, key, fallback, out, 4096, file.c_str());
    return out;
}
uint64_t integer(const std::wstring &file, const wchar_t *section, const wchar_t *key) {
    auto value = get(file, section, key);
    return _wcstoui64(value.c_str(), nullptr, 10);
}
void plain(const std::wstring &path) {
    fs::path part;
    for (const auto &s : fs::absolute(path)) {
        part /= s;
        DWORD a = GetFileAttributesW(part.c_str());
        need(a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_REPARSE_POINT),
             "Choose a local Velox folder without links or cloud placeholders.");
    }
}
void write(const std::wstring &file, const std::wstring &text) {
    plain(file);
    createDirs(fs::path(file).parent_path().wstring());
    auto temp = file + L".new-" + std::to_wstring(GetCurrentProcessId());
    plain(temp);
    std::ofstream out(fs::path(temp), std::ios::binary | std::ios::trunc);
    out.put(char(0xff));
    out.put(char(0xfe));
    out.write(reinterpret_cast<const char *>(text.data()), text.size() * sizeof(wchar_t));
    out.close();
    need(out.good(), "Couldn't save instance preferences.");
    need(MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
         "Couldn't publish instance preferences.");
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, file.c_str());
}
uint64_t born(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return 0;
    FILETIME c{}, e{}, k{}, u{};
    bool ok = GetProcessTimes(h, &c, &e, &k, &u);
    CloseHandle(h);
    return ok ? (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime : 0;
}
bool alive(DWORD pid, uint64_t birth, const std::wstring &expected) {
    if (!pid || !birth)
        return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!h)
        return false;
    wchar_t image[32768]{};
    DWORD n = 32768;
    FILETIME c{}, e{}, k{}, u{};
    bool ok = WaitForSingleObject(h, 0) == WAIT_TIMEOUT && GetProcessTimes(h, &c, &e, &k, &u) &&
              ((uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime) == birth && QueryFullProcessImageNameW(h, 0, image, &n) &&
              _wcsicmp(image, expected.c_str()) == 0;
    CloseHandle(h);
    return ok;
}
std::wstring classFor(const std::wstring &key) {
    return key.empty() ? L"VeloxLauncher" : L"VeloxLauncher-" + key;
}
bool owns(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;
    wchar_t image[32768]{};
    DWORD n = 32768;
    bool yes = QueryFullProcessImageNameW(process, 0, image, &n) && _wcsicmp(image, exePath().c_str()) == 0;
    CloseHandle(process);
    return yes;
}
Info read(const std::wstring &key) {
    Info i;
    i.id = key;
    i.data = folder(key);
    i.current = key == selected;
    i.name = get(key.empty() ? baseDataFolder() + L"\\identity.ini" : meta(key), L"instance", L"name",
                 key.empty() ? i18n::text(L"Main phone") : i18n::text(L"Velox phone"));
    i.name.resize(std::min<size_t>(40, i.name.size()));
    i.port = key.empty() ? 5580 : int(integer(meta(key), L"instance", L"port"));
    need(i.port >= 5554 && i.port <= 5682 && i.port % 2 == 0 && (key.empty() || i.port != 5580),
         "The instance has an invalid connection port.");
    auto settings = i.data + L"\\settings.ini", ledger = i.data + L"\\runtime.ini";
    i.sdk = get(settings, L"launcher", L"sdk");
    i.error = get(i.data + L"\\status.ini", L"phone", L"error");
    i.window = window(key);
    DWORD wrapper = DWORD(integer(ledger, L"runtime", L"wrapper")), owner = DWORD(integer(ledger, L"runtime", L"owner"));
    auto ownerSdk = get(ledger, L"runtime", L"sdk", i.sdk.c_str());
    bool phone = alive(wrapper, integer(ledger, L"runtime", L"wrapperborn"), ownerSdk + L"\\emulator\\emulator.exe");
    auto pid = DWORD(integer(settings, L"running", L"pid"));
    bool booted = alive(pid, integer(settings, L"running", L"born"), i.sdk + L"\\emulator\\qemu\\windows-x86_64\\qemu-system-x86_64.exe");
    phone = phone || booted;
    bool reservation = !wrapper && alive(owner, integer(ledger, L"runtime", L"ownerborn"), exePath());
    i.active = phone || reservation;
    i.starting = reservation || (phone && !booted);
    if (i.window) {
        auto status = reinterpret_cast<INT_PTR>(GetPropW(i.window, L"VeloxPhoneState"));
        if (status == static_cast<INT_PTR>(State::Starting) + 1)
            i.starting = true;
        else if (status == static_cast<INT_PTR>(State::Running) + 1 && phone)
            i.starting = false;
    }
    if (i.active) {
        i.cores = std::clamp<int>(integer(ledger, L"runtime", L"cores"), 0, MAX_CPU_CORES);
        i.memory = std::clamp<int>(integer(ledger, L"runtime", L"memory"), 0, MAX_MEMORY_GB);
        if (!i.cores) {
            i.cores = std::clamp<int>(integer(settings, L"running", L"cpu"), 0, MAX_CPU_CORES);
            if (!i.cores)
                i.cores = coresFor(Settings{});
        }
        if (!i.memory) {
            i.memory = std::clamp<int>(integer(settings, L"running", L"ram"), 0, MAX_MEMORY_GB);
            if (!i.memory)
                i.memory = memoryFor(Settings{});
        }
    }
    if (i.active)
        i.memory = std::max(3, i.memory);
    return i;
}
bool available(int p) {
    // Checking listeners avoids mistaking a just-stopped phone's TIME_WAIT sockets
    // for a live emulator, and never binds a port owned by another application.
    for (ULONG family : {ULONG(AF_INET), ULONG(AF_INET6)}) {
        ULONG bytes = 0;
        DWORD result = GetExtendedTcpTable(nullptr, &bytes, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
        if (result == ERROR_NOT_SUPPORTED && family == AF_INET6)
            continue;
        if (result != ERROR_INSUFFICIENT_BUFFER || !bytes || bytes > (16u << 20))
            return false;
        std::vector<unsigned char> buffer(bytes);
        result = GetExtendedTcpTable(buffer.data(), &bytes, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
        if (result != NO_ERROR)
            return false;
        if (family == AF_INET) {
            auto table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID *>(buffer.data());
            for (DWORD i = 0; i < table->dwNumEntries; i++) {
                int local = ntohs(static_cast<u_short>(table->table[i].dwLocalPort));
                if (local == p || local == p + 1)
                    return false;
            }
        } else {
            auto table = reinterpret_cast<MIB_TCP6TABLE_OWNER_PID *>(buffer.data());
            for (DWORD i = 0; i < table->dwNumEntries; i++) {
                int local = ntohs(static_cast<u_short>(table->table[i].dwLocalPort));
                if (local == p || local == p + 1)
                    return false;
            }
        }
    }
    return true;
}
struct Gate {
    HANDLE h = nullptr;
    bool locked = false;
    Gate() {
        h = CreateMutexW(nullptr, FALSE, L"Local\\VeloxInstanceAllocation");
        if (h) {
            DWORD r = WaitForSingleObject(h, 5000);
            locked = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
        }
    }
    ~Gate() {
        if (locked)
            ReleaseMutex(h);
        if (h)
            CloseHandle(h);
    }
};
void saveRuntime() {
    std::wostringstream text;
    text << L"[runtime]\r\nowner=" << runtime.owner << L"\r\nownerborn=" << runtime.ownerBorn << L"\r\nwrapper=" << runtime.wrapper
         << L"\r\nwrapperborn=" << runtime.wrapperBorn << L"\r\ncores=" << runtime.cores << L"\r\nmemory=" << runtime.memory << L"\r\nport="
         << runtime.port << L"\r\n";
    text << L"sdk=" << runtime.sdk << L"\r\n";
    write(folder(selected) + L"\\runtime.ini", text.str());
}
std::wstring quote(const std::wstring &s) {
    std::wstring out = L"\"";
    size_t n = 0;
    for (auto c : s) {
        if (c == L'\\') {
            ++n;
            continue;
        }
        out.append(c == L'"' ? 2 * n + 1 : n, L'\\');
        out += c;
        n = 0;
    }
    out.append(n * 2, L'\\');
    return out + L"\"";
}

std::wstring mutexFor(const std::wstring &key) {
    uint64_t hash = 1469598103934665603ull;
    for (wchar_t c : baseDataFolder()) {
        hash ^= towlower(c);
        hash *= 1099511628211ull;
    }
    std::wostringstream s;
    s << L"Local\\VeloxLauncher-" << std::hex << hash << L"-" << key;
    return s.str();
}

struct LauncherGuard {
    HANDLE handle = nullptr;
    bool owned = false;
    explicit LauncherGuard(const std::wstring &key) {
        handle = CreateMutexW(nullptr, TRUE, mutexFor(key).c_str());
        owned = handle && GetLastError() != ERROR_ALREADY_EXISTS;
    }
    ~LauncherGuard() {
        if (owned)
            ReleaseMutex(handle);
        if (handle)
            CloseHandle(handle);
    }
};

struct ComRelease {
    void operator()(IUnknown *value) const {
        if (value)
            value->Release();
    }
};

// Refuse a permanent-delete callback, even if Windows cannot recycle this folder.
class RecycleGuard final : public IFileOperationProgressSink {
    LONG references = 1;

  public:
    bool rejected = false, recycled = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(IFileOperationProgressSink))
            return E_NOINTERFACE;
        *out = static_cast<IFileOperationProgressSink *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG left = InterlockedDecrement(&references);
        if (!left)
            delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem *, LPCWSTR) override { return E_ABORT; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override { return E_ABORT; }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags, IShellItem *) override {
        rejected = !(flags & TSF_DELETE_RECYCLE_IF_POSSIBLE);
        return rejected ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem *, HRESULT result, IShellItem *created) override {
        recycled = SUCCEEDED(result) && created;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem *, LPCWSTR) override { return E_ABORT; }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem *, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
};

std::wstring resolved(const fs::path &path) {
    auto full = fs::absolute(path).lexically_normal().wstring();
    HANDLE handle = CreateFileW(full.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return full;
    DWORD length = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    std::vector<wchar_t> buffer(length ? length + 1 : 1);
    DWORD copied =
        length ? GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) : 0;
    CloseHandle(handle);
    if (copied && copied < buffer.size()) {
        full.assign(buffer.data(), copied);
        if (full.rfind(L"\\\\?\\UNC\\", 0) == 0)
            full = L"\\\\" + full.substr(8);
        else if (full.rfind(L"\\\\?\\", 0) == 0)
            full.erase(0, 4);
    }
    return full;
}
bool inside(const std::wstring &path, const fs::path &parent) {
    if (path.empty())
        return false;
    auto child = resolved(path), base = resolved(parent);
    return CompareStringOrdinal(child.c_str(), int(child.size()), base.c_str(), int(base.size()), TRUE) == CSTR_EQUAL ||
           (child.size() > base.size() && child[base.size()] == L'\\' &&
            CompareStringOrdinal(child.c_str(), int(base.size()), base.c_str(), int(base.size()), TRUE) == CSTR_EQUAL);
}
void recyclePath(const fs::path &target) {
    struct Apartment {
        HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ~Apartment() {
            if (SUCCEEDED(result))
                CoUninitialize();
        }
    } apartment;
    need(SUCCEEDED(apartment.result), "Windows file operations are unavailable. The phone was not removed.");
    IFileOperation *rawOperation = nullptr;
    need(SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&rawOperation))),
         "Windows file operations are unavailable. The phone was not removed.");
    std::unique_ptr<IFileOperation, ComRelease> operation(rawOperation);
    IShellItem *rawItem = nullptr;
    need(SUCCEEDED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&rawItem))), "Couldn't open the phone folder.");
    std::unique_ptr<IShellItem, ComRelease> item(rawItem);
    std::unique_ptr<RecycleGuard, ComRelease> sink(new RecycleGuard);
    auto flags =
        FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOFX_RECYCLEONDELETE | FOFX_EARLYFAILURE | FOFX_ADDUNDORECORD;
    need(SUCCEEDED(operation->SetOperationFlags(flags)) && SUCCEEDED(operation->DeleteItem(item.get(), sink.get())),
         "Windows couldn't prepare the Recycle Bin operation. The phone was not removed.");
    HRESULT result = operation->PerformOperations();
    BOOL aborted = TRUE;
    operation->GetAnyOperationsAborted(&aborted);
    need(SUCCEEDED(result) && !aborted && !sink->rejected && sink->recycled && !exists(target.wstring()),
         "Windows couldn't recycle this phone. Close files in use and check the Recycle Bin settings; permanent deletion was not "
         "requested.");
}
} // namespace
const std::wstring &id() {
    return selected;
}
std::wstring dataPath(const std::wstring &primary) {
    return selected.empty() ? primary : primary + L"\\instances\\" + selected + L"\\data";
}
int port() {
    return selectedPort.load();
}
Command initialCommand() {
    return intent;
}
std::wstring name() {
    return read(selected).name;
}
std::wstring windowClass() {
    return classFor(selected);
}
std::wstring mutexName() {
    return mutexFor(selected);
}
HWND window(const std::wstring &key) {
    struct Query {
        std::wstring cls;
        HWND found = nullptr;
    } query{classFor(key)};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto &q = *reinterpret_cast<Query *>(l);
            wchar_t cls[128]{};
            GetClassNameW(h, cls, 128);
            if (q.cls == cls && owns(h)) {
                q.found = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&query));
    return query.found;
}
bool configure(std::wstring &error) {
    try {
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        need(argv != nullptr, "Couldn't read startup options.");
        std::vector<std::wstring> args;
        for (int i = 1; i < argc; i++)
            args.emplace_back(argv[i]);
        LocalFree(argv);
        for (size_t i = 0; i < args.size(); i++) {
            if (args[i] == L"--instance") {
                need(i + 1 < args.size() && valid(args[i + 1]), "Invalid Velox instance identifier.");
                selected = args[++i];
            } else if (args[i] == L"--start-phone")
                intent = Command::Start;
            else if (args[i] == L"--stop-phone")
                intent = Command::Stop;
        }
        if (!selected.empty()) {
            plain(meta(selected));
            need(exists(meta(selected)) && fs::file_size(meta(selected)) < 65536, "This Velox instance doesn't exist.");
            selectedPort = read(selected).port;
        }
        return true;
    } catch (const std::exception &e) {
        error = widen(e.what());
        return false;
    }
}
std::vector<Info> list() {
    std::vector<Info> out;
    try {
        out.push_back(read(L""));
        auto catalog = root();
        if (isDir(catalog)) {
            plain(catalog);
            for (auto &e : fs::directory_iterator(catalog)) {
                if (e.is_directory() && valid(e.path().filename().wstring())) {
                    try {
                        plain(e.path().wstring());
                        out.push_back(read(e.path().filename().wstring()));
                    } catch (...) {
                    }
                    if (out.size() >= 65)
                        break;
                }
            }
        }
        std::sort(out.begin() + 1, out.end(), [](auto &a, auto &b) { return a.name < b.name; });
    } catch (...) {
    }
    return out;
}

Maintenance::Maintenance(const std::wstring &key) {
    try {
        need(key.empty() || valid(key), "Invalid instance identifier.");
        Gate gate;
        need(gate.locked, "Another phone is starting. Try again in a moment.");
        handle = CreateMutexW(nullptr, FALSE, (mutexFor(key) + L"-maintenance").c_str());
        need(handle != nullptr, "Couldn't protect the phone during maintenance.");
        auto status = WaitForSingleObject(handle, 0);
        acquired = status == WAIT_OBJECT_0 || status == WAIT_ABANDONED;
        need(acquired, "This phone is already being maintained.");
        if (key != selected) {
            launcher = CreateMutexW(nullptr, TRUE, mutexFor(key).c_str());
            bool owned = launcher && GetLastError() != ERROR_ALREADY_EXISTS;
            if (!owned) {
                if (launcher)
                    CloseHandle(launcher);
                launcher = nullptr;
            }
            need(owned, "Close this phone's controls before maintenance.");
        }
        auto info = read(key);
        need(!info.active && !info.starting && (key == selected || !info.window),
             "Stop this phone and close its other controls before maintenance.");
        if (key == selected)
            need(state == State::Stopped && !emulatorRunning(), "Stop the phone before maintenance.");
        auto storeFor = [](const Info &phone) {
            if (phone.current)
                return paths().avdDir;
            auto home = get(phone.data + L"\\settings.ini", L"launcher", L"home");
            if (home.empty() && phone.id.empty())
                home = envVar(L"USERPROFILE") + L"\\.android";
            return home.empty() ? std::wstring() : home + L"\\avd\\Gaming_Phone.avd";
        };
        auto store = storeFor(info);
        need(!store.empty() && fs::path(store).is_absolute() && isDir(store) && exists(store + L"\\config.ini"),
             "Phone storage was not found. Choose a valid phone folder before maintenance.");
        plain(store);
        need(!inside(paths().sdk, store) && !inside(info.sdk, store), "Move the Android SDK outside this phone folder before maintenance.");
        for (const auto &other : list()) {
            need(!inside(other.sdk, store), "Move the Android SDK outside this phone folder before maintenance.");
            if (other.id == key)
                continue;
            auto otherStore = storeFor(other);
            need(otherStore.empty() || (!inside(store, otherStore) && !inside(otherStore, store)),
                 "Another phone shares this storage. Choose separate phone folders before maintenance.");
        }
    } catch (const std::exception &e) {
        error = widen(e.what());
        if (acquired) {
            ReleaseMutex(handle);
            acquired = false;
        }
    }
}
Maintenance::~Maintenance() {
    if (launcher) {
        ReleaseMutex(launcher);
        CloseHandle(launcher);
    }
    if (acquired)
        ReleaseMutex(handle);
    if (handle)
        CloseHandle(handle);
}
namespace {
std::wstring cleanName(std::wstring label) {
    while (!label.empty() && iswspace(label.front()))
        label.erase(label.begin());
    while (!label.empty() && iswspace(label.back()))
        label.pop_back();
    need(!label.empty() && label.size() <= 40 && label.find_first_of(L"\r\n\t[]=") == std::wstring::npos &&
             std::none_of(label.begin(), label.end(), [](wchar_t c) { return c < 32 || c == 127; }),
         "Use 1–40 characters without line breaks or INI symbols.");
    return label;
}
Paths phonePaths(const Info &info) {
    if (info.current)
        return paths();
    auto file = info.data + L"\\settings.ini";
    auto home = get(file, L"launcher", L"home");
    if (home.empty() && info.id.empty())
        home = envVar(L"USERPROFILE") + L"\\.android";
    need(!home.empty(), "This phone has no registered Android storage.");
    Paths p;
    p.sdk = info.sdk.empty() ? paths().sdk : info.sdk;
    p.avdHome = home + L"\\avd";
    p.avdDir = p.avdHome + L"\\Gaming_Phone.avd";
    p.rootImage = get(file, L"launcher", L"rootimage");
    p.log = info.data + L"\\emulator.log";
    return p;
}
void put(const std::wstring &file, const wchar_t *section, const wchar_t *key, const std::wstring &value) {
    plain(file);
    need(value.find_first_of(L"\r\n") == std::wstring::npos && WritePrivateProfileStringW(section, key, value.c_str(), file.c_str()),
         "Couldn't save phone storage preferences.");
}
std::wstring identifier() {
    GUID guid{};
    need(SUCCEEDED(CoCreateGuid(&guid)), "Couldn't create a phone identifier.");
    wchar_t text[48]{};
    StringFromGUID2(guid, text, 48);
    std::wstring key;
    for (auto c : std::wstring(text))
        if (iswxdigit(c))
            key += wchar_t(towlower(c));
    need(valid(key), "Couldn't create a phone identifier.");
    return key;
}
bool disposable(const std::wstring &relative) {
    return relative == L"cache.img" || relative == L"cache.img.qcow2" || relative == L"velox-snapshot.ini" || relative == L"snapshots" ||
           relative.rfind(L"snapshots\\", 0) == 0;
}
void switchHome(const Info &info, const std::wstring &home, const std::wstring &rootImage) {
    auto file = info.data + L"\\settings.ini", fresh = file + L".storage-new";
    plain(file);
    plain(fresh);
    need(CopyFileW(file.c_str(), fresh.c_str(), FALSE), "Couldn't preserve the phone settings for the storage switch.");
    put(fresh, L"launcher", L"home", home);
    put(fresh, L"launcher", L"rootimage", rootImage);
    put(fresh, L"launcher", L"quickboot", L"0");
    WritePrivateProfileStringW(L"running", nullptr, nullptr, fresh.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, fresh.c_str());
    need(MoveFileExW(fresh.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
         "Couldn't switch phone storage. The original is still available.");
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, file.c_str());
}
} // namespace
bool rename(const std::wstring &key, const std::wstring &label, const Report &report) {
    try {
        need(key.empty() || valid(key), "Invalid instance identifier.");
        Gate gate;
        need(gate.locked, "Another phone is being prepared. Try again in a moment.");
        auto name = cleanName(label);
        auto info = read(key);
        write(key.empty() ? baseDataFolder() + L"\\identity.ini" : meta(key),
              L"[instance]\r\nname=" + name + L"\r\nport=" + std::to_wstring(info.port) + L"\r\n");
        report(i18n::text(L"Phone name updated."), false);
        return true;
    } catch (const std::exception &e) {
        report(widen(e.what()), true);
        return false;
    }
}
Storage storage(const std::wstring &key) {
    Storage out;
    out.id = key;
    try {
        need(key.empty() || valid(key), "Invalid instance identifier.");
        auto info = read(key);
        auto p = phonePaths(info);
        need(!p.avdDir.empty() && fs::path(p.avdDir).is_absolute() && isDir(p.avdDir) && exists(p.avdDir + L"\\config.ini"),
             "Phone storage was not found. Choose a valid phone folder before maintenance.");
        out.home = fs::path(p.avdHome).parent_path().wstring();
        out.previous = get(info.data + L"\\storage.ini", L"previous", L"home");
        plain(p.avdDir);
        plain(info.data);
        auto scan = [&](const std::wstring &dir, bool phone) {
            if (!isDir(dir))
                return;
            size_t count = 0;
            for (const auto &e : fs::recursive_directory_iterator(dir)) {
                need(++count <= 200000, "Storage contains too many files to scan safely.");
                plain(e.path().wstring());
                if (!e.is_regular_file())
                    continue;
                auto n = e.file_size();
                if (phone) {
                    out.phone += n;
                    DWORD hi = 0;
                    SetLastError(NO_ERROR);
                    DWORD lo = GetCompressedFileSizeW(e.path().c_str(), &hi);
                    out.allocated += lo == INVALID_FILE_SIZE && GetLastError() ? n : (uint64_t(hi) << 32) | lo;
                    if (disposable(fs::relative(e.path(), dir).wstring()))
                        out.reclaimable += n;
                } else
                    out.desktop += n;
            }
        };
        scan(p.avdDir, true);
        // Keep this count local: other instances are separate phones, not desktop overhead.
        for (const auto &e : fs::directory_iterator(info.data)) {
            plain(e.path().wstring());
            if (e.is_regular_file()) {
                out.desktop += e.file_size();
                if (e.path().filename() == L"emulator.log" || e.path().filename() == L"preview.png")
                    out.reclaimable += e.file_size();
            } else if (e.is_directory() && (e.path().filename() == L"controls" || e.path().filename() == L"presets" || e.path().filename() == L"wallpaper-choice"))
                scan(e.path().wstring(), false);
        }
    } catch (const std::exception &e) {
        out.error = widen(e.what());
    }
    return out;
}
bool cleanup(const std::wstring &key, const Report &report) {
    Maintenance lease(key);
    if (!lease.acquired) {
        report(lease.error, true);
        return false;
    }
    try {
        auto info = read(key);
        auto p = phonePaths(info);
        std::vector<fs::path> candidates;
        for (auto name : {L"snapshots", L"cache.img", L"cache.img.qcow2", L"velox-snapshot.ini"})
            candidates.push_back(fs::path(p.avdDir) / name);
        candidates.push_back(p.log);
        candidates.push_back(fs::path(info.data) / L"preview.png");
        for (const auto &path : candidates)
            if (fs::exists(path)) {
                plain(path.wstring());
                if (fs::is_directory(path))
                    for (const auto &e : fs::recursive_directory_iterator(path))
                        plain(e.path().wstring());
            }
        for (const auto &path : candidates)
            if (fs::exists(path))
                recyclePath(path);
        report(i18n::text(L"Snapshots, cache and logs moved to the Recycle Bin. Apps and saves are kept."), false);
        return true;
    } catch (const std::exception &e) {
        report(widen(e.what()), true);
        return false;
    }
}
bool clone(const std::wstring &key, const std::wstring &label, bool full, const Report &report) {
    Maintenance lease(key);
    if (!lease.acquired) {
        report(lease.error, true);
        return false;
    }
    try {
        auto labelClean = cleanName(label);
        auto info = read(key);
        auto p = phonePaths(info);
        auto freshKey = identifier(), target = root() + L"\\" + freshKey;
        plain(target);
        need(!exists(target), "Instance folder already exists.");
        createDirs(target + L"\\data");
        auto home = target + L"\\Android";
        if (full) {
            if (!vault::copyPhone(p, home, report))
                return false;
        } else {
            std::wstring error;
            bool created = setup::createPhoneAt(home, error);
            need(created, narrow(error).c_str());
        }
        auto settings = target + L"\\data\\settings.ini", source = info.data + L"\\settings.ini";
        write(settings, L"[launcher]\r\nconfigured=1\r\nhome=" + home + L"\r\nsdk=" + p.sdk +
                            L"\r\nquickboot=0\r\nautostart=0\r\nnumber=\r\nroot=0\r\n");
        for (auto k :
             {L"screen", L"hz", L"cpu", L"ram", L"gpu", L"snappy", L"boost", L"frame", L"bootscreen", L"bootstyle", L"cursor", L"brand",
              L"language", L"gesturenavigation", L"reversebuttons", L"autorotate", L"touches", L"backgroundfps", L"backgroundmute"}) {
            auto value = get(source, L"launcher", k);
            if (!value.empty())
                put(settings, L"launcher", k, value);
        }
        if (full && get(source, L"launcher", L"root") == L"1") {
            need(exists(home + L"\\root\\ramdisk-root.img"), "The Root boot image is missing.");
            put(settings, L"launcher", L"root", L"1");
            put(settings, L"launcher", L"rootimage", home + L"\\root\\ramdisk-root.img");
        }
        for (auto name : {L"controls", L"presets", L"wallpaper-choice"}) {
            auto from = info.data + L"\\" + name, to = target + L"\\data\\" + name;
            plain(from);
            if (isDir(from))
                for (const auto &e : fs::directory_iterator(from)) {
                    plain(e.path().wstring());
                    bool wallpaper = std::wstring(name) == L"wallpaper-choice";
                    if (e.is_regular_file() && (wallpaper ? wallpaperChoiceFile(e.path().filename().wstring()) : e.path().extension() == L".ini")) {
                        need(e.file_size() <= (wallpaper ? 32u << 20 : 1u << 20), "A profile file is too large.");
                        createDirs(to);
                        need(CopyFileW(e.path().c_str(), (fs::path(to) / e.path().filename()).c_str(), TRUE),
                             "Couldn't copy controls or presets.");
                    }
                }
        }
        // Catalog publication is last. Until then, no launcher can open the copy.
        Gate gate;
        need(gate.locked, "Another phone is being prepared. Try again in a moment.");
        auto phones = list();
        need(phones.size() < 9, "This installation supports the main phone and eight additional phones.");
        std::set<int> used;
        for (auto &i : phones)
            used.insert(i.port);
        int chosen = 0;
        for (int n = 5582; n <= 5682; n += 2)
            if (!used.count(n) && available(n)) {
                chosen = n;
                break;
            }
        need(chosen != 0, "No free Android emulator connection ports are available.");
        write(target + L"\\instance.ini", L"[instance]\r\nname=" + labelClean + L"\r\nport=" + std::to_wstring(chosen) + L"\r\n");
        report(i18n::text(L"Phone copy ready. The source is unchanged."), false);
        return true;
    } catch (const std::exception &e) {
        report(widen(e.what()), true);
        return false;
    }
}
bool relocate(const std::wstring &key, const std::wstring &parent, const Report &report) {
    Maintenance lease(key);
    if (!lease.acquired) {
        report(lease.error, true);
        return false;
    }
    try {
        auto info = read(key);
        auto p = phonePaths(info);
        plain(parent);
        need(isDir(parent) && parent.size() > 2 && parent[1] == L':' && GetDriveTypeW((parent.substr(0, 3)).c_str()) == DRIVE_FIXED &&
                 parent.find_first_of(L"\r\n") == std::wstring::npos,
             "Choose a folder on a local fixed drive.");
        auto target = parent + L"\\Velox Phone " + identifier();
        for (auto &other : list()) {
            auto otherPaths = phonePaths(other);
            need(!inside(target, otherPaths.avdDir) && !inside(target, other.sdk),
                 "Choose storage outside phone data and the shared Android SDK.");
        }
        if (!vault::copyPhone(p, target, report))
            return false;
        auto previousHome = fs::path(p.avdHome).parent_path().wstring();
        write(info.data + L"\\storage.ini",
              L"[previous]\r\nhome=" + previousHome + L"\r\nrootimage=" + p.rootImage + L"\r\nnewhome=" + target + L"\r\n");
        switchHome(info, target, exists(target + L"\\root\\ramdisk-root.img") ? target + L"\\root\\ramdisk-root.img" : L"");
        report(i18n::text(L"Storage moved and verified. The original stays available through Undo move."), false);
        return true;
    } catch (const std::exception &e) {
        report(widen(e.what()), true);
        return false;
    }
}
bool undoRelocation(const std::wstring &key, const Report &report) {
    Maintenance lease(key);
    if (!lease.acquired) {
        report(lease.error, true);
        return false;
    }
    try {
        auto info = read(key);
        auto file = info.data + L"\\storage.ini";
        auto home = get(file, L"previous", L"home"), rootImage = get(file, L"previous", L"rootimage");
        plain(home);
        need(isDir(home + L"\\avd\\Gaming_Phone.avd"), "The previous phone storage is missing.");
        auto active = phonePaths(info);
        auto expected = get(file, L"previous", L"newhome");
        need(!expected.empty() && inside(active.avdDir, expected), "Phone storage has changed since the move. Undo was not applied.");
        switchHome(info, home, rootImage);
        report(i18n::text(L"Previous storage selected. Both copies remain on disk."), false);
        return true;
    } catch (const std::exception &e) {
        report(widen(e.what()), true);
        return false;
    }
}
bool create(const std::wstring &sdk, const Report &report) {
    try {
        Gate gate;
        need(gate.locked, "Another phone is being prepared. Try again in a moment.");
        auto existing = list();
        need(existing.size() < 9, "This installation supports the main phone and eight additional phones.");
        need(!sdk.empty() && exists(sdk + L"\\system-images\\android-35\\google_apis_playstore\\x86_64\\system.img"),
             "Set up Android in the main Velox window first.");
        need(sdk.find_first_of(L"\r\n") == std::wstring::npos && root().find_first_of(L"\r\n") == std::wstring::npos,
             "Instance folder paths cannot contain line breaks.");
        plain(root());
        createDirs(root());
        std::set<int> ports;
        for (auto &i : existing)
            ports.insert(i.port);
        int chosen = 0;
        for (int p = 5582; p <= 5682; p += 2)
            if (!ports.count(p) && available(p)) {
                chosen = p;
                break;
            }
        need(chosen != 0, "No free Android emulator connection ports are available.");
        GUID guid{};
        need(SUCCEEDED(CoCreateGuid(&guid)), "Couldn't create a phone identifier.");
        wchar_t encoded[48]{};
        StringFromGUID2(guid, encoded, 48);
        std::wstring key;
        for (wchar_t c : std::wstring(encoded))
            if (iswxdigit(c))
                key += wchar_t(towlower(c));
        need(valid(key), "Couldn't create a phone identifier.");
        auto final = root() + L"\\" + key, stage = root() + L"\\.creating-" + key;
        need(!exists(final) && !exists(stage), "Instance folder already exists.");
        createDirs(stage + L"\\data");
        std::wstring error;
        if (!setup::createPhoneAt(stage + L"\\Android", error, final + L"\\Android"))
            throw std::runtime_error(narrow(error));
        int number = 2;
        std::wstring label;
        do {
            label = L"Phone " + std::to_wstring(number++);
        } while (std::any_of(existing.begin(), existing.end(), [&](auto &i) { return i.name == label; }));
        write(stage + L"\\instance.ini", L"[instance]\r\nname=" + label + L"\r\nport=" + std::to_wstring(chosen) + L"\r\n");
        write(stage + L"\\data\\settings.ini", L"[launcher]\r\nconfigured=1\r\nhome=" + final + L"\\Android\r\nsdk=" + sdk +
                                                   L"\r\ncpu=2\r\nram=2\r\nhz=60\r\nscreen=fhdplus\r\nroot=0\r\nquickboot=0\r\nframe="
                                                   L"1\r\ngesturenavigation=0\r\nautorotate=1\r\nlanguage=" +
                                                   std::to_wstring(GetPrivateProfileIntW(L"launcher", L"language", 0, iniPath().c_str())) +
                                                   L"\r\n");
        need(MoveFileExW(stage.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "Couldn't publish the new phone.");
        report(label + L" created. Android is shared; its apps and data are separate.", false);
        return true;
    } catch (const std::exception &e) {
        report(widen(e.what()), true);
        return false;
    }
}
bool recycle(const std::wstring &key, const Report &report) {
    try {
        need(valid(key) && key != selected, "Only another stopped additional phone with closed controls can be removed.");
        Gate gate;
        need(gate.locked, "Another phone is being prepared. Try again in a moment.");
        LauncherGuard launcher(key);
        need(launcher.owned, "Stop this phone and close its controls before removing it.");
        auto catalog = fs::absolute(root()).lexically_normal();
        auto target = (catalog / key).lexically_normal();
        need(target.parent_path() == catalog && target.filename() == key, "Invalid instance folder.");
        plain(target.wstring());
        plain(meta(key));
        need(exists(meta(key)) && fs::file_size(meta(key)) < 65536, "This phone is no longer available.");
        auto info = read(key);
        need(!info.active && !info.starting && !info.window, "Stop this phone and close its controls before removing it.");
        need(!inside(paths().sdk, target) && !inside(info.sdk, target),
             "This phone contains an Android SDK. Move the SDK before removing it.");
        for (const auto &other : list()) {
            need(!inside(other.sdk, target), "This phone contains an Android SDK. Move the SDK before removing it.");
            if (other.id != key)
                need(!inside(get(other.data + L"\\settings.ini", L"launcher", L"home"), target),
                     "Another phone uses this folder. Choose separate phone storage before removing it.");
        }
        for (const auto &entry : fs::recursive_directory_iterator(target)) {
            auto attributes = GetFileAttributesW(entry.path().c_str());
            need(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
                 "This phone contains a link or cloud placeholder. Its files were not removed.");
        }
        recyclePath(target);
        report(info.name + i18n::text(L" moved to the Recycle Bin. Restore its folder there to undo removal."), false);
        return true;
    } catch (const std::exception &e) {
        report(widen(e.what()), true);
        return false;
    }
}
bool command(const std::wstring &key, Command action, const Report &report) {
    try {
        need(key.empty() || valid(key), "Invalid instance identifier.");
        if (!key.empty()) {
            plain(meta(key));
            need(exists(meta(key)), "This phone is no longer available.");
        }
        if (auto h = window(key)) {
            if (action == Command::Open) {
                ShowWindowAsync(h, SW_RESTORE);
                SetForegroundWindow(h);
            } else
                PostMessageW(h, CONTROL, static_cast<WPARAM>(action), 0);
            return true;
        }
        if (action == Command::CloseControls)
            return true;
        std::wstring cmd = quote(exePath());
        if (!key.empty())
            cmd += L" --instance " + key;
        if (action == Command::Start)
            cmd += L" --start-phone";
        if (action == Command::Stop)
            cmd += L" --stop-phone";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = action == Command::Open ? SW_SHOWNORMAL : SW_HIDE;
        wchar_t station[256]{}, desktop[256]{};
        DWORD bytes = 0;
        std::wstring surface;
        if (GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, station, sizeof(station), &bytes) &&
            GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof(desktop), &bytes)) {
            surface = std::wstring(station) + L"\\" + desktop;
            si.lpDesktop = surface.data();
        }
        PROCESS_INFORMATION pi{};
        need(CreateProcessW(exePath().c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, exeFolder().c_str(), &si, &pi) != FALSE,
             "Couldn't open that phone's controls.");
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    } catch (const std::exception &e) {
        report(widen(e.what()), true);
        return false;
    }
}
bool otherLaunchers() {
    for (auto &i : list())
        if (!i.current && i.window)
            return true;
    return false;
}
bool ownsWrapper(DWORD wrapper) {
    auto ledger = folder(selected) + L"\\runtime.ini";
    return wrapper && wrapper == DWORD(integer(ledger, L"runtime", L"wrapper")) &&
           alive(wrapper, integer(ledger, L"runtime", L"wrapperborn"),
                 get(ledger, L"runtime", L"sdk", paths().sdk.c_str()) + L"\\emulator\\emulator.exe");
}
void failure(const std::wstring &message) {
    try {
        auto clean = message.substr(0, 1024);
        for (auto &c : clean)
            if (c < 32)
                c = L' ';
        write(folder(selected) + L"\\status.ini", L"[phone]\r\nerror=" + clean + L"\r\n");
    } catch (...) {
    }
}
bool budget(const Settings &s, std::wstring &error) {
    try {
        // The pinned Play image raises 2-GB requests to 2560 MB. Reserve whole
        // gigabytes conservatively without changing the user's requested target.
        int cpu = coresFor(s), ram = std::max(3, memoryFor(s));
        for (auto &i : list())
            if (!i.current && i.active) {
                cpu += i.cores;
                ram += i.memory;
            }
        int limit = std::max(std::max(3, memoryFor(s)), host().ramGB - std::max(3, host().ramGB / 4));
        need(cpu <= host().threads, "The running phones exceed this PC's CPU budget. Lower a phone's core allocation first.");
        need(ram <= limit, "The running phones exceed the safe RAM budget. Lower a phone's memory allocation first.");
        return true;
    } catch (const std::exception &e) {
        error = widen(e.what());
        return false;
    }
}
bool reserve(const Settings &s, std::wstring &error) {
    try {
        Gate gate;
        need(gate.locked, "Another phone is starting. Try again in a moment.");
        auto maintenance = CreateMutexW(nullptr, FALSE, (mutexFor(selected) + L"-maintenance").c_str());
        need(maintenance != nullptr, "Couldn't check phone maintenance ownership.");
        auto wait = WaitForSingleObject(maintenance, 0);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED)
            ReleaseMutex(maintenance);
        CloseHandle(maintenance);
        need(wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED, "This phone is being copied or maintained. Try again when it finishes.");
        if (!budget(s, error))
            return false;
        if (!selected.empty()) {
            auto phones = list();
            need(phones.size() <= 9, "Too many phones are present. Remove another additional phone before starting this one.");
            std::set<int> used;
            for (const auto &other : phones)
                if (other.id != selected)
                    used.insert(other.port);
            if (used.count(selectedPort)) {
                int replacement = 0;
                for (int candidate = 5582; candidate <= 5682; candidate += 2)
                    if (!used.count(candidate) && available(candidate)) {
                        replacement = candidate;
                        break;
                    }
                need(replacement != 0, "No free Android emulator connection ports are available.");
                write(meta(selected), L"[instance]\r\nname=" + name() + L"\r\nport=" + std::to_wstring(replacement) + L"\r\n");
                selectedPort = replacement;
            }
        }
        need(available(port()), "This phone's connection ports are already in use. Close its previous emulator before starting it again.");
        std::lock_guard<std::mutex> lock(runtimeLock);
        runtime = {GetCurrentProcessId(), 0, born(GetCurrentProcessId()), 0, coresFor(s), std::max(3, memoryFor(s)), port(), paths().sdk};
        saveRuntime();
        failure(L"");
        return true;
    } catch (const std::exception &e) {
        error = widen(e.what());
        return false;
    }
}
bool started(DWORD wrapper, const std::wstring &sdk, int actualPort) {
    std::lock_guard<std::mutex> guard(runtimeLock);
    runtime.wrapper = wrapper;
    runtime.wrapperBorn = born(wrapper);
    runtime.sdk = sdk;
    runtime.port = actualPort;
    try {
        saveRuntime();
        return true;
    } catch (...) {
        return false;
    }
}
void lowerCpuAllocation(int actual) {
    std::lock_guard<std::mutex> guard(runtimeLock);
    if (actual < 1 || actual >= runtime.cores || !runtime.wrapper)
        return;
    runtime.cores = actual;
    try {
        saveRuntime();
    } catch (...) {
    } // A stale higher reservation remains conservative.
}
void stopped() {
    std::lock_guard<std::mutex> guard(runtimeLock);
    runtime = {};
    try {
        saveRuntime();
    } catch (...) {
    }
}
} // namespace phone::instances
