#include "phone.cpp"
#include "playtools.cpp"
#include <iostream>
#include <stdexcept>

void require(bool value, const char *text) {
    if (!value)
        throw std::runtime_error(text);
}
int main(int argc, char **argv) {
    try {
        require(argc == 2, "Need our hidden emulator wrapper PID");
        phone::Settings settings;
        bool first = false;
        phone::loadSettings(settings, first);
        phone::findPaths();
        phone::g_launchedPid = std::stoul(argv[1]);
        phone::g_emuProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, phone::g_launchedPid);
        require(phone::g_emuProcess && phone::emulatorRunning(), "Owned test phone missing");
        phone::state = phone::State::Running;
        phone::running = settings;
        auto report = [](const std::wstring &text, bool error) {
            if (error)
                std::cerr << phone::narrow(text) << "\n";
        };
        phone::experience::load();
        phone::experience::hostPreferences(settings);
        require(phone::experience::ensureCompanion(report), "Companion installation failed");
        phone::experience::requestLibrary();
        phone::experience::tick(report);
        auto apps = phone::experience::library();
        auto app = std::find_if(apps.begin(), apps.end(), [](auto &a) { return a.package == L"app.velox.inputprobe"; });
        require(app != apps.end() && app->game && phone::exists(app->icon), "Real catalog, game metadata or icon missing");
        auto probe = *app;
        require(!phone::experience::validComponent(L"com.example.app/.Main;reboot"), "Invalid launch component accepted");
        phone::experience::favorite(probe, report);
        phone::experience::tick(report);
        apps = phone::experience::library();
        app = std::find_if(apps.begin(), apps.end(), [](auto &a) { return a.package == L"app.velox.inputprobe"; });
        require(app != apps.end() && app->favorite != probe.favorite, "Favorite didn't synchronize");
        phone::experience::favorite(*app, report);
        phone::experience::classify(probe, report);
        phone::experience::tick(report);
        apps = phone::experience::library();
        app = std::find_if(apps.begin(), apps.end(), [](auto &a) { return a.package == L"app.velox.inputprobe"; });
        require(app != apps.end() && !app->game, "Game classification didn't synchronize");
        phone::experience::classify(*app, report);
        require(phone::experience::launch(probe.component, report), "Library launch failed");
        Sleep(1000);
        require(phone::adb(L"shell \"dumpsys window | grep -m1 mCurrentFocus\"").find("app.velox.inputprobe/") != std::string::npos,
                "Library launch did not focus the probe");
        std::cout << "PASS: real app catalog, icons, favorites, game classification and launch\n" << std::flush;

        using namespace phone::playtools;
        selectProfile(probe.package, false);
        {
            std::lock_guard<std::mutex> guard(lock);
            current.enabled = true;
            Binding stick;
            stick.kind = Kind::Stick;
            stick.x = .2f;
            stick.y = .7f;
            Binding tap;
            tap.key = VK_SPACE;
            tap.x = .8f;
            tap.y = .7f;
            Binding swipe;
            swipe.kind = Kind::Swipe;
            swipe.key = 'E';
            Binding aim;
            aim.kind = Kind::Aim;
            current.bindings = {stick, tap, swipe, aim};
            persist();
        }
        selectProfile(L"app.velox.inputprobe.other", false);
        selectProfile(probe.package, false);
        require(profile().bindings.size() == 4 && profile().enabled, "Profile did not persist");
        selectProfile(probe.package, true);
        require(profile().bindings.empty(), "Orientation profiles were mixed");
        selectProfile(probe.package, false);
        {
            std::lock_guard<std::mutex> guard(lock);
            viewport = {0, 0, 1080, 2400};
            cancel();
            keyInput('W', true);
            keyInput('S', true);
            keyInput('S', false);
            keyInput('W', false);
            require(events.size() == 4 && events[0].op == 'D' && events[1].op == 'U' && events[2].op == 'D' && events[3].op == 'U',
                    "Opposing joystick keys lost their touch contact");
            cancel();
            keyInput('W', true);
            keyInput(VK_SPACE, true);
            keyInput(VK_SPACE, false);
            keyInput('W', false);
            require(events.size() == 4 && events[0].id == 0 && events[1].id == 1 && events[2].id == 1 && events[3].id == 0,
                    "Simultaneous controls lost pointer IDs");
            cancel();
        }
        std::cout << "PASS: per-game/orientation persistence, opposing WASD keys and simultaneous bindings\n" << std::flush;

        phone::adb(L"shell am start -W -n app.velox.inputprobe/.Probe --ei orientation 1", 10000);
        Sleep(300);
        phone::adb(L"shell run-as app.velox.inputprobe rm -f files/events.tsv");
        InputPipe pipe;
        require(pipe.open(), "Persistent input pipe failed to connect");
        require(pipe.send("D 0 2000 7000\n"), "Down failed");
        Sleep(30);
        require(pipe.send("D 1 8000 7000\nM 0 2500 6500\n"), "Multi-touch failed");
        Sleep(30);
        require(pipe.send("U 1\nU 0\nD 2 3000 4000\nM 2 7000 4000\nU 2\nD 3 5000 5000\nC\n"), "Swipe/cancel failed");
        Sleep(200);
        pipe.send("Q\n");
        pipe.close();
        std::string touches = phone::adb(L"shell run-as app.velox.inputprobe cat files/events.tsv");
        require(touches.find("\t5\t1\t2\t") != std::string::npos, "Android didn't receive simultaneous pointers");
        require(touches.find("\t3\t0\t1\t3:") != std::string::npos, "Android didn't receive touch cancellation");
        std::ofstream results{std::filesystem::path(phone::exeFolder() + L"experience\\touch-events.tsv")};
        results << touches;
        std::cout << "PASS: non-root Android multi-touch, swipe and cancellation over the production input pipe\n" << std::flush;

        phone::experience::openSettings(report);
        Sleep(300);
        phone::adb(L"shell uiautomator dump /data/local/tmp/velox-settings-test.xml", 10000);
        std::string xml = phone::adb(L"shell cat /data/local/tmp/velox-settings-test.xml");
        std::regex toggle("text=\"Automatically follow game orientation\"[^>]*bounds=\"\\[([0-9]+),([0-9]+)\\]\\[([0-9]+),([0-9]+)\\]\"");
        std::smatch match;
        require(std::regex_search(xml, match, toggle), "Orientation setting not accessible");
        int x = (std::stoi(match[1]) + std::stoi(match[3])) / 2, y = (std::stoi(match[2]) + std::stoi(match[4])) / 2;
        phone::adb(L"shell input tap " + std::to_wstring(x) + L" " + std::to_wstring(y));
        Sleep(150);
        auto change = phone::experience::tick(report);
        require(change && change->rotate != settings.autoRotate, "Phone-to-desktop settings didn't arrive");
        phone::adb(L"shell input tap " + std::to_wstring(x) + L" " + std::to_wstring(y));
        Sleep(150);
        change = phone::experience::tick(report);
        require(change && change->rotate == settings.autoRotate, "Phone preference restoration failed");
        std::cout << "PASS: in-phone settings changes reach desktop and restore correctly\n" << std::flush;
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
