#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#include <MinHook.h>
#include <PluginAPI.h>

#include "edf6_overlay_api.h"

namespace {

constexpr int kPlayerTeam = 0;
constexpr int kEnemyTeam = 1;
constexpr int32_t kDamageInfoTeamOffset = 0x24;
constexpr int32_t kTargetTeamOffset = 0x314;
constexpr ULONGLONG kVisibleAfterHitMs = 5000;
constexpr ULONGLONG kVisibleAfterKillMs = 1500;
constexpr DWORD kWaitForGameDllMs = 30000;
constexpr DWORD kWaitForHostMs = 30000;
constexpr int kDefaultToggleKey = VK_F3;
constexpr int kHitHistory = 64;
constexpr ULONGLONG kDpsWindowMs = 5000;
constexpr int kSegments = 10;
constexpr float kDesignScale = 0.55f;

constexpr uint32_t kCardBackground = 0x1E2422F2;
constexpr uint32_t kCardBorder = 0x2E3532FF;
constexpr uint32_t kSegmentEmpty = 0x2E3532FF;
constexpr uint32_t kTextColor = 0xE8ECE9FF;
constexpr uint32_t kSoftColor = 0xB7BEBAFF;
constexpr uint32_t kMutedColor = 0x8C938FFF;

const uint8_t kHpWriteBlock[] = {
    0xF3, 0x0F, 0x58, 0x87, 0x00, 0x00, 0x00, 0x00,
    0xF3, 0x0F, 0x5D, 0x87, 0x00, 0x00, 0x00, 0x00,
    0xF3, 0x0F, 0x5F, 0x87, 0x00, 0x00, 0x00, 0x00,
    0xF3, 0x0F, 0x11, 0x87, 0x00, 0x00, 0x00, 0x00,
};
const char kHpWriteBlockMask[] = "xxxx??xxxxxx??xxxxxx??xxxxxx??xx";

const uint8_t kReadsDamageInfoTeam[] = {0x49, 0x63, 0x45, 0x24};
const uint8_t kReadsTargetTeam[] = {0x4C, 0x63, 0x87, 0x14, 0x03, 0x00, 0x00};
const uint8_t kExpectedPrologue[] = {0x48, 0x8B, 0xC4};

using ApplyDamageFn = void(__fastcall *)(uintptr_t target, uintptr_t damageInfo);

struct Hit {
    ULONGLONG at = 0;
    float damage = 0.0f;
};

struct TrackedEnemy {
    uintptr_t target = 0;
    float hp = 0.0f;
    float maxHp = 0.0f;
    ULONGLONG lastUpdate = 0;
    float lastPlayerHit = 0.0f;
    Hit hits[kHitHistory];
    int hitCount = 0;
};

ApplyDamageFn originalApplyDamage = nullptr;
int32_t hpOffset = 0;
int32_t maxHpOffset = 0;
std::mutex trackedMutex;
TrackedEnemy tracked;
std::atomic<bool> counterEnabled{true};
std::atomic<const Edf6OverlayHost *> host{nullptr};

std::string GamePath(const char *relative) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    if (slash) {
        *(slash + 1) = '\0';
    }
    return std::string(path) + relative;
}

void Log(const char *message) {
    if (const Edf6OverlayHost *h = host.load()) {
        const std::string line = std::string("hp enemigos: ") + message;
        h->log(line.c_str());
        return;
    }
    FILE *fh = nullptr;
    if (fopen_s(&fh, GamePath("EnemyHp.log").c_str(), "a") == 0 && fh) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(fh, "[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, message);
        fclose(fh);
    }
}

void LogF(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Log(buf);
}

int ReadToggleKey() {
    std::ifstream in(GamePath("Mods\\EnemyHp\\config.ini"));
    std::string line;
    while (std::getline(in, line)) {
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        std::string key = line.substr(0, equals);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        if (key == "tecla" || key == "key") {
            const int vk = (int)strtol(line.c_str() + equals + 1, nullptr, 0);
            if (vk > 0 && vk < 256) {
                return vk;
            }
        }
    }
    return kDefaultToggleKey;
}

float ReadFloat(uintptr_t base, int32_t offset) {
    return *reinterpret_cast<const float *>(base + offset);
}

int32_t ReadInt(uintptr_t base, int32_t offset) {
    return *reinterpret_cast<const int32_t *>(base + offset);
}

void __fastcall HookedApplyDamage(uintptr_t target, uintptr_t damageInfo) {
    const float hpBefore = ReadFloat(target, hpOffset);
    const bool playerHitsEnemy = ReadInt(damageInfo, kDamageInfoTeamOffset) == kPlayerTeam &&
                                 ReadInt(target, kTargetTeamOffset) == kEnemyTeam;
    originalApplyDamage(target, damageInfo);
    const float hpAfter = ReadFloat(target, hpOffset);
    if (hpAfter == hpBefore) {
        return;
    }
    std::lock_guard<std::mutex> lock(trackedMutex);
    if (!playerHitsEnemy && target != tracked.target) {
        return;
    }
    if (target != tracked.target) {
        tracked = TrackedEnemy();
        tracked.target = target;
    }
    const ULONGLONG now = GetTickCount64();
    tracked.hp = hpAfter;
    tracked.maxHp = ReadFloat(target, maxHpOffset);
    tracked.lastUpdate = now;
    const float damage = hpBefore - hpAfter;
    if (playerHitsEnemy && damage > 0.0f) {
        tracked.lastPlayerHit = damage;
        tracked.hits[tracked.hitCount % kHitHistory] = Hit{now, damage};
        tracked.hitCount++;
    }
}

bool TextSection(HMODULE module, const uint8_t **start, size_t *size) {
    auto base = reinterpret_cast<const uint8_t *>(module);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
        if (memcmp(section->Name, ".text", 5) == 0) {
            *start = base + section->VirtualAddress;
            *size = section->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

const uint8_t *FindPattern(const uint8_t *start, size_t size, const uint8_t *pattern, const char *mask) {
    const size_t length = strlen(mask);
    for (size_t i = 0; i + length <= size; i++) {
        size_t j = 0;
        while (j < length && (mask[j] == '?' || start[i + j] == pattern[j])) {
            j++;
        }
        if (j == length) {
            return start + i;
        }
    }
    return nullptr;
}

bool ContainsBytes(const uint8_t *start, const uint8_t *end, const uint8_t *bytes, size_t length) {
    for (const uint8_t *p = start; p + length <= end; p++) {
        if (memcmp(p, bytes, length) == 0) {
            return true;
        }
    }
    return false;
}

const uint8_t *LocateApplyDamage(HMODULE gameDll) {
    const uint8_t *text = nullptr;
    size_t textSize = 0;
    if (!TextSection(gameDll, &text, &textSize)) {
        Log("EDF.dll sin seccion .text");
        return nullptr;
    }
    const uint8_t *hpWrite = FindPattern(text, textSize, kHpWriteBlock, kHpWriteBlockMask);
    if (!hpWrite) {
        Log("no encontre la escritura de la vida (cambio el juego?)");
        return nullptr;
    }
    const int32_t addOffset = *reinterpret_cast<const int32_t *>(hpWrite + 4);
    const int32_t storeOffset = *reinterpret_cast<const int32_t *>(hpWrite + 28);
    if (addOffset != storeOffset) {
        LogF("la vida se lee de +0x%X y se escribe en +0x%X", addOffset, storeOffset);
        return nullptr;
    }
    hpOffset = addOffset;
    maxHpOffset = *reinterpret_cast<const int32_t *>(hpWrite + 12);

    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(hpWrite), &imageBase, nullptr);
    if (!entry) {
        Log("sin entrada de .pdata para la funcion de dano");
        return nullptr;
    }
    const uint8_t *function = reinterpret_cast<const uint8_t *>(imageBase + entry->BeginAddress);
    if (memcmp(function, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0 ||
        !ContainsBytes(function, hpWrite, kReadsDamageInfoTeam, sizeof(kReadsDamageInfoTeam)) ||
        !ContainsBytes(function, hpWrite, kReadsTargetTeam, sizeof(kReadsTargetTeam))) {
        LogF("la funcion en EDF.dll+0x%llX no tiene la forma esperada",
             static_cast<unsigned long long>(function - reinterpret_cast<const uint8_t *>(gameDll)));
        return nullptr;
    }
    return function;
}

bool HookDamage() {
    HMODULE gameDll = nullptr;
    for (DWORD waited = 0; !(gameDll = GetModuleHandleA("EDF.dll")) && waited < kWaitForGameDllMs; waited += 100) {
        Sleep(100);
    }
    if (!gameDll) {
        Log("EDF.dll no cargo");
        return false;
    }
    const uint8_t *function = LocateApplyDamage(gameDll);
    if (!function) {
        return false;
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        LogF("MH_Initialize fallo (%d)", init);
        return false;
    }
    void *target = const_cast<uint8_t *>(function);
    if (MH_CreateHook(target, reinterpret_cast<void *>(&HookedApplyDamage),
                      reinterpret_cast<void **>(&originalApplyDamage)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        Log("no pude enganchar la funcion de dano");
        return false;
    }
    LogF("enganchado EDF.dll+0x%llX, vida en +0x%X, maxima en +0x%X",
         static_cast<unsigned long long>(function - reinterpret_cast<const uint8_t *>(gameDll)), hpOffset,
         maxHpOffset);
    return true;
}

TrackedEnemy Snapshot() {
    std::lock_guard<std::mutex> lock(trackedMutex);
    return tracked;
}

bool StillVisible(const TrackedEnemy &enemy, ULONGLONG now) {
    if (!counterEnabled || !enemy.target) {
        return false;
    }
    const ULONGLONG window = enemy.hp <= 0.0f ? kVisibleAfterKillMs : kVisibleAfterHitMs;
    return now - enemy.lastUpdate < window;
}

std::string WithThousands(float value) {
    char digits[32];
    snprintf(digits, sizeof(digits), "%.0f", value);
    std::string out;
    const int length = static_cast<int>(strlen(digits));
    for (int i = 0; i < length; i++) {
        if (i > 0 && (length - i) % 3 == 0) {
            out += ',';
        }
        out += digits[i];
    }
    return out;
}

uint32_t HealthColor(float fraction) {
    if (fraction > 0.5f) {
        return 0x5ED17AFF;
    }
    if (fraction > 0.2f) {
        return 0xF0A73AFF;
    }
    return 0xFF6B6BFF;
}

float PlayerDps(const TrackedEnemy &enemy, ULONGLONG now) {
    float total = 0.0f;
    ULONGLONG oldest = now;
    int inWindow = 0;
    const int stored = enemy.hitCount < kHitHistory ? enemy.hitCount : kHitHistory;
    for (int i = 0; i < stored; i++) {
        const Hit &hit = enemy.hits[i];
        if (now - hit.at <= kDpsWindowMs) {
            total += hit.damage;
            oldest = hit.at < oldest ? hit.at : oldest;
            inWindow++;
        }
    }
    if (inWindow < 2) {
        return 0.0f;
    }
    const float seconds = (float)(now - oldest) / 1000.0f;
    return total / (seconds > 1.0f ? seconds : 1.0f);
}

std::string TimeLeft(float seconds) {
    char buf[32];
    const int s = (int)std::ceil(seconds);
    if (s < 60) {
        snprintf(buf, sizeof(buf), "~%ds", s);
    } else {
        snprintf(buf, sizeof(buf), "~%dm %02ds", s / 60, s % 60);
    }
    return buf;
}

float TextWidth(const Edf6OverlayHost *h, float size, int font, float spacing, const char *text) {
    float w = 0.0f;
    float ht = 0.0f;
    h->textExSize(size, font, spacing, text, &w, &ht);
    return w;
}

float TextHeight(const Edf6OverlayHost *h, float size, int font, const char *text) {
    float w = 0.0f;
    float ht = 0.0f;
    h->textExSize(size, font, 0.0f, text, &w, &ht);
    return ht;
}

void OnToggle() {
    counterEnabled = !counterEnabled;
    Log(counterEnabled ? "contador visible" : "contador oculto");
}

int WantsDraw() {
    return StillVisible(Snapshot(), GetTickCount64()) ? 1 : 0;
}

void Draw(const Edf6OverlayHost *h) {
    const ULONGLONG now = GetTickCount64();
    const TrackedEnemy enemy = Snapshot();
    if (!StillVisible(enemy, now)) {
        return;
    }
    const float k = h->scale() * kDesignScale;
    float screenW = 0.0f;
    float screenH = 0.0f;
    h->screenSize(&screenW, &screenH);
    const float width = 560.0f * k;
    const float height = 96.0f * k;
    const float pad = 16.0f * k;
    const float x0 = (screenW - width) * 0.5f;
    const float y0 = 40.0f * k;
    const float right = x0 + width - pad;
    h->fillRect(x0, y0, x0 + width, y0 + height, kCardBackground);
    h->strokeRect(x0, y0, x0 + width, y0 + height, kCardBorder, k);

    const bool defeated = enemy.hp <= 0.0f;
    float fraction = enemy.maxHp > 0.0f ? enemy.hp / enemy.maxHp : 0.0f;
    fraction = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
    const uint32_t color = HealthColor(fraction);

    h->fillRect(x0 + pad, y0 + pad + 6.0f * k, x0 + pad + 7.0f * k, y0 + pad + 13.0f * k, color);
    h->textEx(x0 + pad + 16.0f * k, y0 + pad, 15.0f * k, kTextColor, EDF6_FONT_BOLD, 2.5f * k, "ENEMY");

    char percent[32];
    if (defeated) {
        snprintf(percent, sizeof(percent), "DEFEATED");
    } else {
        snprintf(percent, sizeof(percent), "%.1f%%", fraction * 100.0f);
    }
    const float bigSize = 22.0f * k;
    const float bigW = TextWidth(h, bigSize, EDF6_FONT_BOLD, 0.0f, percent);
    const float bigH = TextHeight(h, bigSize, EDF6_FONT_BOLD, percent);
    const float bigY = y0 + pad - 5.0f * k;
    h->textEx(right - bigW, bigY, bigSize, color, EDF6_FONT_BOLD, 0.0f, percent);

    const float smallSize = 13.0f * k;
    const float smallY = bigY + bigH - TextHeight(h, smallSize, EDF6_FONT_MONO, "0") - 3.0f * k;
    const float dps = PlayerDps(enemy, now);
    if (dps > 0.0f && !defeated) {
        float cursor = right - bigW - 18.0f * k;
        const std::string eta = TimeLeft(enemy.hp / dps);
        const float etaW = TextWidth(h, smallSize, EDF6_FONT_MONO, 0.0f, eta.c_str());
        h->textEx(cursor - etaW, smallY, smallSize, kMutedColor, EDF6_FONT_MONO, 0.0f, eta.c_str());
        cursor -= etaW + 14.0f * k;
        const std::string rate = WithThousands(dps) + " dps";
        const float rateW = TextWidth(h, smallSize, EDF6_FONT_MONO, 0.0f, rate.c_str());
        h->textEx(cursor - rateW, smallY, smallSize, kMutedColor, EDF6_FONT_MONO, 0.0f, rate.c_str());
    }

    const float barY = y0 + 44.0f * k;
    const float barH = 14.0f * k;
    const float gap = 2.0f * k;
    const float segmentW = (width - 2.0f * pad - gap * (kSegments - 1)) / kSegments;
    for (int i = 0; i < kSegments; i++) {
        const float sx = x0 + pad + i * (segmentW + gap);
        h->fillRect(sx, barY, sx + segmentW, barY + barH, kSegmentEmpty);
        float fill = fraction * kSegments - i;
        fill = fill < 0.0f ? 0.0f : (fill > 1.0f ? 1.0f : fill);
        if (fill > 0.0f) {
            h->fillRect(sx, barY, sx + segmentW * fill, barY + barH, color);
        }
    }

    const float bottomY = barY + barH + 10.0f * k;
    const std::string amounts = WithThousands(enemy.hp > 0.0f ? enemy.hp : 0.0f) + " / " + WithThousands(enemy.maxHp);
    h->textEx(x0 + pad, bottomY, smallSize, kSoftColor, EDF6_FONT_MONO, 0.0f, amounts.c_str());
    if (enemy.lastPlayerHit > 0.0f) {
        const std::string hit = "-" + WithThousands(enemy.lastPlayerHit);
        const float hitW = TextWidth(h, smallSize, EDF6_FONT_MONO, 0.0f, hit.c_str());
        h->textEx(right - hitW, bottomY, smallSize, kMutedColor, EDF6_FONT_MONO, 0.0f, hit.c_str());
    }
}

Edf6OverlayModule overlayModule = {EDF6_OVERLAY_API_VERSION, "Enemy HP", kDefaultToggleKey, &OnToggle, &WantsDraw, &Draw};

bool RegisterWithHost() {
    HMODULE hostDll = nullptr;
    for (DWORD waited = 0; !(hostDll = GetModuleHandleA(EDF6_OVERLAY_HOST_DLL)) && waited < kWaitForHostMs;
         waited += 100) {
        Sleep(100);
    }
    if (!hostDll) {
        Log("no encontre " EDF6_OVERLAY_HOST_DLL ": este mod necesita el Compendium para dibujar y leer la tecla");
        return false;
    }
    auto registerModule =
        reinterpret_cast<Edf6OverlayRegisterFn>(GetProcAddress(hostDll, EDF6_OVERLAY_REGISTER));
    if (!registerModule) {
        Log("el Compendium instalado no acepta modulos: hace falta la 0.3.0 o mas nueva");
        return false;
    }
    overlayModule.toggleKey = ReadToggleKey();
    const Edf6OverlayHost *granted = nullptr;
    if (!registerModule(&overlayModule, &granted) || !granted) {
        Log("el Compendium rechazo el modulo: hace falta la 0.3.0 o mas nueva (detalle en Compendium.log)");
        return false;
    }
    host = granted;
    return true;
}

DWORD WINAPI StartThread(LPVOID) {
    if (RegisterWithHost()) {
        LogF("registrado en el Compendium, tecla 0x%02X", overlayModule.toggleKey);
        HookDamage();
    }
    return 0;
}

}

extern "C" BOOL __declspec(dllexport) EML6_Load(PluginInfo *pluginInfo) {
    pluginInfo->infoVersion = PluginInfo::MaxInfoVer;
    pluginInfo->name = "Enemy HP";
    pluginInfo->version = PLUG_VER(1, 1, 0, 0);
    static bool started = false;
    if (started) {
        return TRUE;
    }
    started = true;
    CreateThread(nullptr, 0, StartThread, nullptr, 0, nullptr);
    return TRUE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
