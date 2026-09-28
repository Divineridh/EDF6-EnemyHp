#include <windows.h>

#include <algorithm>
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
constexpr ULONGLONG kVisibleAfterKillMs = 4000;
constexpr DWORD kWaitForGameDllMs = 30000;
constexpr DWORD kWaitForHostMs = 30000;
constexpr int kDefaultToggleKey = VK_F3;
constexpr int kHitHistory = 64;
constexpr ULONGLONG kDpsWindowMs = 5000;
constexpr int kSegments = 10;
constexpr float kDesignScale = 0.55f;
constexpr int kMaxTracked = 16;
constexpr int kMaxCompact = 6;
constexpr int kMaxHpSamples = 32;
constexpr int kMinSamplesToCompare = 5;
constexpr float kExceptionalHpRatio = 4.0f;
constexpr ULONGLONG kExceptionalAfterMs = 4000;
constexpr ULONGLONG kTrailHoldMs = 500;
constexpr ULONGLONG kTrailShrinkMs = 300;

constexpr uint32_t kCardBackground = 0x1E2422F2;
constexpr uint32_t kCardBorder = 0x2E3532FF;
constexpr uint32_t kSegmentEmpty = 0x2E3532FF;
constexpr uint32_t kTrailColor = 0xD6CCB8FF;
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
    ULONGLONG firstPlayerHitAt = 0;
    ULONGLONG lastPlayerHitAt = 0;
    ULONGLONG killedAt = 0;
    float lastPlayerHit = 0.0f;
    float playerDamage = 0.0f;
    float trailHp = 0.0f;
    bool exceptional = false;
    Hit hits[kHitHistory];
    int hitCount = 0;
};

struct Tracker {
    TrackedEnemy enemies[kMaxTracked];
    float maxHpSamples[kMaxHpSamples] = {};
    int sampleCount = 0;
};

ApplyDamageFn originalApplyDamage = nullptr;
int32_t hpOffset = 0;
int32_t maxHpOffset = 0;
std::mutex trackedMutex;
Tracker tracker;
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

TrackedEnemy *FindTracked(uintptr_t target) {
    for (TrackedEnemy &enemy : tracker.enemies) {
        if (enemy.target == target) {
            return &enemy;
        }
    }
    return nullptr;
}

TrackedEnemy *OldestSlot() {
    TrackedEnemy *oldest = &tracker.enemies[0];
    for (TrackedEnemy &enemy : tracker.enemies) {
        if (!enemy.target) {
            return &enemy;
        }
        if (enemy.lastUpdate < oldest->lastUpdate) {
            oldest = &enemy;
        }
    }
    return oldest;
}

float MedianMaxHp() {
    const int count = tracker.sampleCount < kMaxHpSamples ? tracker.sampleCount : kMaxHpSamples;
    float sorted[kMaxHpSamples];
    std::copy(tracker.maxHpSamples, tracker.maxHpSamples + count, sorted);
    std::nth_element(sorted, sorted + count / 2, sorted + count);
    return sorted[count / 2];
}

bool StandsOut(float maxHp) {
    return tracker.sampleCount >= kMinSamplesToCompare && maxHp >= MedianMaxHp() * kExceptionalHpRatio;
}

void RecordMaxHp(float maxHp) {
    tracker.maxHpSamples[tracker.sampleCount % kMaxHpSamples] = maxHp;
    tracker.sampleCount++;
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
    TrackedEnemy *enemy = FindTracked(target);
    const bool reusedPointer = enemy && enemy->hp <= 0.0f && hpBefore > 0.0f;
    if (reusedPointer) {
        *enemy = TrackedEnemy();
    }
    if (!enemy || reusedPointer) {
        if (!playerHitsEnemy) {
            return;
        }
        enemy = enemy ? enemy : OldestSlot();
        *enemy = TrackedEnemy();
        enemy->target = target;
        enemy->maxHp = ReadFloat(target, maxHpOffset);
        enemy->exceptional = StandsOut(enemy->maxHp);
        RecordMaxHp(enemy->maxHp);
    }
    const ULONGLONG now = GetTickCount64();
    enemy->hp = hpAfter;
    enemy->maxHp = ReadFloat(target, maxHpOffset);
    enemy->lastUpdate = now;
    if (hpAfter <= 0.0f && !enemy->killedAt) {
        enemy->killedAt = now;
    }
    const float damage = hpBefore - hpAfter;
    if (playerHitsEnemy && damage > 0.0f) {
        if (!enemy->firstPlayerHitAt) {
            enemy->firstPlayerHitAt = now;
        }
        if (now - enemy->lastPlayerHitAt > kTrailHoldMs) {
            enemy->trailHp = hpBefore;
        }
        enemy->lastPlayerHitAt = now;
        enemy->lastPlayerHit = damage;
        enemy->playerDamage += damage;
        enemy->hits[enemy->hitCount % kHitHistory] = Hit{now, damage};
        enemy->hitCount++;
        if (hpAfter > 0.0f && now - enemy->firstPlayerHitAt > kExceptionalAfterMs) {
            enemy->exceptional = true;
        }
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

bool Visible(const TrackedEnemy &enemy, ULONGLONG now) {
    if (!enemy.target || !enemy.firstPlayerHitAt) {
        return false;
    }
    const ULONGLONG window = enemy.hp <= 0.0f ? kVisibleAfterKillMs : kVisibleAfterHitMs;
    return now - enemy.lastUpdate < window;
}

int VisibleEnemies(TrackedEnemy *out, ULONGLONG now) {
    std::lock_guard<std::mutex> lock(trackedMutex);
    int count = 0;
    for (const TrackedEnemy &enemy : tracker.enemies) {
        if (Visible(enemy, now)) {
            out[count++] = enemy;
        }
    }
    return count;
}

float Clamp01(float value) {
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

float HpFraction(const TrackedEnemy &enemy) {
    return enemy.maxHp > 0.0f ? Clamp01(enemy.hp / enemy.maxHp) : 0.0f;
}

float TrailFraction(const TrackedEnemy &enemy, ULONGLONG now) {
    const float current = HpFraction(enemy);
    const ULONGLONG since = now - enemy.lastPlayerHitAt;
    if (enemy.maxHp <= 0.0f || since >= kTrailHoldMs + kTrailShrinkMs) {
        return current;
    }
    const float keep = since <= kTrailHoldMs ? 1.0f : 1.0f - (float)(since - kTrailHoldMs) / kTrailShrinkMs;
    const float hp = enemy.hp > 0.0f ? enemy.hp : 0.0f;
    const float trail = Clamp01((hp + (enemy.trailHp - hp) * keep) / enemy.maxHp);
    return trail > current ? trail : current;
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

std::string KillSummary(const TrackedEnemy &enemy) {
    const float seconds = (float)(enemy.killedAt - enemy.firstPlayerHitAt) / 1000.0f;
    char buf[64];
    if (seconds < 1.0f) {
        snprintf(buf, sizeof(buf), "%.1fs", seconds);
    } else {
        snprintf(buf, sizeof(buf), "%.1fs  %s dps", seconds, WithThousands(enemy.playerDamage / seconds).c_str());
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
    if (!counterEnabled) {
        return 0;
    }
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(trackedMutex);
    for (const TrackedEnemy &enemy : tracker.enemies) {
        if (Visible(enemy, now)) {
            return 1;
        }
    }
    return 0;
}

void DrawCard(const Edf6OverlayHost *h, float x0, float y0, float x1, float y1, float k) {
    h->fillRect(x0, y0, x1, y1, kCardBackground);
    h->strokeRect(x0, y0, x1, y1, kCardBorder, k);
}

void DrawRightText(const Edf6OverlayHost *h, float right, float y, float size, uint32_t color, int font,
                   const std::string &text) {
    const float w = TextWidth(h, size, font, 0.0f, text.c_str());
    h->textEx(right - w, y, size, color, font, 0.0f, text.c_str());
}

void DrawCompact(const Edf6OverlayHost *h, const TrackedEnemy &enemy, float x0, float y0, float width, float k,
                 ULONGLONG now) {
    const float height = 56.0f * k;
    const float pad = 10.0f * k;
    const float right = x0 + width - pad;
    DrawCard(h, x0, y0, x0 + width, y0 + height, k);

    const bool defeated = enemy.hp <= 0.0f;
    const float fraction = HpFraction(enemy);
    const uint32_t color = HealthColor(fraction);
    const float labelSize = 12.0f * k;
    h->textEx(x0 + pad, y0 + pad - 2.0f * k, labelSize, defeated ? kMutedColor : kTextColor, EDF6_FONT_BOLD,
              2.0f * k, "ENEMY");
    char percent[32];
    if (defeated) {
        snprintf(percent, sizeof(percent), "DEFEATED");
    } else if (fraction < 0.01f) {
        snprintf(percent, sizeof(percent), "<1%%");
    } else {
        snprintf(percent, sizeof(percent), "%.0f%%", fraction * 100.0f);
    }
    DrawRightText(h, right, y0 + pad - 2.0f * k, labelSize, color, EDF6_FONT_MONO, percent);

    const float barX0 = x0 + pad;
    const float barX1 = right;
    const float barY = y0 + 28.0f * k;
    const float barH = 4.0f * k;
    h->fillRect(barX0, barY, barX1, barY + barH, kSegmentEmpty);
    const float trail = TrailFraction(enemy, now);
    if (trail > fraction) {
        h->fillRect(barX0, barY, barX0 + (barX1 - barX0) * trail, barY + barH, kTrailColor);
    }
    if (fraction > 0.0f) {
        h->fillRect(barX0, barY, barX0 + (barX1 - barX0) * fraction, barY + barH, color);
    }

    const float smallSize = 11.0f * k;
    const float bottomY = y0 + 37.0f * k;
    const std::string amounts = WithThousands(enemy.hp > 0.0f ? enemy.hp : 0.0f) + " / " + WithThousands(enemy.maxHp);
    h->textEx(barX0, bottomY, smallSize, kSoftColor, EDF6_FONT_MONO, 0.0f, amounts.c_str());
    if (defeated) {
        DrawRightText(h, right, bottomY, smallSize, kMutedColor, EDF6_FONT_MONO, KillSummary(enemy));
    } else if (enemy.lastPlayerHit > 0.0f) {
        DrawRightText(h, right, bottomY, smallSize, kMutedColor, EDF6_FONT_MONO, "-" + WithThousands(enemy.lastPlayerHit));
    }
}

void DrawDetailed(const Edf6OverlayHost *h, const TrackedEnemy &enemy, float x0, float y0, float width, float k,
                  ULONGLONG now) {
    const float height = 96.0f * k;
    const float pad = 16.0f * k;
    const float right = x0 + width - pad;
    DrawCard(h, x0, y0, x0 + width, y0 + height, k);

    const bool defeated = enemy.hp <= 0.0f;
    const float fraction = HpFraction(enemy);
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
    if (defeated) {
        const std::string summary = KillSummary(enemy);
        DrawRightText(h, right - bigW - 18.0f * k, smallY, smallSize, kMutedColor, EDF6_FONT_MONO, summary);
    } else if (dps > 0.0f) {
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
    const float trail = TrailFraction(enemy, now);
    for (int i = 0; i < kSegments; i++) {
        const float sx = x0 + pad + i * (segmentW + gap);
        h->fillRect(sx, barY, sx + segmentW, barY + barH, kSegmentEmpty);
        const float trailFill = Clamp01(trail * kSegments - i);
        if (trailFill > 0.0f) {
            h->fillRect(sx, barY, sx + segmentW * trailFill, barY + barH, kTrailColor);
        }
        const float fill = Clamp01(fraction * kSegments - i);
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

bool DrawsFirst(const TrackedEnemy *a, const TrackedEnemy *b) {
    const bool aAlive = a->hp > 0.0f;
    const bool bAlive = b->hp > 0.0f;
    if (aAlive != bAlive) {
        return aAlive;
    }
    return a->lastUpdate > b->lastUpdate;
}

void Draw(const Edf6OverlayHost *h) {
    if (!counterEnabled) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    static TrackedEnemy visible[kMaxTracked];
    const int count = VisibleEnemies(visible, now);
    if (count == 0) {
        return;
    }
    const TrackedEnemy *order[kMaxTracked];
    for (int i = 0; i < count; i++) {
        order[i] = &visible[i];
    }
    std::sort(order, order + count, DrawsFirst);

    const float k = h->scale() * kDesignScale;
    float screenW = 0.0f;
    float screenH = 0.0f;
    h->screenSize(&screenW, &screenH);
    const float width = 560.0f * k;
    const float x0 = (screenW - width) * 0.5f;
    float y = 40.0f * k;

    const TrackedEnemy *detailed = nullptr;
    for (int i = 0; i < count && !detailed; i++) {
        if (order[i]->exceptional) {
            detailed = order[i];
        }
    }
    if (detailed) {
        DrawDetailed(h, *detailed, x0, y, width, k, now);
        y += (96.0f + 8.0f) * k;
    }

    const float gap = 8.0f * k;
    const float columnW = (width - gap) * 0.5f;
    int drawn = 0;
    for (int i = 0; i < count && drawn < kMaxCompact; i++) {
        if (order[i] == detailed) {
            continue;
        }
        const float cx = x0 + (drawn % 2) * (columnW + gap);
        const float cy = y + (drawn / 2) * (56.0f * k + gap);
        DrawCompact(h, *order[i], cx, cy, columnW, k, now);
        drawn++;
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
