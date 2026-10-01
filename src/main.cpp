#include <Geode/Geode.hpp>
#include <Geode/loader/Hook.hpp>
#include <Geode/modify/CCDirector.hpp>

#include "RefreshRate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

#if defined(GEODE_IS_WINDOWS)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

using namespace geode::prelude;

namespace {
    bool g_enabled = true;
    bool g_fastFormatEnabled = true;
    bool g_renderDividerEnabled = true;
    bool g_overrideVisualFPS = false;
    bool g_debuggerEnabled = false;
    int g_visualFPS = 60;

    geode::Hook* g_fastFormatHook = nullptr;
    bool g_fastFormatInstalled = false;

    double g_drawAccumulator = 0.0;
    double g_debugElapsed = 0.0;

    std::uint64_t g_logicCalls = 0;
    std::uint64_t g_renderCalls = 0;
    std::uint64_t g_skippedDraws = 0;
    std::uint64_t g_formatCalls = 0;

    bool isCBFLoaded() {
        return Loader::get()->isModLoaded("syzzi.click_between_frames");
    }

    double currentTargetFPS() {
        double target = g_overrideVisualFPS
            ? static_cast<double>(g_visualFPS)
            : dashboost::platformRefreshRate();

        if (!std::isfinite(target) || target < 1.0) {
            target = 60.0;
        }

        return std::clamp(target, 1.0, 360.0);
    }

    bool renderDividerActive() {
        return g_enabled && g_renderDividerEnabled && !isCBFLoaded();
    }

    void resetDrawState() {
        g_drawAccumulator = 0.0;
    }

    bool assignFormatted(cocos2d::CCString* self, char const* format, va_list args) {
        if (!self || !format) {
            return false;
        }

        std::array<char, 512> stackBuffer{};

        va_list firstPass;
        va_copy(firstPass, args);
        int required = std::vsnprintf(
            stackBuffer.data(),
            stackBuffer.size(),
            format,
            firstPass
        );
        va_end(firstPass);

        if (required < 0) {
            return false;
        }

        if (static_cast<std::size_t>(required) < stackBuffer.size()) {
            self->m_sString = stackBuffer.data();
            return true;
        }

        std::string dynamicBuffer(static_cast<std::size_t>(required) + 1, '\0');

        va_list secondPass;
        va_copy(secondPass, args);
        int written = std::vsnprintf(
            dynamicBuffer.data(),
            dynamicBuffer.size(),
            format,
            secondPass
        );
        va_end(secondPass);

        if (written < 0) {
            return false;
        }

        self->m_sString = dynamicBuffer.c_str();
        return true;
    }

    cocos2d::CCString* fastCreateWithFormat(char const* format, ...) {
        auto* result = cocos2d::CCString::create("");

        va_list args;
        va_start(args, format);
        bool formatted = assignFormatted(result, format, args);
        va_end(args);

        ++g_formatCalls;

        if (!formatted) {
            result->m_sString = "";
        }

        return result;
    }

    void setFastFormatHookState() {
        if (!g_fastFormatHook) {
            return;
        }

        bool shouldEnable = g_enabled && g_fastFormatEnabled;
        auto result = g_fastFormatHook->toggle(shouldEnable);

        if (result.isErr() && g_debuggerEnabled) {
            log::warn("[DashBoost] Failed to toggle Fast Format hook");
        }
    }

    void installFastFormatHook() {
#if defined(GEODE_IS_WINDOWS)
        HMODULE cocos = GetModuleHandleW(L"libcocos2d.dll");
        if (!cocos) {
            log::warn("[DashBoost] libcocos2d.dll was not found; Fast Format unavailable");
            return;
        }

        auto address = reinterpret_cast<void*>(
            GetProcAddress(
                cocos,
                "?createWithFormat@CCString@cocos2d@@SAPEAV12@PEBDZZ"
            )
        );

        if (!address) {
            log::warn("[DashBoost] CCString::createWithFormat export was not found; Fast Format unavailable");
            return;
        }

        auto hook = Mod::get()->hook(
            address,
            &fastCreateWithFormat,
            "cocos2d::CCString::createWithFormat",
            tulip::hook::TulipConvention::Cdecl
        );

        if (hook.isOk()) {
            g_fastFormatHook = hook.unwrap();
            g_fastFormatInstalled = true;
        }
        else {
            log::warn("[DashBoost] Failed to install Fast Format hook");
        }

#elif defined(GEODE_IS_IOS)
        auto hook = GEODE_MOD_STATIC_HOOK(
            0x2680c0,
            &fastCreateWithFormat,
            cocos2d::CCString::createWithFormat
        );

        if (hook.isOk()) {
            g_fastFormatHook = hook.unwrap();
            g_fastFormatInstalled = true;
        }
        else {
            log::warn("[DashBoost] Failed to install iOS Fast Format hook");
        }
#endif
    }

    void readSettings() {
        auto* mod = Mod::get();

        g_enabled = mod->getSettingValue<bool>("enabled");
        g_fastFormatEnabled = mod->getSettingValue<bool>("fast-format");
        g_renderDividerEnabled = mod->getSettingValue<bool>("render-divider");
        g_overrideVisualFPS = mod->getSettingValue<bool>("override-visual-fps");
        g_debuggerEnabled = mod->getSettingValue<bool>("debugger");
        g_visualFPS = static_cast<int>(
            mod->getSettingValue<std::int64_t>("visual-fps")
        );
    }

    void resetDebugCounters() {
        g_debugElapsed = 0.0;
        g_logicCalls = 0;
        g_renderCalls = 0;
        g_skippedDraws = 0;
        g_formatCalls = 0;
    }

    void logStatus(char const* reason) {
        if (!g_debuggerEnabled) {
            return;
        }

#if defined(GEODE_IS_IOS)
        bool patchless = Loader::get()->isPatchless();
#else
        bool patchless = false;
#endif

        log::info(
            "[DashBoost] {} | enabled={} fast-format={} hook={} render-divider={} target={}Hz cbf={} patchless={}",
            reason,
            g_enabled,
            g_fastFormatEnabled,
            g_fastFormatInstalled && g_fastFormatHook && g_fastFormatHook->isEnabled(),
            g_renderDividerEnabled,
            currentTargetFPS(),
            isCBFLoaded(),
            patchless
        );
    }

    void debugTick(double delta) {
        if (!g_debuggerEnabled) {
            return;
        }

        if (std::isfinite(delta) && delta > 0.0) {
            g_debugElapsed += delta;
        }

        if (g_debugElapsed < 1.0) {
            return;
        }

#if defined(GEODE_IS_IOS)
        bool patchless = Loader::get()->isPatchless();
#else
        bool patchless = false;
#endif

        log::info(
            "[DashBoost] 1s | logic={} rendered={} skipped={} format={} target={}Hz divider-active={} cbf={} patchless={}",
            g_logicCalls,
            g_renderCalls,
            g_skippedDraws,
            g_formatCalls,
            currentTargetFPS(),
            renderDividerActive(),
            isCBFLoaded(),
            patchless
        );

        g_debugElapsed = std::fmod(g_debugElapsed, 1.0);
        g_logicCalls = 0;
        g_renderCalls = 0;
        g_skippedDraws = 0;
        g_formatCalls = 0;
    }
}

class $modify(DashBoostDirector, cocos2d::CCDirector) {
    void drawScene() {
        double actualDelta = this->getActualDeltaTime();
        ++g_logicCalls;

        static bool wasDividerActive = false;
        bool dividerActive = renderDividerActive();

        if (dividerActive != wasDividerActive) {
            resetDrawState();
            wasDividerActive = dividerActive;
        }

        if (!dividerActive || this->getTotalFrames() < 150) {
            ++g_renderCalls;
            cocos2d::CCDirector::drawScene();
            debugTick(actualDelta);
            return;
        }

        double targetDelta = 1.0 / currentTargetFPS();

        if (std::isfinite(actualDelta) && actualDelta > 0.0) {
            g_drawAccumulator += actualDelta;
        }

        if (g_drawAccumulator + 1e-9 >= targetDelta) {
            g_drawAccumulator = std::fmod(g_drawAccumulator, targetDelta);
            ++g_renderCalls;
            cocos2d::CCDirector::drawScene();
            debugTick(actualDelta);
            return;
        }

        ++g_skippedDraws;

        if (!this->isPaused()) {
            this->getScheduler()->update(this->getDeltaTime());
        }

        if (this->getNextScene()) {
            this->setNextScene();
        }

        debugTick(actualDelta);
    }
};

$on_mod(Loaded) {
    readSettings();
    installFastFormatHook();
    setFastFormatHookState();

    listenForSettingChanges<bool>("enabled", [](bool value) {
        g_enabled = value;
        resetDrawState();
        setFastFormatHookState();
        logStatus(value ? "master enabled" : "master disabled");
    });

    listenForSettingChanges<bool>("fast-format", [](bool value) {
        g_fastFormatEnabled = value;
        setFastFormatHookState();
        logStatus(value ? "Fast Format enabled" : "Fast Format disabled");
    });

    listenForSettingChanges<bool>("render-divider", [](bool value) {
        g_renderDividerEnabled = value;
        resetDrawState();
        logStatus(value ? "Render Divider enabled" : "Render Divider disabled");
    });

    listenForSettingChanges<bool>("override-visual-fps", [](bool value) {
        g_overrideVisualFPS = value;
        resetDrawState();
        logStatus(value ? "visual FPS override enabled" : "visual FPS override disabled");
    });

    listenForSettingChanges<std::int64_t>("visual-fps", [](std::int64_t value) {
        g_visualFPS = static_cast<int>(value);
        resetDrawState();
        logStatus("visual FPS changed");
    });

    listenForSettingChanges<bool>("debugger", [](bool value) {
        g_debuggerEnabled = value;
        resetDebugCounters();
        if (value) {
            logStatus("debugger enabled");
        }
    });

    logStatus("loaded");
}
