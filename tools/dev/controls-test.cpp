#include "playtools.cpp"
#include <iostream>
#include <stdexcept>
void require(bool v, const char *text) {
    if (!v)
        throw std::runtime_error(text);
}
int main() {
    using namespace phone::playtools;
    try {
        HWND foreground = GetForegroundWindow();
        POINT cursorBefore;
        GetCursorPos(&cursorBefore);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token;
        Gdiplus::GdiplusStartup(&token, &input, nullptr);
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = DefWindowProcW;
        wc.lpszClassName = L"VeloxTestSurface";
        RegisterClassW(&wc);
        HWND parent = CreateWindowW(wc.lpszClassName, L"Offscreen test parent", WS_POPUP, -20000, -20000, 300, 600, nullptr, nullptr,
                                    wc.hInstance, nullptr);
        display =
            CreateWindowW(wc.lpszClassName, L"Offscreen test display", WS_CHILD, 0, 0, 300, 600, parent, nullptr, wc.hInstance, nullptr);
        wc.lpfnWndProc = editorProc;
        wc.lpszClassName = L"VeloxControlStudio";
        RegisterClassW(&wc);
        phone::state = phone::State::Running;
        selectProfile(L"app.velox.editortest", false);
        {
            std::lock_guard<std::mutex> g(lock);
            current.bindings.clear();
            current.enabled = false;
            persist();
        }
        edit(false);
        require(editor && GetParent(editor) == display, "Editor was not attached to the display");
        SendMessageW(editor, WM_LBUTTONDOWN, 0, MAKELPARAM(150, 300));
        SendMessageW(editor, WM_LBUTTONUP, 0, MAKELPARAM(150, 300));
        SendMessageW(editor, WM_KEYDOWN, 'K', 0);
        require(profile().bindings.size() == 1 && profile().bindings[0].key == 'K', "Click and key capture did not create a binding");
        SendMessageW(editor, WM_LBUTTONDOWN, 0, MAKELPARAM(150, 300));
        SendMessageW(editor, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(210, 360));
        SendMessageW(editor, WM_LBUTTONUP, 0, MAKELPARAM(210, 360));
        require(std::abs(profile().bindings[0].x - .7f) < .001f && std::abs(profile().bindings[0].y - .6f) < .001f,
                "Binding drag did not use normalized coordinates");
        for (int i = 0; i < 250; i++) {
            SetWindowPos(parent, nullptr, -20000 + i * 2, -20000 + i, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT a, b;
            GetWindowRect(editor, &a);
            GetWindowRect(display, &b);
            require(a.left == b.left && a.top == b.top, "Editor drifted during movement");
        }
        SendMessageW(editor, WM_CLOSE, 0, 0);
        require(!editor && IsWindow(display), "Closing editor damaged the emulator display");
        selectProfile(L"app.velox.editortest.other", false);
        selectProfile(L"app.velox.editortest", false);
        require(profile().bindings.size() == 1 && profile().bindings[0].key == 'K', "Edited binding was not saved");
        phonePid = 999999;
        focusEmu = parent;
        KBDLLHOOKSTRUCT key{};
        key.vkCode = 'K';
        keyProc(HC_ACTION, WM_KEYDOWN, reinterpret_cast<LPARAM>(&key));
        require(events.empty(), "Mapped input escaped the phone focus boundary");
        POINT cursorAfter;
        GetCursorPos(&cursorAfter);
        require(GetForegroundWindow() == foreground && cursorAfter.x == cursorBefore.x && cursorAfter.y == cursorBefore.y,
                "Test changed foreground or cursor");
        DestroyWindow(parent);
        display = nullptr;
        std::cout << "PASS: editor placement, key capture, dragging, persistence, 250 moves, close safety and focus boundary; "
                     "foreground/cursor unchanged\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
