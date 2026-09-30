#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>
#include <cstdlib>

namespace fs = std::filesystem;

namespace {
constexpr int IDC_CANDIDATES = 1001;
constexpr int IDC_REPAIR = 1002;
constexpr int IDC_STATUS = 1003;
constexpr int IDC_LOG = 1004;
constexpr int ID_HELP = 1005;
constexpr int IDC_HINT = 1006;
constexpr int IDC_TITLE = 1007;
constexpr int IDC_MINIMIZE = 1008;
constexpr int IDC_CLOSE = 1009;
constexpr int IDC_LOG_EDIT = 1010;
constexpr int IDC_SPLITTER = 1011;
constexpr int IDC_REFRESH_DESKTOP = 1013;
constexpr int IDC_RESTART_EXPLORER = 1014;
constexpr int IDI_APP = 101;
constexpr int kMaxCandidates = 30;

struct ShortcutInfo {
    std::wstring path;
    std::wstring target;
    std::wstring arguments;
    std::wstring icon;
    bool isUrl = false;
};

struct IconCandidate {
    std::wstring label;
    std::wstring path;
};

struct VdfNode {
    std::wstring key;
    std::wstring value;
    std::vector<VdfNode> children;
    bool object = false;
};

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

bool EndsWithI(std::wstring_view value, std::wstring_view suffix) {
    return value.size() >= suffix.size() && _wcsicmp(value.data() + value.size() - suffix.size(), std::wstring(suffix).c_str()) == 0;
}

std::wstring Trim(std::wstring value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring ReadProfileString(const std::wstring& file, const wchar_t* section, const wchar_t* key) {
    std::wstring value(32768, L'\0');
    const DWORD length = GetPrivateProfileStringW(section, key, L"", value.data(), static_cast<DWORD>(value.size()), file.c_str());
    value.resize(length);
    return value;
}

struct ComPtrShellLink {
    IShellLinkW* link = nullptr;
    ~ComPtrShellLink() { if (link) link->Release(); }
};

ShortcutInfo ReadShortcut(const std::wstring& path) {
    ShortcutInfo result;
    result.path = path;
    result.isUrl = EndsWithI(path, L".url");
    if (result.isUrl) {
        result.target = ReadProfileString(path, L"InternetShortcut", L"URL");
        result.icon = ReadProfileString(path, L"InternetShortcut", L"IconFile");
        return result;
    }

    ComPtrShellLink shellLink;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink.link));
    if (FAILED(hr)) throw std::runtime_error("无法创建 Windows 快捷方式对象。");
    IPersistFile* persist = nullptr;
    hr = shellLink.link->QueryInterface(IID_PPV_ARGS(&persist));
    if (FAILED(hr)) throw std::runtime_error("无法读取快捷方式文件。");
    hr = persist->Load(path.c_str(), STGM_READ);
    if (FAILED(hr)) { persist->Release(); throw std::runtime_error("快捷方式无法打开。"); }

    wchar_t buffer[32768]{};
    WIN32_FIND_DATAW findData{};
    shellLink.link->GetPath(buffer, static_cast<int>(std::size(buffer)), &findData, SLGP_RAWPATH);
    result.target = buffer;
    std::fill(std::begin(buffer), std::end(buffer), L'\0');
    shellLink.link->GetArguments(buffer, static_cast<int>(std::size(buffer)));
    result.arguments = buffer;
    persist->Release();
    return result;
}

void SetShortcutIcon(const ShortcutInfo& shortcut, const std::wstring& exePath) {
    if (shortcut.isUrl) {
        if (!WritePrivateProfileStringW(L"InternetShortcut", L"IconFile", exePath.c_str(), shortcut.path.c_str()))
            throw std::runtime_error("写入 .url 图标路径失败。");
        if (!WritePrivateProfileStringW(L"InternetShortcut", L"IconIndex", L"0", shortcut.path.c_str()))
            throw std::runtime_error("写入 .url 图标索引失败。");
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, shortcut.path.c_str(), nullptr);
        return;
    }

    ComPtrShellLink shellLink;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink.link));
    if (FAILED(hr)) throw std::runtime_error("无法创建 Windows 快捷方式对象。");
    IPersistFile* persist = nullptr;
    hr = shellLink.link->QueryInterface(IID_PPV_ARGS(&persist));
    if (FAILED(hr)) throw std::runtime_error("无法写入快捷方式。");
    hr = persist->Load(shortcut.path.c_str(), STGM_READWRITE);
    if (SUCCEEDED(hr)) hr = shellLink.link->SetIconLocation(exePath.c_str(), 0);
    if (SUCCEEDED(hr)) hr = persist->Save(nullptr, TRUE);
    persist->Release();
    if (FAILED(hr)) throw std::runtime_error("快捷方式保存失败，请检查文件权限。");
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, shortcut.path.c_str(), nullptr);
}

std::optional<std::wstring> FindAppId(const std::wstring& text) {
    const auto lower = Lower(text);
    for (const wchar_t* marker : {L"rungameid/", L"applaunch/"}) {
        size_t pos = lower.find(marker);
        if (pos != std::wstring::npos) {
            pos += wcslen(marker);
            size_t end = pos;
            while (end < lower.size() && iswdigit(lower[end])) ++end;
            if (end > pos) return text.substr(pos, end - pos);
        }
    }
    size_t pos = 0;
    while ((pos = lower.find(L"-applaunch", pos)) != std::wstring::npos) {
        size_t start = pos + 10;
        while (start < lower.size() && iswspace(lower[start])) ++start;
        size_t end = start;
        while (end < lower.size() && iswdigit(lower[end])) ++end;
        if (end > start) return text.substr(start, end - start);
        pos = start;
    }
    return std::nullopt;
}

std::wstring ReadRegistryString(HKEY root, const wchar_t* subkey, const wchar_t* name, REGSAM view) {
    DWORD bytes = 0;
    if (RegGetValueW(root, subkey, name, RRF_RT_REG_SZ | view, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, subkey, name, RRF_RT_REG_SZ | view, nullptr, value.data(), &bytes) != ERROR_SUCCESS) return {};
    value.resize(wcslen(value.c_str()));
    return value;
}

std::wstring FindSteamRoot(const std::wstring& target) {
    if (!target.empty()) {
        std::error_code ec;
        fs::path p(target);
        if (fs::is_regular_file(p, ec)) p = p.parent_path();
        if (fs::is_directory(p, ec)) {
            auto candidate = p;
            if (Lower(candidate.filename().wstring()) == L"steam.exe") candidate = candidate.parent_path();
            if (fs::exists(candidate / L"steamapps", ec)) return candidate.wstring();
        }
    }
    for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        auto path = ReadRegistryString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", view);
        if (path.empty()) path = ReadRegistryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", view);
        if (!path.empty()) return path;
    }
    return {};
}

std::vector<std::wstring> TokenizeVdf(const std::wstring& input) {
    std::vector<std::wstring> tokens;
    size_t i = 0;
    while (i < input.size()) {
        if (iswspace(input[i])) { ++i; continue; }
        if (input[i] == L'/' && i + 1 < input.size() && input[i + 1] == L'/') {
            while (i < input.size() && input[i] != L'\n') ++i;
            continue;
        }
        if (input[i] == L'{' || input[i] == L'}') { tokens.emplace_back(1, input[i++]); continue; }
        if (input[i] == L'"') {
            ++i;
            std::wstring token;
            while (i < input.size() && input[i] != L'"') {
                if (input[i] == L'\\' && i + 1 < input.size()) {
                    ++i;
                    if (input[i] == L't') token.push_back(L'\t');
                    else if (input[i] == L'n') token.push_back(L'\n');
                    else token.push_back(input[i]);
                    ++i;
                } else token.push_back(input[i++]);
            }
            if (i < input.size()) ++i;
            tokens.push_back(std::move(token));
            continue;
        }
        size_t start = i;
        while (i < input.size() && !iswspace(input[i]) && input[i] != L'{' && input[i] != L'}') ++i;
        tokens.push_back(input.substr(start, i - start));
    }
    return tokens;
}

std::vector<VdfNode> ParseVdfObject(const std::vector<std::wstring>& tokens, size_t& i, bool nested) {
    std::vector<VdfNode> nodes;
    while (i < tokens.size()) {
        if (tokens[i] == L"}") { if (nested) ++i; break; }
        VdfNode node;
        node.key = tokens[i++];
        if (i >= tokens.size()) { nodes.push_back(std::move(node)); break; }
        if (tokens[i] == L"{") {
            ++i;
            node.object = true;
            node.children = ParseVdfObject(tokens, i, true);
        } else {
            node.value = tokens[i++];
        }
        nodes.push_back(std::move(node));
    }
    return nodes;
}

void CollectKey(const std::vector<VdfNode>& nodes, std::wstring_view key, std::vector<std::wstring>& values) {
    for (const auto& node : nodes) {
        if (_wcsicmp(node.key.c_str(), std::wstring(key).c_str()) == 0 && !node.object) values.push_back(node.value);
        CollectKey(node.children, key, values);
    }
}

std::wstring ReadTextFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::string bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff && static_cast<unsigned char>(bytes[1]) == 0xfe) {
        std::wstring text((bytes.size() - 2) / sizeof(wchar_t), L'\0');
        memcpy(text.data(), bytes.data() + 2, text.size() * sizeof(wchar_t));
        return text;
    }
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xef && static_cast<unsigned char>(bytes[1]) == 0xbb && static_cast<unsigned char>(bytes[2]) == 0xbf) bytes.erase(0, 3);
    if (bytes.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (count > 0) {
        std::wstring text(count, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), count);
        return text;
    }
    const int ansiCount = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(ansiCount, L'\0');
    if (ansiCount) MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), ansiCount);
    return text;
}

std::optional<std::wstring> FindGameDirectory(const std::wstring& root, const std::wstring& appId) {
    std::vector<fs::path> libraries{root};
    const fs::path libraryFile = fs::path(root) / L"steamapps" / L"libraryfolders.vdf";
    const auto libraryText = ReadTextFile(libraryFile);
    if (!libraryText.empty()) {
        auto tokens = TokenizeVdf(libraryText);
        size_t i = 0;
        auto tree = ParseVdfObject(tokens, i, false);
        std::vector<std::wstring> paths;
        CollectKey(tree, L"path", paths);
        for (const auto& path : paths) libraries.emplace_back(path);
    }
    std::vector<std::wstring> visited;
    for (const auto& library : libraries) {
        auto normalized = Lower(library.lexically_normal().wstring());
        if (std::find(visited.begin(), visited.end(), normalized) != visited.end()) continue;
        visited.push_back(std::move(normalized));
        const auto manifest = library / L"steamapps" / (L"appmanifest_" + appId + L".acf");
        auto text = ReadTextFile(manifest);
        if (text.empty()) continue;
        auto tokens = TokenizeVdf(text);
        size_t i = 0;
        auto tree = ParseVdfObject(tokens, i, false);
        std::vector<std::wstring> dirs;
        CollectKey(tree, L"installdir", dirs);
        if (!dirs.empty() && !dirs.front().empty()) return (library / L"steamapps" / L"common" / dirs.front()).wstring();
    }
    return std::nullopt;
}

std::wstring ErrorText(const std::exception& ex) {
    const std::string message = ex.what();
    const int count = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, nullptr, 0);
    if (count <= 1) return L"操作失败";
    std::wstring result(count - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, result.data(), count);
    return result;
}

void RefreshDesktop() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
    HWND shellView = nullptr;
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        HWND view = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
        if (view) {
            *reinterpret_cast<HWND*>(param) = view;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&shellView));
    if (shellView) SendMessageW(shellView, WM_COMMAND, 0x7103, 0);
}

bool ClearIconCache() {
    wchar_t systemDirectory[MAX_PATH]{};
    if (!GetSystemDirectoryW(systemDirectory, MAX_PATH)) return false;
    const std::wstring executable = std::wstring(systemDirectory) + L"\\ie4uinit.exe";
    if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    std::wstring commandLine = L"\"" + executable + L"\" -ClearIconCache";
    std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
    commandBuffer.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(executable.c_str(), commandBuffer.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!created) return false;
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
    return true;
}

bool RestartExplorer() {
    wchar_t systemDirectory[MAX_PATH]{};
    if (!GetSystemDirectoryW(systemDirectory, MAX_PATH)) return false;
    const std::wstring taskkill = std::wstring(systemDirectory) + L"\\taskkill.exe";
    std::wstring commandLine = L"\"" + taskkill + L"\" /f /im explorer.exe";
    std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
    commandBuffer.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(taskkill.c_str(), commandBuffer.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return false;
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Sleep(500);
    HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

class App {
public:
    HWND window = nullptr;
    HWND status = nullptr, hint = nullptr, candidatesList = nullptr, repairButton = nullptr, logEdit = nullptr, splitter = nullptr, refreshDesktop = nullptr, restartExplorer = nullptr;
    HIMAGELIST imageList = nullptr;
    std::vector<IconCandidate> icons;
    std::optional<ShortcutInfo> shortcut;
    HFONT font = nullptr;
    HFONT titleFont = nullptr;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH panelBrush = nullptr;
    HBRUSH editBrush = nullptr;
    UINT dpi = 96;
    int width = 760, height = 570;
    int splitY = 0;
    bool draggingSplitter = false;

    ~App() {
        if (imageList) ImageList_Destroy(imageList);
        if (font) DeleteObject(font);
        if (titleFont) DeleteObject(titleFont);
        if (backgroundBrush) DeleteObject(backgroundBrush);
        if (panelBrush) DeleteObject(panelBrush);
        if (editBrush) DeleteObject(editBrush);
    }

    int Scale(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }

    void Log(const std::wstring& message) {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        wchar_t prefix[32]{};
        swprintf_s(prefix, L"[%02u:%02u:%02u] ", time.wHour, time.wMinute, time.wSecond);
        std::wstring line = prefix + message + L"\r\n";
        const int end = GetWindowTextLengthW(logEdit);
        SendMessageW(logEdit, EM_SETSEL, end, end);
        SendMessageW(logEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        std::wofstream file(fs::path(temp) / L"SteamIconFix.log", std::ios::app);
        if (file) file << line;
    }

    void SetStatus(const std::wstring& text) { SetWindowTextW(status, text.c_str()); }

    void Layout() {
        RECT rc{};
        GetClientRect(window, &rc);
        const int w = rc.right, h = rc.bottom;
        width = w;
        height = h;
        const int margin = Scale(28), groupGap = Scale(12), buttonH = Scale(40);
        if (splitY == 0) splitY = h - Scale(220);
        splitY = std::clamp(splitY, Scale(180), std::max(Scale(181), h - Scale(130)));
        const int bottomY = h - margin - buttonH;
        MoveWindow(GetDlgItem(window, ID_HELP), w - margin - Scale(92), Scale(18), Scale(92), Scale(34), TRUE);
        MoveWindow(hint, margin, Scale(20), w - margin * 2 - Scale(112), Scale(34), TRUE);
        MoveWindow(status, margin, Scale(62), w - margin * 2, Scale(48), TRUE);
        MoveWindow(candidatesList, margin, Scale(122), w - margin * 2, splitY - Scale(122), TRUE);
        MoveWindow(splitter, margin, splitY, w - margin * 2, Scale(7), TRUE);
        MoveWindow(GetDlgItem(window, IDC_LOG), margin, splitY + Scale(16), w - margin * 2, Scale(24), TRUE);
        const int logTop = splitY + Scale(46);
        const int logBottomGap = Scale(16);
        MoveWindow(logEdit, margin, logTop, w - margin * 2,
            std::max(Scale(70), bottomY - logBottomGap - logTop), TRUE);
        const int repairW = Scale(190), refreshW = Scale(120), restartW = Scale(155);
        const int restartX = w - margin - restartW;
        const int refreshX = restartX - groupGap - refreshW;
        MoveWindow(repairButton, margin, bottomY, repairW, buttonH, TRUE);
        MoveWindow(refreshDesktop, refreshX, bottomY, refreshW, buttonH, TRUE);
        MoveWindow(restartExplorer, restartX, bottomY, restartW, buttonH, TRUE);
        ListView_SetColumnWidth(candidatesList, 0, std::max(Scale(300), w - margin * 2 - Scale(18)));
    }

    void AddCandidate(const std::wstring& path, const std::wstring& label) {
        if (icons.size() >= kMaxCandidates) return;
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) return;
        SHFILEINFOW info{};
        if (!SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES)) return;
        const int image = ImageList_AddIcon(imageList, info.hIcon);
        DestroyIcon(info.hIcon);
        if (image < 0) return;
        icons.push_back({label, path});
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = static_cast<int>(icons.size() - 1);
        item.iImage = image;
        item.pszText = const_cast<wchar_t*>(icons.back().label.c_str());
        ListView_InsertItem(candidatesList, &item);
    }

    void LoadShortcut(const std::wstring& path) {
        Log(L"读取快捷方式：" + path);
        shortcut = ReadShortcut(path);
        icons.clear();
        ListView_DeleteAllItems(candidatesList);
        ImageList_RemoveAll(imageList);
        EnableWindow(repairButton, FALSE);
        const auto appId = FindAppId(shortcut->target + L" " + shortcut->arguments);
        const auto steamRoot = FindSteamRoot(shortcut->target);
        std::optional<std::wstring> gameDir;
        if (appId && !steamRoot.empty()) gameDir = FindGameDirectory(steamRoot, *appId);

        std::wstring detail = L"AppID: " + (appId ? *appId : L"未找到") + L"    游戏目录: " + (gameDir ? *gameDir : L"未定位");
        SetStatus(detail);
        Log(L"目标：" + shortcut->target);
        Log(L"AppID=" + (appId ? *appId : L"(未找到)"));
        Log(L"SteamRoot=" + (steamRoot.empty() ? L"(未找到)" : steamRoot));
        Log(L"GameDir=" + (gameDir ? *gameDir : L"(未找到)"));

        AddCandidate(shortcut->target, L"快捷方式目标");
        if (gameDir && fs::is_directory(*gameDir)) {
            std::error_code ec;
            constexpr int kMaxScanDepth = 3;
            fs::directory_options options = fs::directory_options::skip_permission_denied;
            for (fs::recursive_directory_iterator it(*gameDir, options, ec), end;
                 !ec && it != end && icons.size() < kMaxCandidates; it.increment(ec)) {
                if (it.depth() >= kMaxScanDepth && it->is_directory(ec)) {
                    it.disable_recursion_pending();
                    continue;
                }
                if (it->is_regular_file(ec) && EndsWithI(it->path().extension().wstring(), L".exe"))
                    AddCandidate(it->path().wstring(), it->path().filename().wstring());
            }
        }
        if (icons.empty()) {
            SetStatus(detail + L"    未找到可用图标");
            Log(L"未找到可用图标");
        } else {
            ListView_SetItemState(candidatesList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            EnableWindow(repairButton, TRUE);
        }
        Log(L"候选图标数：" + std::to_wstring(icons.size()));
    }

    void Repair() {
        const int selected = ListView_GetNextItem(candidatesList, -1, LVNI_SELECTED);
        if (!shortcut || selected < 0 || static_cast<size_t>(selected) >= icons.size()) return;
        try {
            SetShortcutIcon(*shortcut, icons[selected].path);
            SetStatus(L"修复完成。由于 Windows 图标缓存，显示可能延迟；点击右下角“重启资源管理器”可一次性呈现之前所有修复结果。");
            Log(L"图标已写回快捷方式（候选 " + std::to_wstring(selected + 1) + L"）：" + icons[selected].path);
            Log(L"提示：图标缓存可能导致显示延迟；点击右下角“重启资源管理器”可一次性呈现之前所有修复结果");
        } catch (const std::exception& ex) {
            const auto message = ErrorText(ex);
            SetStatus(L"修复失败：" + message);
            Log(L"修复异常：" + message);
        }
    }

    static LRESULT CALLBACK SplitterProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, message, wp, lp);
        switch (message) {
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
            return TRUE;
        case WM_LBUTTONDOWN:
            self->draggingSplitter = true;
            SetCapture(hwnd);
            return 0;
        case WM_MOUSEMOVE:
            if (self->draggingSplitter) {
                POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ClientToScreen(hwnd, &point);
                ScreenToClient(self->window, &point);
                self->splitY = point.y;
                self->Layout();
            }
            return 0;
        case WM_LBUTTONUP:
            self->draggingSplitter = false;
            ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            self->draggingSplitter = false;
            return 0;
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }

    void Drop(HDROP drop) {
        const UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
        wchar_t path[32768]{};
        if (count) DragQueryFileW(drop, 0, path, static_cast<UINT>(std::size(path)));
        DragFinish(drop);
        const std::wstring value(path);
        if (!EndsWithI(value, L".lnk") && !EndsWithI(value, L".url")) {
            SetStatus(L"请拖入 .lnk 或 .url 快捷方式");
            Log(L"忽略非快捷方式文件：" + value);
            return;
        }
        try { LoadShortcut(value); }
        catch (const std::exception& ex) {
            const auto message = ErrorText(ex);
            SetStatus(L"读取失败：" + message);
            Log(L"读取异常：" + message);
        }
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto create = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<App*>(create->lpCreateParams);
            self->window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wp, lp);
        switch (message) {
        case WM_CREATE: {
            self->dpi = GetDpiForWindow(hwnd);
            self->font = CreateFontW(-self->Scale(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            self->titleFont = CreateFontW(-self->Scale(21), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            self->backgroundBrush = CreateSolidBrush(RGB(245, 247, 250));
            self->panelBrush = CreateSolidBrush(RGB(255, 255, 255));
            self->editBrush = CreateSolidBrush(RGB(250, 251, 253));
            auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND h = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
                SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(self->font), TRUE);
                if (wcscmp(cls, L"BUTTON") == 0 || wcscmp(cls, WC_LISTVIEWW) == 0 || wcscmp(cls, L"EDIT") == 0)
                    SetWindowTheme(h, L"Explorer", nullptr);
                return h;
            };
            self->hint = make(L"STATIC", L"将桌面上的 Steam 快捷方式拖到这里", SS_LEFT, IDC_HINT);
            SendMessageW(self->hint, WM_SETFONT, reinterpret_cast<WPARAM>(self->titleFont), TRUE);
            self->status = make(L"STATIC", L"支持 .lnk 和 .url", SS_LEFT, IDC_STATUS);
            self->candidatesList = make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, IDC_CANDIDATES);
            ListView_SetExtendedListViewStyle(self->candidatesList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.pszText = const_cast<wchar_t*>(L"可用图标"); column.cx = 650;
            ListView_InsertColumn(self->candidatesList, 0, &column);
            self->imageList = ImageList_Create(self->Scale(32), self->Scale(32), ILC_COLOR32 | ILC_MASK, 8, 8);
            ListView_SetImageList(self->candidatesList, self->imageList, LVSIL_SMALL);
            self->splitter = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOTIFY,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SPLITTER)), GetModuleHandleW(nullptr), nullptr);
            SetWindowLongPtrW(self->splitter, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            SetWindowLongPtrW(self->splitter, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&App::SplitterProc));
            self->repairButton = make(L"BUTTON", L"修复所选图标", BS_DEFPUSHBUTTON | BS_CENTER | WS_TABSTOP, IDC_REPAIR);
            EnableWindow(self->repairButton, FALSE);
            self->refreshDesktop = make(L"BUTTON", L"刷新桌面", BS_PUSHBUTTON | BS_CENTER | WS_TABSTOP, IDC_REFRESH_DESKTOP);
            self->restartExplorer = make(L"BUTTON", L"重启资源管理器", BS_PUSHBUTTON | BS_CENTER | WS_TABSTOP, IDC_RESTART_EXPLORER);
            make(L"BUTTON", L"帮助", BS_PUSHBUTTON, ID_HELP);
            make(L"STATIC", L"操作日志", SS_LEFT, IDC_LOG);
            self->logEdit = make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, IDC_LOG_EDIT);
            DragAcceptFiles(hwnd, TRUE);
            self->Layout();
            self->Log(L"程序启动");
            return 0;
        }
        case WM_SIZE: self->Layout(); return 0;
        case WM_DPICHANGED: {
            self->dpi = HIWORD(wp);
            const auto suggested = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, self->backgroundBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC: {
            auto dc = reinterpret_cast<HDC>(wp);
            auto control = reinterpret_cast<HWND>(lp);
            SetBkMode(dc, TRANSPARENT);
            if (control == self->status) {
                SetTextColor(dc, RGB(38, 76, 115));
                return reinterpret_cast<INT_PTR>(self->panelBrush);
            }
            SetTextColor(dc, RGB(44, 50, 66));
            return reinterpret_cast<INT_PTR>(self->backgroundBrush);
        }
        case WM_CTLCOLOREDIT: {
            auto dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, RGB(35, 42, 52));
            SetBkColor(dc, RGB(250, 251, 253));
            return reinterpret_cast<INT_PTR>(self->editBrush);
        }
        case WM_DROPFILES: self->Drop(reinterpret_cast<HDROP>(wp)); return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
            case IDC_REPAIR: self->Repair(); return 0;
            case IDC_REFRESH_DESKTOP:
                if (ClearIconCache()) {
                    RefreshDesktop();
                    self->SetStatus(L"已发送图标缓存刷新请求，并刷新桌面。缓存效果可能因 Windows Explorer 状态而延迟生效。");
                    self->Log(L"已发送 Windows 图标缓存清理请求，并发送桌面刷新通知");
                } else {
                    RefreshDesktop();
                    self->SetStatus(L"已刷新桌面，但图标缓存清理请求未能启动。");
                    self->Log(L"图标缓存清理请求未能启动，已发送普通桌面刷新通知");
                }
                return 0;
            case IDC_RESTART_EXPLORER:
                self->SetStatus(L"正在重启资源管理器，桌面和任务栏会短暂消失。");
                self->Log(L"开始重启 Windows 资源管理器");
                if (RestartExplorer()) {
                    self->SetStatus(L"资源管理器已重启。");
                    self->Log(L"资源管理器重启完成");
                } else {
                    self->SetStatus(L"资源管理器重启失败。");
                    self->Log(L"资源管理器重启失败");
                }
                return 0;
            case ID_HELP:
                MessageBoxW(hwnd, L"使用方法：\n1. 将 Steam .url 或 .lnk 快捷方式拖入窗口。\n2. 在列表中选择图标。\n3. 点击“修复所选图标”。\n\n由于 Windows 图标缓存，修复后图标可能会延迟显示。若要一次性呈现之前所有修复结果，请点击窗口最右下角的“重启资源管理器”按钮。\n\n程序只修改快捷方式图标，不会修改游戏文件。\n日志：%TEMP%\\SteamIconFix.log", L"SteamIconFix 使用帮助", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            break;
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR*>(lp)->idFrom == IDC_CANDIDATES && reinterpret_cast<NMHDR*>(lp)->code == LVN_ITEMCHANGED) {
                const int selected = ListView_GetNextItem(self->candidatesList, -1, LVNI_SELECTED);
                EnableWindow(self->repairButton, selected >= 0 && static_cast<size_t>(selected) < self->icons.size());
            }
            break;
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize.x = self->Scale(760); info->ptMinTrackSize.y = self->Scale(500);
            return 0;
        }
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    App app;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = App::WindowProc;
    wc.hInstance = instance;
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(248, 249, 250));
    wc.lpszClassName = L"SteamIconFixWindow";
    if (!RegisterClassExW(&wc)) { CoUninitialize(); return 1; }
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"SteamIconFix", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 920, 720, nullptr, nullptr, instance, &app);
    if (!hwnd) { DeleteObject(wc.hbrBackground); CoUninitialize(); return 1; }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    DeleteObject(wc.hbrBackground);
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
