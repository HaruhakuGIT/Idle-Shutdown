#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "sound.h"
#include <windows.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <hidusage.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using Microsoft::WRL::ComPtr;

namespace {
constexpr int width = 760;
constexpr int height = 450;
constexpr float pi = std::numbers::pi_v<float>;
constexpr int minimumSeconds = 5;
constexpr int maximumSeconds = 120;
constexpr DWORD minimumIdleMinutes = 1;
constexpr DWORD maximumIdleMinutes = 240;
constexpr DWORD mouseMotionGap = 250;
constexpr int mouseMotionThreshold = 12;
constexpr float intro = 0.8f;
constexpr float collapseDuration = 0.66f;
constexpr float outro = 1.90f;
constexpr float centerX = width / 2.0f;
constexpr float centerY = 139.0f;
constexpr float displayScale = 1.55f;
constexpr float dimOpacity = 0.36f;
constexpr UINT_PTR frameTimer = 1;
constexpr UINT_PTR idleTimer = 2;
constexpr auto red = 0xbb0000;
constexpr auto hotRed = 0xbf0000;
constexpr auto lightText = 0xe0dbd8;
constexpr auto screenWash = 0x100503;
constexpr wchar_t overlayClass[] = L"ShutdownWarningOverlay";
constexpr wchar_t trayClass[] = L"ShutdownWarningTray";
constexpr wchar_t settingsClass[] = L"ShutdownWarningSettings";
constexpr wchar_t preferencesKey[] = L"Software\\ShutdownWarning";
constexpr wchar_t startupName[] = L"Shutdown Warning";
constexpr wchar_t startupRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t startupApprovalKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

struct Preferences {
    DWORD seconds = 12;
    DWORD sounds = 1;
    DWORD idleMinutes = 10;
    DWORD automatic = 1;
    DWORD forceShutdown = 0;
};

Preferences loadPreferences() {
    Preferences saved;
    DWORD size = sizeof(saved);
    if (RegGetValueW(HKEY_CURRENT_USER, preferencesKey, L"Preview", RRF_RT_REG_BINARY,
                     nullptr, &saved, &size) != ERROR_SUCCESS ||
        (size != sizeof(saved) && size != 4 * sizeof(DWORD) && size != 2 * sizeof(DWORD)) ||
        saved.seconds < minimumSeconds || saved.seconds > maximumSeconds || saved.sounds > 1 ||
        saved.idleMinutes < minimumIdleMinutes || saved.idleMinutes > maximumIdleMinutes ||
        saved.automatic > 1 || saved.forceShutdown > 1) {
        return {};
    }
    return saved;
}

bool savePreferences(const Preferences& choices) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, preferencesKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const auto* data = reinterpret_cast<const BYTE*>(&choices);
    const auto result = RegSetValueExW(key, L"Preview", 0, REG_BINARY, data, sizeof(choices));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

const std::wstring& executablePath() {
    static const std::wstring path = [] {
        std::wstring buffer(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        return length > 0 && length < buffer.size() ? buffer.substr(0, length) : std::wstring{};
    }();
    return path;
}

bool samePath(std::wstring_view left, std::wstring_view right) {
    return !left.empty() && !right.empty() &&
           CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool isMissingValue(LSTATUS status) {
    return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

struct StartupEntry {
    LSTATUS status{};
    std::wstring path;
    bool usesStandardArguments{};
};

StartupEntry readStartupEntry() {
    wchar_t command[MAX_PATH + 1]{};
    DWORD bytes = sizeof(command);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, startupRunKey, startupName,
                                        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, command, &bytes);
    if (status != ERROR_SUCCESS || !command[0]) {
        return {status, {}, false};
    }
    int count = 0;
    auto** arguments = CommandLineToArgvW(command, &count);
    if (!arguments) {
        return {static_cast<LSTATUS>(GetLastError()), {}, false};
    }
    StartupEntry result{status, count > 0 ? arguments[0] : L"",
                        count == 1 || (count == 2 && std::wstring_view(arguments[1]) == L"--autostart")};
    LocalFree(arguments);
    return result;
}

bool startsAtLogin() {
    if (!samePath(readStartupEntry().path, executablePath())) {
        return false;
    }
    // Task Manager can disable Run entries while leaving the launch command in place.
    BYTE approval[32]{};
    DWORD bytes = sizeof(approval);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, startupApprovalKey, startupName,
                                        RRF_RT_REG_BINARY, nullptr, approval, &bytes);
    if (isMissingValue(status)) {
        return true;
    }
    return status == ERROR_SUCCESS && bytes >= sizeof(DWORD) && (approval[0] == 2 || approval[0] == 6);
}

bool removeStartupValue(PCWSTR key) {
    const LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, key, startupName);
    return status == ERROR_SUCCESS || isMissingValue(status);
}

bool removeOwnedStartup() {
    const auto existing = readStartupEntry();
    if (existing.status != ERROR_SUCCESS) {
        return isMissingValue(existing.status);
    }
    return !samePath(existing.path, executablePath()) || removeStartupValue(startupRunKey);
}

bool writeStartupCommand() {
    const auto& path = executablePath();
    const std::wstring command = L"\"" + path + L"\" --autostart";
    if (path.empty() || command.size() >= MAX_PATH) {
        return false;
    }
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, startupRunKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS status = RegSetValueExW(key, startupName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool setStartsAtLogin(bool enabled) {
    if (!enabled) {
        return removeOwnedStartup();
    }
    // An explicit opt-in can re-enable an entry previously disabled in Task Manager.
    return writeStartupCommand() && (startsAtLogin() || removeStartupValue(startupApprovalKey));
}

bool migrateStartup(std::wstring_view previousPath) {
    const auto existing = readStartupEntry();
    if (existing.status != ERROR_SUCCESS) {
        return isMissingValue(existing.status);
    }
    const auto separator = previousPath.find_last_of(L"\\/");
    const auto fileName = separator == std::wstring_view::npos ? previousPath : previousPath.substr(separator + 1);
    if (!samePath(fileName, L"ShutdownWarning.exe") || !existing.usesStandardArguments ||
        !samePath(existing.path, previousPath)) {
        return true;
    }
    // Installer path migration preserves any Task Manager disable marker.
    return writeStartupCommand();
}

DWORD requestForcedPowerOff() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return GetLastError();
    }

    TOKEN_PRIVILEGES enable{};
    enable.PrivilegeCount = 1;
    enable.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &enable.Privileges[0].Luid)) {
        const DWORD error = GetLastError();
        CloseHandle(token);
        return error;
    }

    TOKEN_PRIVILEGES previous{};
    DWORD previousSize{};
    const BOOL adjusted = AdjustTokenPrivileges(token, FALSE, &enable, sizeof(previous), &previous, &previousSize);
    DWORD result = GetLastError();
    if (adjusted && result == ERROR_SUCCESS) {
        constexpr DWORD flags = SHUTDOWN_FORCE_SELF | SHUTDOWN_FORCE_OTHERS | SHUTDOWN_POWEROFF;
        constexpr DWORD reason = SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED;
        result = InitiateShutdownW(nullptr, nullptr, 0, flags, reason);
        if (result == ERROR_SHUTDOWN_IN_PROGRESS || result == ERROR_SHUTDOWN_IS_SCHEDULED) {
            result = ERROR_SUCCESS;
        } else if (result == ERROR_ACCESS_DENIED || result == ERROR_NOT_SUPPORTED ||
                   result == ERROR_INVALID_FUNCTION || result == ERROR_CALL_NOT_IMPLEMENTED ||
                   result == ERROR_INVALID_PARAMETER) {
            result = InitiateSystemShutdownExW(nullptr, nullptr, 0, TRUE, FALSE, reason)
                ? ERROR_SUCCESS : GetLastError();
            if (result == ERROR_SHUTDOWN_IN_PROGRESS) {
                result = ERROR_SUCCESS;
            }
        }
        if (previous.PrivilegeCount) {
            AdjustTokenPrivileges(token, FALSE, &previous, 0, nullptr, nullptr);
        }
    }
    CloseHandle(token);
    return result;
}

struct IdlePeriod {
    DWORD began{};
    bool handled{};
};

struct MouseMotion {
    HANDLE device{};
    DWORD lastMotion{};
    int64_t x{};
    int64_t y{};

    bool moved(HANDLE source, DWORD now, LONG dx, LONG dy) {
        if (!dx && !dy) {
            return false;
        }
        // Count displacement within a gesture. Opposite movements cancel each other out.
        if (device != source || now - lastMotion >= mouseMotionGap) {
            x = y = 0;
        }
        device = source;
        lastMotion = now;
        x += dx;
        y += dy;
        return std::abs(x) + std::abs(y) >= mouseMotionThreshold;
    }
};

float ease(float value) {
    const float t = std::clamp(value, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

class Overlay {
public:
    Overlay(DWORD seconds, bool forcePowerOff)
        : countdown(static_cast<float>(seconds)), powerOffAtZero(forcePowerOff) {}

    ~Overlay() {
        if (oldBitmap) {
            SelectObject(surface, oldBitmap);
        }
        if (bitmap) {
            DeleteObject(bitmap);
        }
        if (surface) {
            DeleteDC(surface);
        }
    }

    bool initialize(HWND window, HMONITOR monitor) {
        MONITORINFO info{ sizeof(info) };
        if (!GetMonitorInfoW(monitor, &info)) {
            return false;
        }
        scale = displayScale * GetDpiForWindow(window) / 96.0f;
        if (scale <= 0) {
            scale = 1;
        }
        const auto& bounds = info.rcMonitor;
        scale = std::min({ scale, (bounds.right - bounds.left) / static_cast<float>(width),
                                  (bounds.bottom - bounds.top) / static_cast<float>(height) });
        size = { bounds.right - bounds.left, bounds.bottom - bounds.top };
        position = { bounds.left, bounds.top };
        const float offsetX = (size.cx - width * scale) / 2;
        const float offsetY = (size.cy - height * scale) / 2 - 12 * scale;
        artworkTransform = D2D1::Matrix3x2F::Scale(scale, scale) *
                           D2D1::Matrix3x2F::Translation(offsetX, offsetY);

        surface = CreateCompatibleDC(nullptr);
        if (!surface) {
            return false;
        }
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = size.cx;
        bitmapInfo.bmiHeader.biHeight = -size.cy;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        bitmap = CreateDIBSection(surface, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap) {
            return false;
        }
        oldBitmap = SelectObject(surface, bitmap);

        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(textFactory.GetAddressOf())))) {
            return false;
        }
        const auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        const RECT area{ 0, 0, size.cx, size.cy };
        if (FAILED(factory->CreateDCRenderTarget(&properties, target.GetAddressOf())) ||
            FAILED(target->BindDC(surface, &area)) ||
            FAILED(target->CreateSolidColorBrush(D2D1::ColorF(red), brush.GetAddressOf())) ||
            !makeBackground(offsetX, offsetY) ||
            !makeFormat(heading, L"Segoe UI", DWRITE_FONT_WEIGHT_BLACK, 70) ||
            !makeFormat(mono, L"Consolas", DWRITE_FONT_WEIGHT_BOLD, 16) ||
            !makeFormat(digits, L"Consolas", DWRITE_FONT_WEIGHT_BOLD, 62)) {
            return false;
        }
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        return true;
    }

    bool frame(HWND window, float time) {
        const float remaining = std::max(0.0f, countdown - std::max(0.0f, time - intro));
        const float finishedFor = std::max(0.0f, time - intro - countdown);
        const float collapse = ease(finishedFor / collapseDuration);
        const float sceneOpacity = 1 - collapse;
        const float fade = ease((finishedFor - 1.30f) / (outro - 1.30f));
        const float black = powerOffAtZero ? fade : 0;
        const float appear = ease(time / intro);
        const int seconds = static_cast<int>(std::ceil(remaining));

        target->BeginDraw();
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        target->Clear(D2D1::ColorF(0, black));
        target->DrawBitmap(shadedDesktop.Get(), nullptr, appear * (1 - black),
                           D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        target->SetTransform(artworkTransform);
        drawBackground(time, appear * sceneOpacity, remaining);
        drawOrbits(time, ease(time / 0.48f), remaining, collapse);
        drawSignals(time, ease((time - 0.15f) / 0.58f) * sceneOpacity, remaining);
        if (seconds > 0 && seconds <= 3) {
            drawBeat(seconds - remaining);
        }
        const float dialScale = 1 - 0.78f * collapse;
        const auto pivot = D2D1::Point2F(centerX, centerY);
        target->SetTransform(D2D1::Matrix3x2F::Scale(dialScale, dialScale, pivot) * artworkTransform);
        drawDial(time, appear * sceneOpacity, remaining);
        drawIntroScan(time);
        target->SetTransform(artworkTransform);
        drawText(seconds, remaining, ease((time - 0.22f) / 0.52f) * sceneOpacity, collapse);
        if (finishedFor > 0) {
            drawShutdownAnimation(finishedFor, collapse);
        }
        return present(window, powerOffAtZero ? 1 : 1 - fade);
    }

    bool blank(HWND window) {
        target->BeginDraw();
        target->Clear(D2D1::ColorF(0, 1.0f));
        return present(window, 1);
    }

private:
    bool present(HWND window, float opacity) {
        if (FAILED(target->EndDraw())) {
            return false;
        }
        POINT source{};
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, static_cast<BYTE>(255 * opacity), AC_SRC_ALPHA };
        return UpdateLayeredWindow(window, nullptr, &position, &size, surface, &source, 0,
                                   &blend, ULW_ALPHA) != FALSE;
    }

    bool makeBackground(float offsetX, float offsetY) {
        const D2D1_GRADIENT_STOP gridColors[] = {
            {0.00f, D2D1::ColorF(red, 0.20f)},
            {0.55f, D2D1::ColorF(red, 0.13f)},
            {1.00f, D2D1::ColorF(red, 0.00f)},
        };
        return makeShadedDesktop(offsetX, offsetY) &&
               makeRadialBrush(gridColors, ARRAYSIZE(gridColors), 414, 250, gridBrush) &&
               makeGrid();
    }

    bool makeShadedDesktop(float offsetX, float offsetY) {
        const auto base = D2D1::ColorF(screenWash, dimOpacity);
        const auto centerTone = D2D1::ColorF(0x150704);
        const auto outerTone = D2D1::ColorF(0x1c0704);
        const float originX = offsetX + centerX * scale;
        const float originY = offsetY + 207 * scale;
        const float inverseRadiusX = 1 / (465 * scale);
        const float inverseRadiusY = 1 / (278 * scale);
        std::vector<uint32_t> pixels(static_cast<size_t>(size.cx) * size.cy);
        for (LONG y = 0; y < size.cy; ++y) {
            const float dy = (y + 0.5f - originY) * inverseRadiusY;
            for (LONG x = 0; x < size.cx; ++x) {
                const float dx = (x + 0.5f - originX) * inverseRadiusX;
                const float distanceSquared = dx * dx + dy * dy;
                const float radius = std::sqrt(std::min(distanceSquared, 1.0f));
                const float shade = 0.86f * (1 - ease(radius));
                const float baseWeight = dimOpacity * (1 - shade);
                const auto mix = [radius](float from, float to) { return std::lerp(from, to, radius); };
                // Fixed subpixel noise breaks up visible 8-bit steps without shimmering between frames.
                const uint32_t seed = (static_cast<uint32_t>(x) * 0x9e3779b1u) ^
                                      (static_cast<uint32_t>(y) * 0x85ebca77u);
                const uint32_t hash = (seed ^ (seed >> 15)) * 0xc2b2ae3du;
                const float threshold = distanceSquared < 1 ? (hash >> 24) / 256.0f : 0.5f;
                const auto byte = [threshold](float value) {
                    return static_cast<uint32_t>(value * 255 + threshold);
                };
                const uint32_t a = byte(shade + baseWeight);
                const uint32_t r = byte(shade * mix(centerTone.r, outerTone.r) + baseWeight * base.r);
                const uint32_t g = byte(shade * mix(centerTone.g, outerTone.g) + baseWeight * base.g);
                const uint32_t b = byte(shade * mix(centerTone.b, outerTone.b) + baseWeight * base.b);
                pixels[static_cast<size_t>(y) * size.cx + x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
        const auto dimensions = D2D1::SizeU(size.cx, size.cy);
        const auto properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        return SUCCEEDED(target->CreateBitmap(dimensions, pixels.data(), size.cx * sizeof(uint32_t),
                                              properties, shadedDesktop.GetAddressOf()));
    }

    bool makeRadialBrush(const D2D1_GRADIENT_STOP* colors, UINT32 count, float radiusX, float radiusY,
                         ComPtr<ID2D1RadialGradientBrush>& gradient) {
        ComPtr<ID2D1GradientStopCollection> stops;
        if (FAILED(target->CreateGradientStopCollection(colors, count, stops.GetAddressOf()))) {
            return false;
        }

        const auto glow = D2D1::RadialGradientBrushProperties(
            D2D1::Point2F(centerX, 207), D2D1::Point2F(), radiusX, radiusY);
        return SUCCEEDED(target->CreateRadialGradientBrush(glow, stops.Get(), gradient.GetAddressOf()));
    }

    bool makeGrid() {
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(factory->CreatePathGeometry(grid.GetAddressOf())) || FAILED(grid->Open(sink.GetAddressOf()))) {
            return false;
        }

        constexpr float side = 17;
        constexpr float rowHeight = 29.44486f;  // Hexagon height, side * sqrt(3).
        for (int column = -17; column <= 17; ++column) {
            const float x = centerX + column * side * 1.5f;
            const float stagger = column % 2 != 0 ? rowHeight / 2 : 0;
            for (int row = -9; row <= 9; ++row) {
                const float y = 207 + row * rowHeight + stagger;
                const D2D1_POINT_2F edges[] = {
                    D2D1::Point2F(x + side / 2, y + rowHeight / 2),
                    D2D1::Point2F(x - side / 2, y + rowHeight / 2),
                    D2D1::Point2F(x - side, y),
                };
                // Adjacent cells supply the other three edges.
                sink->BeginFigure(D2D1::Point2F(x + side, y), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLines(edges, ARRAYSIZE(edges));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
            }
        }
        return SUCCEEDED(sink->Close());
    }

    void drawBackground(float time, float appear, float remaining) {
        const float urgency = 1 - ease(remaining / 4.0f);
        gridBrush->SetOpacity(appear);
        target->DrawGeometry(grid.Get(), gridBrush.Get(), 0.9f);

        // Faint outgoing rings give the alarm motion outside the countdown dial.
        const auto center = D2D1::Point2F(centerX, centerY);
        for (const float offset : {0.0f, 0.5f}) {
            const float phase = std::fmod(time * 0.64f + offset, 1.0f);
            const float radius = 91 + phase * 124;
            const float envelope = ease(phase / 0.14f) * (1 - ease(phase));
            const float opacity = appear * envelope * (0.28f + 0.12f * urgency);
            const auto wave = D2D1::Ellipse(center, radius, radius);
            target->DrawEllipse(wave, color(red, 0.34f * opacity), 9);
            target->DrawEllipse(wave, color(red, opacity), 1.4f);
        }
    }

    void drawOrbits(float time, float reveal, float remaining, float collapse) {
        const float urgentTime = std::max(0.0f, 4 - remaining);
        const auto center = D2D1::Point2F(centerX, centerY);
        const float radius = (85 + 46 * reveal) * (1 - 0.88f * collapse);
        const float orbitHeight = 36 * (1 - 0.88f * collapse);
        const float opacity = reveal * (1 - collapse);
        for (const int direction : {-1, 1}) {
            const float tilt = direction * (31 - 14 * (1 - reveal) + 22 * collapse);
            const float phase = direction * (time * 1.8f + urgentTime * urgentTime * 0.15f + 2.6f * collapse) +
                                (direction < 0 ? pi : 0.0f);
            const auto rotation = D2D1::Matrix3x2F::Rotation(tilt, center);
            target->SetTransform(rotation * artworkTransform);
            target->DrawEllipse(D2D1::Ellipse(center, radius, orbitHeight), color(red, 0.34f * opacity), 1.1f);
            for (int index = 5; index >= 0; --index) {
                const float angle = phase - direction * index * 0.095f;
                const auto point = D2D1::Point2F(centerX + radius * std::cos(angle),
                                                 centerY + orbitHeight * std::sin(angle));
                const float intensity = 1 - index / 6.0f;
                const float dotRadius = 0.8f + 2.8f * intensity;
                if (index == 0) {
                    target->FillEllipse(D2D1::Ellipse(point, 7.2f, 7.2f), color(red, 0.24f * opacity));
                }
                target->FillEllipse(D2D1::Ellipse(point, dotRadius, dotRadius),
                                    color(hotRed, intensity * opacity));
            }
        }
        target->SetTransform(artworkTransform);
    }

    void drawIntroScan(float time) {
        const float progress = std::clamp((time - 0.12f) / 0.67f, 0.0f, 1.0f);
        const float opacity = std::sin(pi * progress);
        if (opacity <= 0.01f) {
            return;
        }

        constexpr float scanRadius = 70;
        const float dy = std::lerp(-scanRadius, scanRadius, ease(progress));
        const float halfWidth = std::sqrt(scanRadius * scanRadius - dy * dy);
        const auto left = D2D1::Point2F(centerX - halfWidth, centerY + dy);
        const auto right = D2D1::Point2F(centerX + halfWidth, centerY + dy);
        target->DrawLine(left, right, color(red, 0.22f * opacity), 9);
        target->DrawLine(left, right, color(hotRed, 0.94f * opacity), 1.6f);
        target->FillEllipse(D2D1::Ellipse(left, 2.5f, 2.5f), color(hotRed, opacity));
        target->FillEllipse(D2D1::Ellipse(right, 2.5f, 2.5f), color(hotRed, opacity));
    }

    void drawBeat(float sinceTick) {
        const float progress = ease(sinceTick / 0.48f);
        const float opacity = 1 - progress;
        if (opacity <= 0.01f) {
            return;
        }
        const auto center = D2D1::Point2F(centerX, centerY);
        const float radius = 80 + 82 * progress;
        const auto ripple = D2D1::Ellipse(center, radius, radius);
        target->DrawEllipse(ripple, color(red, 0.32f * opacity), 10);
        target->DrawEllipse(ripple, color(hotRed, 0.85f * opacity), 2.4f);
    }

    void drawShutdownAnimation(float age, float collapse) {
        const float blackout = 0.94f * ease((age - 0.36f) / 0.64f);
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        target->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(size.cx), static_cast<float>(size.cy)),
                              color(0x080102, blackout));
        target->SetTransform(artworkTransform);

        const float sweep = ease((age - 0.54f) / 0.29f);
        const float close = ease((age - 0.88f) / 0.35f);
        const float y = std::lerp(centerY, height / 2.0f + 12, sweep);
        const float coreOpacity = ease(age / 0.21f) * (1 - close);
        const auto center = D2D1::Point2F(centerX, y);
        const float coreRadius = std::lerp(25.0f, 4.5f, collapse) * (1 - 0.70f * close);
        const auto glow = D2D1::Ellipse(center, coreRadius * 2.4f, coreRadius * 2.4f);
        target->FillEllipse(glow, color(red, 0.13f * coreOpacity));
        target->DrawEllipse(D2D1::Ellipse(center, coreRadius, coreRadius),
                            color(red, 0.76f * coreOpacity), 1.4f);
        target->FillEllipse(D2D1::Ellipse(center, 3.2f, 3.2f), color(lightText, 0.82f * coreOpacity));

        const float beam = sweep * (1 - close);
        const float halfWidth = 296 * beam;
        const auto left = D2D1::Point2F(centerX - halfWidth, y);
        const auto right = D2D1::Point2F(centerX + halfWidth, y);
        target->DrawLine(left, right, color(red, 0.21f * beam), 13.0f);
        target->DrawLine(left, right, color(hotRed, beam), 2.2f);
        for (const float direction : {-1.0f, 1.0f}) {
            const float x = centerX + direction * halfWidth;
            const float tail = x - direction * 22 * beam;
            target->DrawLine(D2D1::Point2F(tail, y - 7 * beam), D2D1::Point2F(x, y),
                             color(hotRed, 0.67f * beam), 1.2f);
            target->DrawLine(D2D1::Point2F(tail, y + 7 * beam), D2D1::Point2F(x, y),
                             color(hotRed, 0.67f * beam), 1.2f);
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 2.5f, 2.5f),
                                color(lightText, 0.66f * beam));
        }
    }

    void drawSignals(float time, float reveal, float remaining) {
        const float urgentTime = std::max(0.0f, 4 - remaining);
        const float phase = time * 5.2f + urgentTime * urgentTime * 0.65f;
        const float trackLength = 112 * reveal;
        constexpr int samples = 42;
        for (const int side : {-1, 1}) {
            const float x = centerX + side * 151;
            const float y = centerY + side * 8;
            const float edge = x + side * trackLength;
            auto* rail = color(red, 0.43f * reveal);
            target->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(edge, y), rail, 0.8f);
            target->DrawLine(D2D1::Point2F(x, y - 13), D2D1::Point2F(x, y + 13), rail, 1.1f);
            target->DrawLine(D2D1::Point2F(edge, y - 8), D2D1::Point2F(edge, y + 8), rail, 1.1f);

            auto* trace = color(red, 0.76f * reveal);
            D2D1_POINT_2F previous = D2D1::Point2F(x, y);
            for (int index = 1; index <= samples; ++index) {
                const float step = index / static_cast<float>(samples);
                const float envelope = std::sin(pi * step);
                const float carrier = std::sin(phase * side - 19 * step);
                const float ripple = 0.30f * std::sin(phase * 1.55f - 42 * step);
                const auto point = D2D1::Point2F(x + side * trackLength * step,
                                                 y + reveal * envelope * (carrier + ripple) * 14);
                target->DrawLine(previous, point, trace, 1.4f);
                previous = point;
            }

            for (int index = 1; index <= 5; ++index) {
                const float marker = x + side * trackLength * (index / 6.0f);
                target->DrawLine(D2D1::Point2F(marker, y + 20), D2D1::Point2F(marker, y + 24),
                                 color(red, 0.46f * reveal), 1.0f);
            }
        }
    }

    bool makeFormat(ComPtr<IDWriteTextFormat>& format, const wchar_t* family,
                    DWRITE_FONT_WEIGHT weight, float fontSize) {
        if (FAILED(textFactory->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL, fontSize, L"en-US",
                                                format.GetAddressOf()))) {
            return false;
        }
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    }

    ID2D1SolidColorBrush* color(unsigned rgb, float alpha) {
        brush->SetColor(D2D1::ColorF(rgb, std::clamp(alpha, 0.0f, 1.0f)));
        return brush.Get();
    }

    static D2D1_POINT_2F radial(float radius, float angle) {
        return D2D1::Point2F(centerX + radius * std::cos(angle), centerY + radius * std::sin(angle));
    }

    void arc(float radius, float startAngle, float sweep, float thickness, float alpha) {
        if (sweep < 0.001f || alpha <= 0) {
            return;
        }
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(factory->CreatePathGeometry(path.GetAddressOf())) ||
            FAILED(path->Open(sink.GetAddressOf()))) {
            return;
        }
        const auto arcSize = D2D1::SizeF(radius, radius);
        sink->BeginFigure(radial(radius, startAngle), D2D1_FIGURE_BEGIN_HOLLOW);
        // Split the stroke so a full revolution still has distinct arc endpoints.
        for (const float portion : {0.5f, 1.0f}) {
            sink->AddArc(D2D1::ArcSegment(radial(radius, startAngle + sweep * portion),
                                         arcSize, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                         D2D1_ARC_SIZE_SMALL));
        }
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        if (SUCCEEDED(sink->Close())) {
            target->DrawGeometry(path.Get(), color(red, alpha), thickness);
        }
    }

    void drawDial(float time, float appear, float remaining) {
        const float radius = 70 + 7 * appear;
        const float urgency = 1 - ease(remaining / 3.0f);
        const float urgentTime = std::max(0.0f, time - intro - countdown + 3);
        const float pulsePhase = time * 0.6f + urgentTime * urgentTime * 0.19f;
        const float pulse = 0.5f + 0.5f * std::sin(2 * pi * pulsePhase);
        const float glow = appear * (0.55f + pulse * (0.22f + 0.23f * urgency));
        const auto circle = D2D1::Ellipse(D2D1::Point2F(centerX, centerY), radius, radius);
        target->DrawEllipse(circle, color(red, 0.20f * glow), 19);
        target->DrawEllipse(circle, color(red, 0.45f * glow), 10);
        target->DrawEllipse(circle, color(red, 0.49f * appear), 1.5f);

        const float startAngle = -pi / 2;
        const float fraction = remaining / countdown;
        const float sweep = 2 * pi * fraction * appear;
        arc(radius, startAngle, sweep, 5.4f, appear * (0.89f + 0.11f * pulse));
        const auto inner = D2D1::Ellipse(circle.point, radius - 16, radius - 16);
        target->DrawEllipse(inner, color(red, 0.27f * appear), 1);

        for (int tickIndex = 0; tickIndex < 48; ++tickIndex) {
            const float angle = startAngle + tickIndex * (pi / 24);
            const bool major = tickIndex % 4 == 0;
            const float reveal = ease(1.5f * appear - tickIndex / 90.0f);
            target->DrawLine(radial(radius + 8, angle), radial(radius + (major ? 16 : 12), angle),
                             color(red, (major ? 0.68f : 0.30f) * reveal), major ? 1.6f : 1.0f);
            if (major) {
                target->DrawLine(radial(radius - 16, angle), radial(radius - 11, angle),
                                 color(red, 0.48f * reveal), 1.5f);
            }
        }

        const auto marker = radial(radius, startAngle + sweep);
        target->FillEllipse(D2D1::Ellipse(marker, 3, 3), color(hotRed, appear * ease(remaining)));
        const float rotation = startAngle + 0.68f * time + 0.28f * urgentTime * urgentTime;
        arc(radius + 22, rotation, 0.68f, 2.8f, glow);
        arc(radius + 22, rotation + pi, 0.68f, 2.8f, glow);

        const auto tip = D2D1::Point2F(centerX, centerY - 40);
        const auto left = D2D1::Point2F(centerX - 45, centerY + 34);
        const auto right = D2D1::Point2F(centerX + 45, centerY + 34);
        for (const float stroke : {11.0f, 4.2f}) {
            auto* edge = color(red, stroke > 5 ? 0.60f * glow : appear);
            target->DrawLine(tip, left, edge, stroke);
            target->DrawLine(left, right, edge, stroke);
            target->DrawLine(right, tip, edge, stroke);
        }

        auto* mark = color(hotRed, appear);
        target->FillRectangle(D2D1::RectF(centerX - 3.0f, centerY - 9,
                                         centerX + 3.0f, centerY + 12), mark);
        target->FillRectangle(D2D1::RectF(centerX - 3.0f, centerY + 17,
                                         centerX + 3.0f, centerY + 23), mark);
    }

    template <size_t Length>
    void label(const wchar_t (&value)[Length], IDWriteTextFormat* format, float y, float boxHeight,
               unsigned rgb, float alpha) {
        target->DrawText(value, static_cast<UINT32>(Length - 1), format,
                         D2D1::RectF(0, y, width, y + boxHeight), color(rgb, alpha));
    }

    void drawText(int seconds, float remaining, float reveal, float collapse) {
        const float rise = 9 * (1 - reveal) - 18 * collapse;
        const float accentY = 259 + rise;
        for (const int side : {-1, 1}) {
            for (int index = 0; index < 3; ++index) {
                const float x = centerX + side * (196 + 11 * index + 14 * (1 - reveal));
                auto* accent = color(red, (0.85f - 0.21f * index) * reveal);
                const auto tip = D2D1::Point2F(x, accentY);
                target->DrawLine(D2D1::Point2F(x + side * 8, accentY - 10), tip, accent, 2.2f);
                target->DrawLine(tip, D2D1::Point2F(x + side * 8, accentY + 10), accent, 2.2f);
            }
        }
        label(L"WARNING", heading.Get(), 224 + rise, 67, red, reveal);
        label(L"SHUTDOWN IMMINENT", mono.Get(), 295 + rise, 19, lightText, reveal);

        const int minutes = seconds / 60;
        const wchar_t clock[] = { L'0', static_cast<wchar_t>(L'0' + minutes), L':',
                                  static_cast<wchar_t>(L'0' + seconds % 60 / 10),
                                  static_cast<wchar_t>(L'0' + seconds % 10), 0 };
        label(clock, digits.Get(), 320, 63, seconds <= 3 ? hotRed : lightText, reveal);

        constexpr int segmentCount = 12;
        constexpr float segmentWidth = 11;
        constexpr float segmentStep = 15;
        const float left = centerX - ((segmentCount - 1) * segmentStep + segmentWidth) / 2;
        const float segmentsLeft = remaining * segmentCount / countdown;
        for (int index = 0; index < segmentCount; ++index) {
            const float x = left + index * segmentStep;
            const float fill = std::clamp(segmentsLeft - index, 0.0f, 1.0f);
            const float y = 388 + rise;
            target->FillRectangle(D2D1::RectF(x, y, x + segmentWidth, y + 3), color(red, reveal * 0.28f));
            target->FillRectangle(D2D1::RectF(x, y, x + segmentWidth * fill, y + 3), color(red, reveal));
        }
        if (seconds == 0 && powerOffAtZero) {
            label(L"POWERING OFF", mono.Get(), 405 + rise, 20, lightText, reveal * 0.88f);
        } else {
            label(L"ESC TO CANCEL", mono.Get(), 405 + rise, 20, lightText, reveal * 0.88f);
        }
    }

    HDC surface{};
    HBITMAP bitmap{};
    HGDIOBJ oldBitmap{};
    SIZE size{};
    POINT position{};
    float scale = 1;
    const float countdown;
    const bool powerOffAtZero;
    D2D1_MATRIX_3X2_F artworkTransform{};
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1DCRenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1Bitmap> shadedDesktop;
    ComPtr<ID2D1RadialGradientBrush> gridBrush;
    ComPtr<ID2D1PathGeometry> grid;
    ComPtr<IDWriteFactory> textFactory;
    ComPtr<IDWriteTextFormat> heading;
    ComPtr<IDWriteTextFormat> mono;
    ComPtr<IDWriteTextFormat> digits;
};

constexpr UINT trayCallback = WM_APP + 1;
constexpr UINT showSettingsMessage = WM_APP + 2;
constexpr UINT trayIconId = 1;
enum ControlId { countdownField = 100, soundField, previewButton, exitItem, idleField, automaticField, forceField, startupField };
constexpr int settingsHeading = 200;
constexpr int settingsWidth = 420;
constexpr int settingsHeight = 438;
constexpr COLORREF panelBackground = RGB(19, 9, 10);
constexpr COLORREF panelField = RGB(37, 17, 19);
constexpr COLORREF panelText = RGB(231, 221, 218);
constexpr COLORREF panelMuted = RGB(178, 151, 152);
constexpr COLORREF panelRed = RGB(187, 0, 0);
constexpr COLORREF panelBorder = RGB(105, 36, 41);
constexpr COLORREF panelKeyboardFocus = RGB(218, 121, 125);

struct Control {
    PCWSTR type;
    PCWSTR text;
    DWORD style;
    int id;
    int x, y, width, height;
};

constexpr Control controls[] = {
    {L"STATIC", L"Automatic warning", 0, settingsHeading, 22, 20, 300, 30},
    {L"STATIC", L"Press a key, click, or move the mouse to cancel.", 0, 201, 22, 55, 300, 21},
    {L"BUTTON", L"Start after inactivity", WS_TABSTOP | BS_AUTOCHECKBOX, automaticField, 22, 90, 300, 28},
    {L"STATIC", L"Inactivity (1-240 minutes)", 0, 202, 22, 135, 275, 22},
    {L"EDIT", L"", WS_TABSTOP | ES_NUMBER | ES_CENTER, idleField, 321, 128, 76, 27},
    {L"STATIC", L"Countdown (5-120 seconds)", 0, 203, 22, 189, 275, 22},
    {L"EDIT", L"", WS_TABSTOP | ES_NUMBER | ES_CENTER, countdownField, 321, 182, 76, 27},
    {L"BUTTON", L"Play warning sounds", WS_TABSTOP | BS_AUTOCHECKBOX, soundField, 22, 224, 300, 28},
    {L"BUTTON", L"Force shutdown after countdown", WS_TABSTOP | BS_AUTOCHECKBOX, forceField, 22, 263, 365, 28},
    {L"STATIC", L"Windows closes apps automatically.\r\nUnsaved work can be lost.", 0, 204, 22, 290, 375, 34},
    {L"BUTTON", L"Start at login", WS_TABSTOP | BS_AUTOCHECKBOX, startupField, 22, 334, 365, 28},
    {L"BUTTON", L"Preview", WS_TABSTOP | BS_PUSHBUTTON, previewButton, 22, 386, 101, 32},
    {L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL, 216, 386, 84, 32},
    {L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 313, 386, 84, 32},
};

int pixels(int dips, UINT dpi) {
    return MulDiv(dips, dpi, 96);
}

void fill(HDC dc, const RECT& area, COLORREF rgb) {
    SetDCBrushColor(dc, rgb);
    FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void paintButton(HWND button, HDC dc) {
    RECT area{};
    GetClientRect(button, &area);
    const UINT dpi = GetDpiForWindow(button);
    const auto style = GetWindowLongPtrW(button, GWL_STYLE) & BS_TYPEMASK;
    const auto state = SendMessageW(button, BM_GETSTATE, 0, 0);
    const bool keyboardFocus = GetFocus() == button &&
                               !(SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
    const bool down = (state & BST_PUSHED) != 0;
    const int id = GetDlgCtrlID(button);
    wchar_t title[64]{};
    GetWindowTextW(button, title, ARRAYSIZE(title));
    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0));
    const auto oldFont = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, panelText);

    if (style == BS_AUTOCHECKBOX) {
        fill(dc, area, panelBackground);
        const bool enabled = (state & BST_CHECKED) != 0;
        const int trackWidth = pixels(34, dpi);
        const int trackHeight = pixels(16, dpi);
        const int top = (area.bottom - trackHeight) / 2;
        RECT track{0, top, trackWidth, top + trackHeight};
        const COLORREF trackBorder = keyboardFocus ? panelKeyboardFocus : (enabled ? panelRed : panelBorder);
        fill(dc, track, trackBorder);
        InflateRect(&track, -pixels(1, dpi), -pixels(1, dpi));
        fill(dc, track, enabled ? RGB(105, 9, 15) : panelField);
        const int thumb = pixels(8, dpi);
        const int inset = pixels(4, dpi);
        const int thumbX = enabled ? trackWidth - thumb - inset : inset;
        const int thumbY = top + (trackHeight - thumb) / 2;
        fill(dc, RECT{thumbX, thumbY, thumbX + thumb, thumbY + thumb},
             enabled ? panelText : panelMuted);
        area.left = trackWidth + pixels(12, dpi);
        DrawTextW(dc, title, -1, &area, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    } else {
        const COLORREF buttonBorder = keyboardFocus ? panelKeyboardFocus : (id == IDOK ? panelRed : panelBorder);
        fill(dc, area, buttonBorder);
        InflateRect(&area, -pixels(1, dpi), -pixels(1, dpi));
        const COLORREF surface = id == IDOK ? RGB(103, 10, 18) : panelField;
        fill(dc, area, down ? RGB(78, 23, 26) : surface);
        DrawTextW(dc, title, -1, &area, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    SelectObject(dc, oldFont);
}

LRESULT CALLBACK styledButton(HWND button, UINT message, WPARAM wParam, LPARAM lParam,
                              UINT_PTR subclassId, DWORD_PTR) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(button, &paint);
        paintButton(button, dc);
        EndPaint(button, &paint);
        return 0;
    }
    if (message == WM_PRINTCLIENT) {
        paintButton(button, reinterpret_cast<HDC>(wParam));
        return 0;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message == WM_LBUTTONDOWN) {
        SendMessageW(GetParent(button), WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(button, styledButton, subclassId);
    }
    const auto result = DefSubclassProc(button, message, wParam, lParam);
    switch (message) {
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case BM_SETSTYLE:
    case BM_SETCHECK:
    case BM_SETSTATE:
    case WM_ENABLE:
    case WM_UPDATEUISTATE:
        InvalidateRect(button, nullptr, FALSE);
        break;
    }
    return result;
}

bool registerWindow(HINSTANCE instance, PCWSTR name, WNDPROC procedure, HBRUSH background = nullptr) {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = procedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = name;
    windowClass.hbrBackground = background;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    return RegisterClassW(&windowClass) != 0;
}

HICON makeTrayIcon() {
    constexpr int iconSize = 32;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = iconSize;
    info.bmiHeader.biHeight = -iconSize;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    const HBITMAP image = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!image) {
        return nullptr;
    }

    auto* pixels = static_cast<uint32_t*>(bits);
    for (int y = 0; y < iconSize; ++y) {
        for (int x = 0; x < iconSize; ++x) {
            const int distance = std::abs(2 * x - (iconSize - 1));
            const bool inside = y >= 3 && y <= 26 && distance <= y + 2;
            const bool mark = x >= 14 && x <= 17 && ((y >= 11 && y <= 18) || (y >= 21 && y <= 23));
            pixels[y * iconSize + x] = inside ? 0xff000000u | (mark ? lightText : red) : 0;
        }
    }

    const BYTE maskBits[iconSize * iconSize / 8]{};
    const HBITMAP mask = CreateBitmap(iconSize, iconSize, 1, 1, maskBits);
    ICONINFO iconInfo{TRUE, 0, 0, mask, image};
    const HICON icon = mask ? CreateIconIndirect(&iconInfo) : nullptr;
    if (mask) {
        DeleteObject(mask);
    }
    DeleteObject(image);
    return icon;
}

LRESULT CALLBACK overlayProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    // The tray owns the preview and closes every overlay together.
    if (message == WM_CLOSE) {
        SendMessageW(GetWindow(window, GW_OWNER), WM_COMMAND, IDCANCEL, 0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

struct PreviewView {
    HWND window;
    std::unique_ptr<Overlay> renderer;
};

class TrayApp {
public:
    explicit TrayApp(HINSTANCE instance) : instance(instance) {}

    int run(bool showSettings) {
        if (!registerWindow(instance, trayClass, trayProc) ||
            !registerWindow(instance, settingsClass, settingsProc) ||
            !registerWindow(instance, overlayClass, overlayProc)) {
            return 1;
        }
        taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        window = CreateWindowExW(0, trayClass, L"Idle Shutdown", WS_OVERLAPPED,
                                 0, 0, 0, 0, nullptr, nullptr, instance, this);
        if (!window) {
            return 1;
        }

        icon = makeTrayIcon();
        registerTrayIcon();
        const RAWINPUTDEVICE devices[] = {
            {HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_MOUSE, RIDEV_INPUTSINK, window},
            {HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_KEYBOARD, RIDEV_INPUTSINK, window},
        };
        rawInputRegistered = RegisterRawInputDevices(devices, ARRAYSIZE(devices), sizeof(RAWINPUTDEVICE));
        resetIdleClock();
        if (!SetTimer(window, idleTimer, 500, nullptr)) {
            MessageBoxW(nullptr, L"Could not start inactivity monitoring.", L"Idle Shutdown", MB_OK | MB_ICONERROR);
            DestroyWindow(window);
            return 1;
        }
        if (showSettings) {
            openSettings();
        }

        MSG message{};
        BOOL status;
        while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (!views.empty() && message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
                cancelPreview();
                continue;
            }
            if (settingsWindow && message.message == WM_KEYDOWN && message.wParam == VK_TAB &&
                (message.hwnd == settingsWindow || IsChild(settingsWindow, message.hwnd))) {
                SendMessageW(settingsWindow, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
            }
            if (!settingsWindow || !IsDialogMessageW(settingsWindow, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return status == -1 ? 1 : 0;
    }

private:
    static TrayApp* instanceFor(HWND window, UINT message, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        }
        return reinterpret_cast<TrayApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    static LRESULT CALLBACK trayProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* app = instanceFor(window, message, lParam);
        return app ? app->handleTray(window, message, wParam, lParam)
                   : DefWindowProcW(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK settingsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* app = instanceFor(window, message, lParam);
        return app ? app->handleSettings(window, message, wParam, lParam)
                   : DefWindowProcW(window, message, wParam, lParam);
    }

    NOTIFYICONDATAW notification() const {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window;
        data.uID = trayIconId;
        return data;
    }

    bool addTrayIcon() const {
        auto data = notification();
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = trayCallback;
        data.hIcon = icon ? icon : LoadIconW(nullptr, IDI_WARNING);
        wcscpy_s(data.szTip, L"Idle Shutdown");
        if (!Shell_NotifyIconW(NIM_ADD, &data) && !Shell_NotifyIconW(NIM_MODIFY, &data)) {
            return false;
        }
        data.uVersion = NOTIFYICON_VERSION_4;
        if (!Shell_NotifyIconW(NIM_SETVERSION, &data)) {
            Shell_NotifyIconW(NIM_DELETE, &data);
            return false;
        }
        return true;
    }

    void registerTrayIcon() {
        trayRegistered = addTrayIcon();
        if (trayRegistered) {
            trayFallbackAt = 0;
            trayFallbackShown = false;
        } else {
            const auto now = GetTickCount64();
            trayRetryAt = now + 1000;
            if (!trayFallbackAt) {
                trayFallbackAt = now + 10000;
            }
        }
    }

    void pollTray() {
        if (trayRegistered) {
            return;
        }
        const auto now = GetTickCount64();
        if (now >= trayRetryAt) {
            registerTrayIcon();
        }
        if (!trayRegistered && !trayFallbackShown && now >= trayFallbackAt) {
            trayFallbackShown = true;
            if (!settingsWindow) {
                openSettings();
            }
        }
    }

    LRESULT handleTray(HWND host, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_INPUT) {
            handleInput(reinterpret_cast<HRAWINPUT>(lParam));
            return DefWindowProcW(host, message, wParam, lParam);
        }
        if (message == trayCallback) {
            switch (LOWORD(lParam)) {
            case NIN_SELECT:
            case NIN_KEYSELECT:
            case WM_LBUTTONUP:
                openSettings();
                break;
            case WM_CONTEXTMENU:
                showMenu({static_cast<short>(LOWORD(wParam)), static_cast<short>(HIWORD(wParam))});
                break;
            }
            return 0;
        }
        if (message == showSettingsMessage) {
            openSettings();
            return 0;
        }
        if (message == taskbarCreated && taskbarCreated != 0) {
            registerTrayIcon();
            return 0;
        }
        if (message == WM_TIMER && wParam == frameTimer) {
            updatePreview();
            return 0;
        }
        if (message == WM_TIMER && wParam == idleTimer) {
            if (blackoutDeadline && GetTickCount64() >= blackoutDeadline) {
                restoreAfterShutdown();
            }
            pollTray();
            refreshStartupSetting();
            pollIdle();
            return 0;
        }
        if (message == WM_ENDSESSION && !wParam && blackoutDeadline) {
            restoreAfterShutdown();
            return 0;
        }
        if (message == WM_DISPLAYCHANGE || (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL)) {
            if (message == WM_DISPLAYCHANGE) {
                absolutePosition.reset();
                mouseMotion = {};
            }
            cancelPreview();
            return 0;
        }
        if (message == WM_CLOSE && shutdownCommitted(GetTickCount64())) {
            return 0;
        }
        if (message == WM_DESTROY) {
            closePreview();
            KillTimer(host, idleTimer);
            auto data = notification();
            Shell_NotifyIconW(NIM_DELETE, &data);
            if (icon) {
                DestroyIcon(icon);
            }
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(host, message, wParam, lParam);
    }

    void showMenu(POINT point) {
        const HMENU menu = CreatePopupMenu();
        if (!menu) {
            return;
        }
        AppendMenuW(menu, MF_STRING, IDOK, L"Settings");
        AppendMenuW(menu, MF_STRING, previewButton, L"Preview");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, exitItem, L"Exit");
        SetMenuDefaultItem(menu, IDOK, FALSE);
        if (point.x == -1 && point.y == -1) {
            GetCursorPos(&point);
        }
        SetForegroundWindow(window);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                            point.x, point.y, 0, window, nullptr);
        PostMessageW(window, WM_NULL, 0, 0);
        DestroyMenu(menu);
        switch (command) {
        case IDOK:
            openSettings();
            break;
        case previewButton:
            startPreview(preferences);
            break;
        case exitItem:
            SendMessageW(window, WM_CLOSE, 0, 0);
            break;
        }
    }

    static HFONT makeSettingsFont(UINT dpi, int pointSize, int weight) {
        return CreateFontW(-MulDiv(pointSize, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    }

    void layoutSettings(POINT origin, bool center) {
        const UINT dpi = GetDpiForWindow(settingsWindow);
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(settingsWindow, GWL_STYLE));
        const auto extended = static_cast<DWORD>(GetWindowLongPtrW(settingsWindow, GWL_EXSTYLE));
        RECT bounds{0, 0, pixels(settingsWidth, dpi), pixels(settingsHeight, dpi)};
        AdjustWindowRectExForDpi(&bounds, style, FALSE, extended, dpi);
        const int fullWidth = bounds.right - bounds.left;
        const int fullHeight = bounds.bottom - bounds.top;
        if (center) {
            MONITORINFO info{sizeof(info)};
            if (GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTONEAREST), &info)) {
                origin.x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - fullWidth) / 2;
                origin.y = info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - fullHeight) / 2;
            }
        }

        const HFONT newFont = makeSettingsFont(dpi, 10, FW_NORMAL);
        const HFONT newHeadingFont = makeSettingsFont(dpi, 16, FW_SEMIBOLD);
        for (const auto& control : controls) {
            const HWND child = GetDlgItem(settingsWindow, control.id);
            SetWindowPos(child, nullptr, pixels(control.x, dpi), pixels(control.y, dpi),
                         pixels(control.width, dpi), pixels(control.height, dpi), SWP_NOZORDER | SWP_NOACTIVATE);
            const auto selected = control.id == settingsHeading ? newHeadingFont : newFont;
            const auto font = selected ? selected : GetStockObject(DEFAULT_GUI_FONT);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        if (settingsFont) {
            DeleteObject(settingsFont);
        }
        settingsFont = newFont;
        if (headingFont) {
            DeleteObject(headingFont);
        }
        headingFont = newHeadingFont;
        SetWindowPos(settingsWindow, nullptr, origin.x, origin.y, fullWidth, fullHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    static void paintSettings(HWND dialog, HDC dc) {
        RECT area{};
        GetClientRect(dialog, &area);
        const auto scale = [dpi = GetDpiForWindow(dialog)](int value) { return pixels(value, dpi); };
        fill(dc, area, panelBackground);
        fill(dc, RECT{0, 0, area.right, scale(2)}, panelRed);
        for (const int y : {77, 166, 253, 324, 373}) {
            fill(dc, RECT{scale(22), scale(y), scale(397), scale(y + 1)}, panelBorder);
        }
        for (const int y : {124, 178}) {
            RECT field{scale(317), scale(y), scale(401), scale(y + 34)};
            fill(dc, field, panelBorder);
            InflateRect(&field, -scale(1), -scale(1));
            fill(dc, field, panelField);
        }

        const auto savedDC = SaveDC(dc);
        SelectObject(dc, GetStockObject(DC_PEN));
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        SetDCPenColor(dc, panelBorder);
        Ellipse(dc, scale(346), scale(16), scale(398), scale(68));
        Ellipse(dc, scale(352), scale(22), scale(392), scale(62));
        SetDCPenColor(dc, panelRed);
        const POINT triangle[] = {{scale(372), scale(29)}, {scale(383), scale(51)},
                                  {scale(361), scale(51)}, {scale(372), scale(29)}};
        Polyline(dc, triangle, ARRAYSIZE(triangle));
        fill(dc, RECT{scale(371), scale(36), scale(373), scale(44)}, panelRed);
        fill(dc, RECT{scale(371), scale(46), scale(373), scale(48)}, panelRed);
        RestoreDC(dc, savedDC);
    }

    void openSettings() {
        if (settingsWindow) {
            SetForegroundWindow(settingsWindow);
            return;
        }

        POINT cursor{};
        GetCursorPos(&cursor);
        settingsWindow = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, settingsClass,
                                         L"Idle Shutdown Settings", WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                         cursor.x, cursor.y, 1, 1, window, nullptr, instance, this);
        if (!settingsWindow) {
            return;
        }
        const BOOL darkCaption = TRUE;
        DwmSetWindowAttribute(settingsWindow, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkCaption, sizeof(darkCaption));
        DwmSetWindowAttribute(settingsWindow, DWMWA_CAPTION_COLOR, &panelBackground, sizeof(panelBackground));
        DwmSetWindowAttribute(settingsWindow, DWMWA_TEXT_COLOR, &panelText, sizeof(panelText));
        DwmSetWindowAttribute(settingsWindow, DWMWA_BORDER_COLOR, &panelBorder, sizeof(panelBorder));
        for (const auto& control : controls) {
            const HWND child = CreateWindowExW(0, control.type, control.text, WS_CHILD | WS_VISIBLE | control.style,
                                               0, 0, 1, 1, settingsWindow,
                                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(control.id)),
                                               instance, nullptr);
            if (!child) {
                DestroyWindow(settingsWindow);
                return;
            }
            if (control.type[0] == L'B') {
                SetWindowSubclass(child, styledButton, 1, 0);
            }
        }
        SendDlgItemMessageW(settingsWindow, countdownField, EM_SETLIMITTEXT, 3, 0);
        SendDlgItemMessageW(settingsWindow, idleField, EM_SETLIMITTEXT, 3, 0);
        SetDlgItemInt(settingsWindow, countdownField, preferences.seconds, FALSE);
        SetDlgItemInt(settingsWindow, idleField, preferences.idleMinutes, FALSE);
        CheckDlgButton(settingsWindow, soundField, preferences.sounds ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(settingsWindow, automaticField, preferences.automatic ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(settingsWindow, forceField, preferences.forceShutdown ? BST_CHECKED : BST_UNCHECKED);
        savedStartAtLogin = startsAtLogin();
        CheckDlgButton(settingsWindow, startupField, savedStartAtLogin ? BST_CHECKED : BST_UNCHECKED);
        EnableWindow(GetDlgItem(settingsWindow, idleField), preferences.automatic != 0);
        layoutSettings(cursor, true);
        ShowWindow(settingsWindow, SW_SHOWNORMAL);
        SetForegroundWindow(settingsWindow);
        SendMessageW(settingsWindow, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
        SetFocus(GetDlgItem(settingsWindow, automaticField));
    }

    DWORD readNumber(int field, DWORD minimum, DWORD maximum, PCWSTR error) {
        wchar_t input[5]{};
        const int length = GetDlgItemTextW(settingsWindow, field, input, ARRAYSIZE(input));
        DWORD number = 0;
        for (int index = 0; index < length; ++index) {
            if (input[index] < L'0' || input[index] > L'9') {
                number = 0;
                break;
            }
            number = number * 10 + input[index] - L'0';
        }
        if (number >= minimum && number <= maximum) {
            return number;
        }
        MessageBoxW(settingsWindow, error, L"Invalid setting", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(settingsWindow, field));
        return 0;
    }

    bool readSettings(Preferences& choices) {
        const auto seconds = readNumber(countdownField, minimumSeconds, maximumSeconds,
                                        L"Enter a countdown from 5 to 120 seconds.");
        if (!seconds) {
            return false;
        }
        const DWORD automatic = IsDlgButtonChecked(settingsWindow, automaticField) == BST_CHECKED;
        const DWORD idleMinutes = automatic ? readNumber(idleField, minimumIdleMinutes, maximumIdleMinutes,
                                                        L"Enter an inactivity time from 1 to 240 minutes.")
                                             : preferences.idleMinutes;
        if (!idleMinutes) {
            return false;
        }
        const DWORD sounds = IsDlgButtonChecked(settingsWindow, soundField) == BST_CHECKED;
        const DWORD forceShutdown = IsDlgButtonChecked(settingsWindow, forceField) == BST_CHECKED;
        choices = {seconds, sounds, idleMinutes, automatic, forceShutdown};
        return true;
    }

    void refreshStartupSetting() {
        if (settingsWindow) {
            const bool actual = startsAtLogin();
            if (actual != savedStartAtLogin) {
                savedStartAtLogin = actual;
                CheckDlgButton(settingsWindow, startupField, actual ? BST_CHECKED : BST_UNCHECKED);
            }
        }
    }

    LRESULT handleSettings(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case automaticField:
                EnableWindow(GetDlgItem(dialog, idleField), IsDlgButtonChecked(dialog, automaticField) == BST_CHECKED);
                return 0;
            case previewButton:
            case IDOK: {
                Preferences choices;
                if (!readSettings(choices)) {
                    return 0;
                }
                if (LOWORD(wParam) == previewButton) {
                    startPreview(choices);
                    return 0;
                }
                const bool startAtLogin = IsDlgButtonChecked(dialog, startupField) == BST_CHECKED;
                if (!savePreferences(choices)) {
                    MessageBoxW(dialog, L"Could not save your settings.", L"Idle Shutdown", MB_OK | MB_ICONERROR);
                    return 0;
                }
                preferences = choices;
                resetIdleClock();
                const bool loginSaved = startAtLogin == savedStartAtLogin || setStartsAtLogin(startAtLogin);
                savedStartAtLogin = startsAtLogin();
                CheckDlgButton(dialog, startupField, savedStartAtLogin ? BST_CHECKED : BST_UNCHECKED);
                if (!loginSaved) {
                    MessageBoxW(dialog, L"Warning settings were saved. The login startup entry could not be updated.",
                                L"Idle Shutdown", MB_OK | MB_ICONERROR);
                } else if (trayRegistered) {
                    DestroyWindow(dialog);
                }
                return 0;
            }
            case IDCANCEL:
                DestroyWindow(dialog);
                return 0;
            }
            break;
        case DM_GETDEFID:
            return MAKELRESULT(IDOK, DC_HASDEFID);
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(dialog, &paint);
            paintSettings(dialog, dc);
            EndPaint(dialog, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            paintSettings(dialog, reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            const auto dc = reinterpret_cast<HDC>(wParam);
            const auto child = reinterpret_cast<HWND>(lParam);
            const int id = GetDlgCtrlID(child);
            const bool edit = id == idleField || id == countdownField;
            SetTextColor(dc, id == 201 || id == 204 || !IsWindowEnabled(child) ? panelMuted : panelText);
            if (edit) {
                SetBkColor(dc, panelField);
                SetDCBrushColor(dc, panelField);
                return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
            }
            SetBkMode(dc, TRANSPARENT);
            SetDCBrushColor(dc, panelBackground);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        case WM_DPICHANGED: {
            const auto* rect = reinterpret_cast<const RECT*>(lParam);
            layoutSettings({rect->left, rect->top}, false);
            return 0;
        }
        case WM_DESTROY:
            if (settingsFont) {
                DeleteObject(settingsFont);
                settingsFont = nullptr;
            }
            if (headingFont) {
                DeleteObject(headingFont);
                headingFont = nullptr;
            }
            settingsWindow = nullptr;
            if (!trayRegistered) {
                PostMessageW(window, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        return DefWindowProcW(dialog, message, wParam, lParam);
    }

    static BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
        auto& monitors = *reinterpret_cast<std::vector<HMONITOR>*>(data);
        monitors.push_back(monitor);
        return TRUE;
    }

    bool addPreviewView(HMONITOR monitor, DWORD seconds, bool forcePowerOff) {
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(monitor, &info)) {
            return false;
        }
        const HWND layer = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                                              WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                          overlayClass, L"Idle Shutdown", WS_POPUP,
                                          info.rcMonitor.left, info.rcMonitor.top, 1, 1,
                                          window, nullptr, instance, nullptr);
        if (!layer) {
            return false;
        }
        auto renderer = std::make_unique<Overlay>(seconds, forcePowerOff);
        if (!renderer->initialize(layer, monitor) || !renderer->frame(layer, 0)) {
            DestroyWindow(layer);
            return false;
        }
        views.push_back({layer, std::move(renderer)});
        return true;
    }

    static bool latestInput(DWORD& tick) {
        LASTINPUTINFO info{sizeof(info)};
        if (!GetLastInputInfo(&info)) {
            return false;
        }
        tick = info.dwTime;
        return true;
    }

    void resetIdleClock() {
        const DWORD now = GetTickCount();
        idle = {now};
        mouseMotion = {};
        if (!rawInputRegistered) {
            fallbackInputTick = now;
            latestInput(fallbackInputTick);
        }
    }

    void recordActivity() {
        idle = {GetTickCount()};
        mouseMotion = {};
        if (automaticPreview) {
            cancelPreview();
        }
    }

    void handleInput(HRAWINPUT handle) {
        RAWINPUT input{};
        UINT size = sizeof(input);
        if (GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) {
            return;
        }
        if (input.header.dwType == RIM_TYPEKEYBOARD) {
            if (input.data.keyboard.VKey != 255) {
                recordActivity();
            }
        } else if (input.header.dwType == RIM_TYPEMOUSE) {
            const auto& mouse = input.data.mouse;
            const auto delta = mouseDelta(input.header.hDevice, mouse);
            if (mouse.usButtonFlags || mouseMotion.moved(input.header.hDevice, static_cast<DWORD>(GetMessageTime()),
                                                        delta.x, delta.y)) {
                recordActivity();
            }
        }
    }

    POINT mouseDelta(HANDLE device, const RAWMOUSE& mouse) {
        if (!(mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            absolutePosition.reset();
            return {mouse.lLastX, mouse.lLastY};
        }
        const bool virtualDesktop = (mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        const POINT position{
            MulDiv(mouse.lLastX, GetSystemMetrics(virtualDesktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN), 65535),
            MulDiv(mouse.lLastY, GetSystemMetrics(virtualDesktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN), 65535),
        };
        POINT delta{};
        if (absolutePosition && absoluteDevice == device && absoluteVirtualDesktop == virtualDesktop) {
            delta = {position.x - absolutePosition->x, position.y - absolutePosition->y};
        }
        absolutePosition = position;
        absoluteDevice = device;
        absoluteVirtualDesktop = virtualDesktop;
        return delta;
    }

    void pollFallbackInput() {
        DWORD input{};
        if (!rawInputRegistered && latestInput(input) && input != fallbackInputTick) {
            fallbackInputTick = input;
            recordActivity();
        }
    }

    void pollIdle() {
        if (!preferences.automatic) {
            return;
        }
        pollFallbackInput();
        if (!idle.handled && GetTickCount() - idle.began >= preferences.idleMinutes * 60000u) {
            idle.handled = true;
            startPreview(preferences, true);
        }
    }

    void startPreview(const Preferences& choices, bool automatic = false) {
        if (!views.empty()) {
            return;
        }
        const bool forcePowerOff = automatic && choices.forceShutdown;
        std::vector<HMONITOR> monitors;
        if (!EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&monitors)) ||
            monitors.empty()) {
            showPreviewError(automatic, forcePowerOff);
            return;
        }
        views.reserve(monitors.size());
        for (const HMONITOR monitor : monitors) {
            if (!addPreviewView(monitor, choices.seconds, forcePowerOff)) {
                closePreview();
                showPreviewError(automatic, forcePowerOff);
                return;
            }
        }
        if (!SetTimer(window, frameTimer, 16, nullptr)) {
            closePreview();
            showPreviewError(automatic, forcePowerOff);
            return;
        }

        preview = choices;
        automaticPreview = automatic;
        previousSecond = static_cast<int>(preview.seconds);
        for (const auto& view : views) {
            ShowWindow(view.window, SW_SHOWNOACTIVATE);
        }
        previewStart = GetTickCount64();
        if (preview.sounds) {
            play(alert);
        }
    }

    void updatePreview() {
        if (views.empty()) {
            return;
        }
        const ULONGLONG now = GetTickCount64();
        const float time = static_cast<float>(now - previewStart) / 1000.0f;
        const float duration = static_cast<float>(preview.seconds);
        const bool committed = shutdownCommitted(now);
        if (time >= intro + duration + outro) {
            finishPreview(committed);
            return;
        }
        if (!committed) {
            if (automaticPreview) {
                pollFallbackInput();
                if (views.empty()) {
                    return;
                }
            }
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                closePreview();
                return;
            }
        }
        const int seconds = static_cast<int>(std::ceil(std::max(0.0f, duration - std::max(0.0f, time - intro))));
        if (preview.sounds && seconds > 0 && seconds <= 3 && seconds != previousSecond) {
            play(tick);
        }
        previousSecond = seconds;
        for (const auto& view : views) {
            if (!view.renderer->frame(view.window, time)) {
                finishPreview(committed);
                return;
            }
        }
    }

    bool shutdownCommitted(ULONGLONG now) const {
        return !views.empty() && automaticPreview && preview.forceShutdown &&
               now - previewStart >= preview.seconds * 1000ull + static_cast<ULONGLONG>(intro * 1000);
    }

    void cancelPreview() {
        if (!shutdownCommitted(GetTickCount64())) {
            closePreview();
        }
    }

    void finishPreview(bool forcePowerOff) {
        if (!forcePowerOff) {
            closePreview();
            return;
        }
        KillTimer(window, frameTimer);
        for (const auto& view : views) {
            view.renderer->blank(view.window);
        }
        DwmFlush();
        // Windows can accept a shutdown request and still abort it asynchronously.
        blackoutDeadline = GetTickCount64() + 15000;
        const DWORD result = requestForcedPowerOff();
        if (result == ERROR_SUCCESS) {
            if (blackoutDeadline) {
                monitorPowerOff = true;
                DefWindowProcW(window, WM_SYSCOMMAND, SC_MONITORPOWER, 2);
            }
            return;
        }
        restoreAfterShutdown();
        wchar_t message[160]{};
        swprintf_s(message, L"Windows could not shut down this PC (error %lu).", result);
        MessageBoxW(settingsWindow, message, L"Idle Shutdown", MB_OK | MB_ICONERROR);
    }

    void restoreAfterShutdown() {
        blackoutDeadline = 0;
        closePreview();
        resetIdleClock();
        if (monitorPowerOff) {
            monitorPowerOff = false;
            DefWindowProcW(window, WM_SYSCOMMAND, SC_MONITORPOWER, -1);
        }
    }

    void closePreview() {
        if (views.empty()) {
            return;
        }
        KillTimer(window, frameTimer);
        if (preview.sounds) {
            PlaySoundW(nullptr, nullptr, 0);
        }
        for (const auto& view : views) {
            DestroyWindow(view.window);
        }
        views.clear();
        automaticPreview = false;
    }

    void showPreviewError(bool automatic, bool forcePowerOff) const {
        if (!automatic) {
            MessageBoxW(settingsWindow, L"Could not start the preview on every display.", L"Idle Shutdown",
                        MB_OK | MB_ICONERROR);
        } else if (forcePowerOff) {
            auto data = notification();
            data.uFlags = NIF_INFO;
            data.dwInfoFlags = NIIF_ERROR;
            wcscpy_s(data.szInfoTitle, L"Idle Shutdown");
            wcscpy_s(data.szInfo, L"The countdown could not start on every display. Windows did not shut down.");
            if (!trayRegistered || !Shell_NotifyIconW(NIM_MODIFY, &data)) {
                MessageBoxW(settingsWindow, data.szInfo, data.szInfoTitle, MB_OK | MB_ICONERROR);
            }
        }
    }

    static void play(const std::vector<char>& sound) {
        PlaySoundW(reinterpret_cast<LPCWSTR>(sound.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }

    HINSTANCE instance;
    HWND window{};
    HWND settingsWindow{};
    HFONT settingsFont{};
    HFONT headingFont{};
    HICON icon{};
    UINT taskbarCreated{};
    ULONGLONG trayRetryAt{};
    ULONGLONG trayFallbackAt{};
    bool trayRegistered{};
    bool trayFallbackShown{};
    Preferences preferences = loadPreferences();
    bool savedStartAtLogin = false;
    Preferences preview{};
    IdlePeriod idle;
    MouseMotion mouseMotion;
    std::optional<POINT> absolutePosition;
    HANDLE absoluteDevice{};
    DWORD fallbackInputTick{};
    bool absoluteVirtualDesktop{};
    bool rawInputRegistered{};
    bool automaticPreview{};
    std::vector<PreviewView> views;
    std::vector<char> alert = sound::warningSound();
    std::vector<char> tick = sound::countdownSound();
    ULONGLONG previewStart{};
    ULONGLONG blackoutDeadline{};
    bool monitorPowerOff{};
    int previousSecond{};
};

int exitOwnedInstance() {
    const auto& ownImage = executablePath();
    if (ownImage.empty()) {
        return 1;
    }
    const HWND existing = FindWindowW(trayClass, nullptr);
    if (!existing) {
        return 0;
    }
    DWORD pid{};
    if (!GetWindowThreadProcessId(existing, &pid) || !pid) {
        return IsWindow(existing) ? 1 : 0;
    }
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) {
        return GetLastError() == ERROR_INVALID_PARAMETER ? 0 : 1;
    }
    int result = 1;
    std::wstring image(32768, L'\0');
    DWORD length = static_cast<DWORD>(image.size());
    if (QueryFullProcessImageNameW(process, 0, image.data(), &length)) {
        DWORD currentPid{};
        GetWindowThreadProcessId(existing, &currentPid);
        if (!samePath(std::wstring_view(image.data(), length), ownImage)) {
            // The tray belongs to a different copy, such as a portable installation.
            result = 0;
        } else if (currentPid == pid && PostMessageW(existing, WM_CLOSE, 0, 0)) {
            result = WaitForSingleObject(process, 4500) == WAIT_OBJECT_0 ? 0 : 1;
        }
    }
    if (result != 0 && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
        result = 0;
    }
    CloseHandle(process);
    return result;
}

enum class LaunchMode { normal, settings, autostart, exit, removeStartup, migrateStartup, invalid };

struct LaunchOptions {
    LaunchMode mode = LaunchMode::normal;
    std::wstring previousPath;
};

LaunchOptions parseLaunchOptions() {
    int argc = 0;
    auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return {LaunchMode::invalid, {}};
    }
    LaunchOptions result{LaunchMode::invalid, {}};
    if (argc == 1) {
        result.mode = LaunchMode::normal;
    } else if (argc == 2) {
        const std::wstring_view option = argv[1];
        if (option == L"--settings") result.mode = LaunchMode::settings;
        else if (option == L"--autostart") result.mode = LaunchMode::autostart;
        else if (option == L"--exit") result.mode = LaunchMode::exit;
        else if (option == L"--remove-startup") result.mode = LaunchMode::removeStartup;
    } else if (argc == 3 && std::wstring_view(argv[1]) == L"--migrate-startup") {
        result = {LaunchMode::migrateStartup, argv[2]};
    }
    LocalFree(argv);
    return result;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const auto options = parseLaunchOptions();
    switch (options.mode) {
    case LaunchMode::exit:
        return exitOwnedInstance();
    case LaunchMode::removeStartup:
        return removeOwnedStartup() ? 0 : 1;
    case LaunchMode::migrateStartup:
        return migrateStartup(options.previousPath) ? 0 : 1;
    case LaunchMode::invalid:
        return 2;
    case LaunchMode::autostart:
        if (!startsAtLogin()) {
            return 0;
        }
        break;
    case LaunchMode::normal:
    case LaunchMode::settings:
        break;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\ShutdownWarningTray");
    if (!singleton) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (options.mode != LaunchMode::autostart) {
            if (const HWND existing = FindWindowW(trayClass, nullptr)) {
                PostMessageW(existing, showSettingsMessage, 0, 0);
            }
        }
        CloseHandle(singleton);
        return 0;
    }
    const int result = TrayApp(instance).run(options.mode == LaunchMode::settings);
    CloseHandle(singleton);
    return result;
}
