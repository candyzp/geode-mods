#include <Geode/Geode.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GJGameLevel.hpp>

#include "RefreshRate.hpp"
#include "PlatformDiagnostics.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

using namespace geode::prelude;

namespace {
    using Clock = std::chrono::steady_clock;

    constexpr std::size_t kFrameHistory = 120;
    constexpr std::size_t kInputHistory = 64;
    constexpr std::size_t kTimelineFrames = 10;
    constexpr double kInputCorrelationWindowMs = 150.0;

    bool g_enabled = true;
    bool g_showOverlay = true;
    bool g_includePlatformSnapshot = true;
    bool g_verboseTimeline = true;
    int g_spikeThresholdMs = 20;
    int g_reportCooldownMs = 300;

    struct FrameSample {
        std::uint64_t frame = 0;
        double rawGapMs = 0.0;
        double adjustedGapMs = 0.0;
        double priorDiagnosticMs = 0.0;
        double drawMs = 0.0;
        double engineDeltaMs = 0.0;
        bool gapSpike = false;
        bool drawSpike = false;
    };

    struct InputEvent {
        double timeSeconds = 0.0;
        int button = -1;
        bool down = false;
        bool player2 = false;
        double hookMs = 0.0;
    };

    struct FrameStats {
        std::size_t samples = 0;
        double averageGapMs = 0.0;
        double averageDrawMs = 0.0;
        double p50GapMs = 0.0;
        double p95GapMs = 0.0;
        double p99GapMs = 0.0;
        double maxGapMs = 0.0;
        double maxDrawMs = 0.0;
        double maxEngineDeltaMs = 0.0;
        std::size_t spikes = 0;
    };

    struct InputStats {
        int events250ms = 0;
        int events1s = 0;
        int presses1s = 0;
        int releases1s = 0;
        double lastEventAgeMs = -1.0;
        double lastPressAgeMs = -1.0;
        double lastHookMs = 0.0;
        double maxHook1sMs = 0.0;
        int lastButton = -1;
        bool lastDown = false;
        bool lastPlayer2 = false;
    };

    std::array<FrameSample, kFrameHistory> g_frames{};
    std::size_t g_frameHead = 0;
    std::size_t g_frameCount = 0;

    std::array<InputEvent, kInputHistory> g_inputs{};
    std::size_t g_inputHead = 0;
    std::size_t g_inputCount = 0;

    Clock::time_point g_previousFrameStart{};
    bool g_havePreviousFrame = false;
    cocos2d::CCScene* g_previousScene = nullptr;

    std::uint64_t g_levelFrame = 0;
    std::uint64_t g_totalSpikes = 0;
    double g_levelStartedAt = 0.0;
    double g_lastDetailedReportAt = -10000.0;
    double g_previousCaptureCostMs = 0.0;
    std::uint64_t g_coalescedReports = 0;
    std::int64_t g_levelId = 0;

    CCLabelBMFont* g_overlayLabel = nullptr;
    double g_overlayExpiresAt = 0.0;

    double secondsSinceBoot(Clock::time_point point = Clock::now()) {
        return std::chrono::duration<double>(point.time_since_epoch()).count();
    }

    double milliseconds(Clock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    }

    double displayRefreshRate() {
        double rate = dashboost::platformRefreshRate();
        if (!std::isfinite(rate) || rate < 1.0) {
            rate = 60.0;
        }
        return std::clamp(rate, 1.0, 360.0);
    }

    double frameBudgetMs() {
        return 1000.0 / displayRefreshRate();
    }

    const char* thermalName(int state) {
        switch (state) {
            case 0: return "Nominal";
            case 1: return "Fair";
            case 2: return "Serious";
            case 3: return "Critical";
            default: return "Unknown";
        }
    }

    void resetLevelCapture() {
        g_frames = {};
        g_inputs = {};
        g_frameHead = 0;
        g_frameCount = 0;
        g_inputHead = 0;
        g_inputCount = 0;
        g_previousFrameStart = {};
        g_havePreviousFrame = false;
        g_previousScene = nullptr;
        g_levelFrame = 0;
        g_totalSpikes = 0;
        g_levelStartedAt = secondsSinceBoot();
        g_lastDetailedReportAt = -10000.0;
        g_previousCaptureCostMs = 0.0;
        g_coalescedReports = 0;
    }

    void recordInput(
        double timestamp,
        int button,
        bool down,
        bool player2,
        double hookMs
    ) {
        if (!g_enabled) {
            return;
        }

        g_inputs[g_inputHead] = {
            timestamp,
            button,
            down,
            player2,
            hookMs
        };

        g_inputHead = (g_inputHead + 1) % kInputHistory;
        g_inputCount = std::min(g_inputCount + 1, kInputHistory);
    }

    InputStats collectInputStats(double nowSeconds) {
        InputStats stats{};

        for (std::size_t offset = 0; offset < g_inputCount; ++offset) {
            const std::size_t index =
                (g_inputHead + kInputHistory - g_inputCount + offset) %
                kInputHistory;
            const auto& event = g_inputs[index];
            const double ageMs = (nowSeconds - event.timeSeconds) * 1000.0;

            if (ageMs < 0.0) {
                continue;
            }

            if (stats.lastEventAgeMs < 0.0 || ageMs < stats.lastEventAgeMs) {
                stats.lastEventAgeMs = ageMs;
                stats.lastButton = event.button;
                stats.lastDown = event.down;
                stats.lastPlayer2 = event.player2;
                stats.lastHookMs = event.hookMs;
            }

            if (event.down &&
                (stats.lastPressAgeMs < 0.0 || ageMs < stats.lastPressAgeMs)) {
                stats.lastPressAgeMs = ageMs;
            }

            if (ageMs <= 250.0) {
                ++stats.events250ms;
            }

            if (ageMs <= 1000.0) {
                ++stats.events1s;
                if (event.down) {
                    ++stats.presses1s;
                }
                else {
                    ++stats.releases1s;
                }
                stats.maxHook1sMs = std::max(stats.maxHook1sMs, event.hookMs);
            }
        }

        return stats;
    }

    void recordFrame(const FrameSample& sample) {
        g_frames[g_frameHead] = sample;
        g_frameHead = (g_frameHead + 1) % kFrameHistory;
        g_frameCount = std::min(g_frameCount + 1, kFrameHistory);
    }

    double percentile(std::array<double, kFrameHistory> values,
                      std::size_t count,
                      double fraction) {
        if (count == 0) {
            return 0.0;
        }

        std::sort(values.begin(), values.begin() + count);
        const auto index = static_cast<std::size_t>(
            std::ceil(static_cast<double>(count - 1) * fraction)
        );
        return values[std::min(index, count - 1)];
    }

    FrameStats collectFrameStats() {
        FrameStats stats{};
        std::array<double, kFrameHistory> gaps{};
        std::size_t gapCount = 0;
        double gapSum = 0.0;
        double drawSum = 0.0;

        for (std::size_t offset = 0; offset < g_frameCount; ++offset) {
            const std::size_t index =
                (g_frameHead + kFrameHistory - g_frameCount + offset) %
                kFrameHistory;
            const auto& sample = g_frames[index];

            if (sample.rawGapMs > 0.0) {
                gaps[gapCount++] = sample.rawGapMs;
                gapSum += sample.rawGapMs;
                drawSum += sample.drawMs;
                ++stats.samples;
                stats.maxGapMs = std::max(stats.maxGapMs, sample.rawGapMs);
                stats.maxDrawMs = std::max(stats.maxDrawMs, sample.drawMs);
                stats.maxEngineDeltaMs =
                    std::max(stats.maxEngineDeltaMs, sample.engineDeltaMs);
            }

            if (sample.gapSpike || sample.drawSpike) {
                ++stats.spikes;
            }
        }

        if (stats.samples > 0) {
            stats.averageGapMs = gapSum / static_cast<double>(stats.samples);
            stats.averageDrawMs = drawSum / static_cast<double>(stats.samples);
            stats.p50GapMs = percentile(gaps, gapCount, 0.50);
            stats.p95GapMs = percentile(gaps, gapCount, 0.95);
            stats.p99GapMs = percentile(gaps, gapCount, 0.99);
        }

        return stats;
    }

    std::string buildTimeline() {
        std::string timeline;
        const std::size_t count = std::min(g_frameCount, kTimelineFrames);

        for (std::size_t offset = 0; offset < count; ++offset) {
            const std::size_t index =
                (g_frameHead + kFrameHistory - count + offset) %
                kFrameHistory;
            const auto& sample = g_frames[index];

            if (!timeline.empty()) {
                timeline += " | ";
            }

            timeline += fmt::format(
                "F{} raw={:.2f} adj={:.2f} diag={:.2f} draw={:.2f} dt={:.2f}{}",
                sample.frame,
                sample.rawGapMs,
                sample.adjustedGapMs,
                sample.priorDiagnosticMs,
                sample.drawMs,
                sample.engineDeltaMs,
                (sample.gapSpike || sample.drawSpike) ? " SPIKE" : ""
            );
        }

        return timeline.empty() ? "not enough frames captured" : timeline;
    }

    std::string buildInputTrail(double nowSeconds) {
        std::string trail;
        const std::size_t count = std::min<std::size_t>(g_inputCount, 8);

        for (std::size_t offset = 0; offset < count; ++offset) {
            const std::size_t index =
                (g_inputHead + kInputHistory - count + offset) %
                kInputHistory;
            const auto& event = g_inputs[index];
            const double ageMs = (nowSeconds - event.timeSeconds) * 1000.0;

            if (!trail.empty()) {
                trail += " | ";
            }

            trail += fmt::format(
                "{}B{}-P{} age={:.1f}ms hook={:.3f}ms",
                event.down ? "DOWN" : "UP",
                event.button,
                event.player2 ? 2 : 1,
                ageMs,
                event.hookMs
            );
        }

        return trail.empty() ? "no player input hooks observed yet" : trail;
    }

    std::string classifySpike(
        const FrameSample& sample,
        const InputStats& input,
        const geode::culpritfinder::PlatformSnapshot& platform,
        bool platformCaptured
    ) {
        const double budget = frameBudgetMs();
        std::string result;

        if (sample.priorDiagnosticMs >= 1.0 &&
            sample.rawGapMs >= static_cast<double>(g_spikeThresholdMs) &&
            sample.adjustedGapMs < static_cast<double>(g_spikeThresholdMs)) {
            result =
                "DIAGNOSTIC SELF-OVERHEAD: the previous Culprit Finder report "
                "consumed a measurable part of this frame gap. Compare raw and "
                "adjusted gaps; this follow-up hitch may be partly caused by "
                "report formatting, logging, or platform sampling.";
        }
        else if (sample.drawMs >= budget * 0.80) {
            result =
                "DRAW PATH: drawScene itself consumed most of the frame budget. "
                "Possible scene traversal, render submission, or driver/vsync wait.";
        }
        else if (sample.adjustedGapMs >= static_cast<double>(g_spikeThresholdMs) &&
                 sample.drawMs < budget * 0.40) {
            result =
                "PRE-DRAW STALL: drawScene was relatively short, so the delay "
                "likely happened before it. Possible game update/scheduler work, "
                "another mod or hook, or OS thread scheduling.";
        }
        else if (sample.drawMs >= budget * 0.55) {
            result =
                "MIXED FRAME: both frame pacing and drawScene work were elevated. "
                "Investigate rendering plus work scheduled earlier in the frame.";
        }
        else {
            result =
                "SCHEDULING / UPDATE SUSPECT: the frame arrived late without a "
                "long drawScene call. This hook cannot identify the exact subsystem.";
        }

        if (input.lastEventAgeMs >= 0.0 &&
            input.lastEventAgeMs <= kInputCorrelationWindowMs) {
            result += fmt::format(
                "\nINPUT CORRELATION: PlayerObject {} button {} for P{} was "
                "{:.1f}ms before the spike. This is timing correlation, not proof "
                "that the input caused it.",
                input.lastDown ? "press" : "release",
                input.lastButton,
                input.lastPlayer2 ? 2 : 1,
                input.lastEventAgeMs
            );
        }
        else {
            result += "\nINPUT CORRELATION: no PlayerObject press/release was "
                      "observed in the last 150ms.";
        }

        if (input.maxHook1sMs >= 1.0) {
            result += fmt::format(
                "\nINPUT HOOK WARNING: the PlayerObject button hook chain reached "
                "{:.3f}ms within the last second. This includes downstream hooks "
                "and is worth investigating.",
                input.maxHook1sMs
            );
        }

        if (sample.engineDeltaMs >= budget * 1.50) {
            result += fmt::format(
                "\nENGINE DELTA: getDeltaTime() jumped to {:.2f}ms, supporting a "
                "late-update / scheduling stall.",
                sample.engineDeltaMs
            );
        }

        if (platformCaptured && platform.thermalState >= 2) {
            result += fmt::format(
                "\nTHERMAL CLUE: iOS reports {} thermal state. This can coincide "
                "with throttling, but does not prove thermal throttling caused "
                "this individual spike.",
                thermalName(platform.thermalState)
            );
        }

        if (platformCaptured && platform.memoryValid &&
            platform.physicalMemoryBytes > 0) {
            const double residentRatio =
                static_cast<double>(platform.residentBytes) /
                static_cast<double>(platform.physicalMemoryBytes);

            if (residentRatio >= 0.50) {
                result += fmt::format(
                    "\nMEMORY CLUE: resident memory is {:.1f}% of physical "
                    "device memory. This is a snapshot, not an allocation trace "
                    "or proof of memory pressure.",
                    residentRatio * 100.0
                );
            }
        }

        if (sample.rawGapMs >= 2000.0) {
            result += "\nLONG GAP: possible app suspension, OS interruption, or "
                      "severe main-thread hang.";
        }

        return result;
    }

    std::string makeReport(
        const FrameSample& sample,
        double nowSeconds,
        const FrameStats& frameStats,
        const InputStats& inputStats,
        const geode::culpritfinder::PlatformSnapshot& platform,
        bool platformCaptured
    ) {
        const double budget = frameBudgetMs();
        const double fps =
            sample.rawGapMs > 0.0 ? 1000.0 / sample.rawGapMs : 0.0;
        const double severity =
            budget > 0.0 ? sample.rawGapMs / budget : 0.0;
        const double levelElapsed =
            std::max(0.0, nowSeconds - g_levelStartedAt);
        const std::uintptr_t sceneAddress =
            reinterpret_cast<std::uintptr_t>(g_previousScene);

        std::string report = fmt::format(
            "[CulpritFinder] SPIKE #{}  ========================================\n"
            "LEVEL: id={} | elapsed={:.2f}s | level-frame={}\n"
            "SCENE: 0x{:x} | paused=false | display={:.1f}Hz | frame-budget={:.3f}ms\n"
            "TRIGGER: raw-gap={:.3f}ms | gap-minus-prior-diagnostics={:.3f}ms | "
            "prior-diagnostic-cost={:.3f}ms | drawScene-wall={:.3f}ms | "
            "engine-delta={:.3f}ms\n"
            "FRAME: instant-fps={:.2f} | raw-gap/budget={:.2f}x | "
            "threshold={}ms | gap-spike={} | draw-spike={}\n"
            "ROLLING {} FRAMES: avg-gap={:.3f}ms | p50={:.3f}ms | p95={:.3f}ms | "
            "p99={:.3f}ms | max-gap={:.3f}ms | avg-draw={:.3f}ms | "
            "max-draw={:.3f}ms | max-engine-delta={:.3f}ms | spikes-in-window={}\n"
            "INPUT: events-250ms={} | events-1s={} | presses-1s={} | releases-1s={} | "
            "last-event-age={:.2f}ms | last-press-age={:.2f}ms | "
            "last-hook={:.3f}ms | max-hook-1s={:.3f}ms\n"
            "POSSIBLE REASONS:\n{}\n"
            "INPUT TRAIL: {}\n"
            "FRAME TIMELINE: {}\n"
            "PLATFORM: captured={}",
            g_totalSpikes,
            g_levelId,
            levelElapsed,
            g_levelFrame,
            sceneAddress,
            displayRefreshRate(),
            budget,
            sample.rawGapMs,
            sample.adjustedGapMs,
            sample.priorDiagnosticMs,
            sample.drawMs,
            sample.engineDeltaMs,
            fps,
            severity,
            g_spikeThresholdMs,
            sample.gapSpike,
            sample.drawSpike,
            frameStats.samples,
            frameStats.averageGapMs,
            frameStats.p50GapMs,
            frameStats.p95GapMs,
            frameStats.p99GapMs,
            frameStats.maxGapMs,
            frameStats.averageDrawMs,
            frameStats.maxDrawMs,
            frameStats.maxEngineDeltaMs,
            frameStats.spikes,
            inputStats.events250ms,
            inputStats.events1s,
            inputStats.presses1s,
            inputStats.releases1s,
            inputStats.lastEventAgeMs,
            inputStats.lastPressAgeMs,
            inputStats.lastHookMs,
            inputStats.maxHook1sMs,
            classifySpike(sample, inputStats, platform, platformCaptured),
            buildInputTrail(nowSeconds),
            g_verboseTimeline ? buildTimeline() : "disabled in settings",
            platformCaptured
        );

        if (platformCaptured) {
            report += fmt::format(
                "\nMEMORY: valid={} | resident={:.1f}MiB | virtual={:.1f}MiB | "
                "physical-device-memory={:.1f}MiB\n"
                "THERMAL: {} | active-processors={}",
                platform.memoryValid,
                static_cast<double>(platform.residentBytes) / (1024.0 * 1024.0),
                static_cast<double>(platform.virtualBytes) / (1024.0 * 1024.0),
                static_cast<double>(platform.physicalMemoryBytes) / (1024.0 * 1024.0),
                thermalName(platform.thermalState),
                platform.activeProcessorCount
            );
        }
        else {
            report += "\nMEMORY / THERMAL: snapshot skipped during log cooldown or "
                      "disabled in settings.";
        }

        report +=
            "\nLIMITS: CPU wall-clock timings only. This cannot directly measure GPU "
            "execution time, identify a specific third-party mod, or trace individual "
            "heap allocations. drawScene timing includes work performed by hooks "
            "around that method.\n"
            "==============================================================";

        return report;
    }

    void updateOverlay(
        const FrameSample& sample,
        const InputStats& input,
        const geode::culpritfinder::PlatformSnapshot& platform,
        bool platformCaptured,
        double nowSeconds
    ) {
        if (!g_overlayLabel || !g_showOverlay) {
            return;
        }

        const double budget = frameBudgetMs();
        const std::string primary =
            sample.priorDiagnosticMs >= 1.0 &&
            sample.rawGapMs >= static_cast<double>(g_spikeThresholdMs) &&
            sample.adjustedGapMs < static_cast<double>(g_spikeThresholdMs)
                ? "LIKELY: DIAGNOSTIC SELF-OVERHEAD"
                : (sample.drawMs >= budget * 0.80
                    ? "LIKELY: DRAW / RENDER PATH"
                    : "LIKELY: PRE-DRAW / UPDATE STALL");

        std::string inputLine = "input: none in 150ms";
        if (input.lastEventAgeMs >= 0.0 &&
            input.lastEventAgeMs <= kInputCorrelationWindowMs) {
            inputLine = fmt::format(
                "input: {} B{} P{} {:.0f}ms ago",
                input.lastDown ? "press" : "release",
                input.lastButton,
                input.lastPlayer2 ? 2 : 1,
                input.lastEventAgeMs
            );
        }

        std::string memoryLine = "platform snapshot skipped";
        if (platformCaptured && platform.memoryValid) {
            memoryLine = fmt::format(
                "RAM {:.0f}MiB | thermal {}",
                static_cast<double>(platform.residentBytes) / (1024.0 * 1024.0),
                thermalName(platform.thermalState)
            );
        }

        const std::string overlay = fmt::format(
            "CULPRIT FINDER - SPIKE #{}\n"
            "gap {:.1f}ms (adjusted {:.1f}) / budget {:.1f}ms\n"
            "drawScene {:.1f}ms | prior diagnostic {:.1f}ms\n"
            "{}\n{}\n{}\nFull report: Geode log",
            g_totalSpikes,
            sample.rawGapMs,
            sample.adjustedGapMs,
            budget,
            sample.drawMs,
            sample.priorDiagnosticMs,
            primary,
            inputLine,
            memoryLine
        );

        g_overlayLabel->setString(overlay.c_str());
        g_overlayLabel->setVisible(true);
        g_overlayExpiresAt = nowSeconds + 6.0;
    }

    void captureSpike(const FrameSample& sample, double nowSeconds) {
        ++g_totalSpikes;

        const auto frameStats = collectFrameStats();
        const auto inputStats = collectInputStats(nowSeconds);

        const double cooldownSeconds =
            static_cast<double>(std::max(g_reportCooldownMs, 0)) / 1000.0;
        const bool detailedReportDue =
            nowSeconds - g_lastDetailedReportAt >= cooldownSeconds;

        geode::culpritfinder::PlatformSnapshot platform{};
        const bool platformCaptured =
            g_includePlatformSnapshot && detailedReportDue;

        if (platformCaptured) {
            platform = geode::culpritfinder::collectPlatformSnapshot();
        }

        updateOverlay(
            sample,
            inputStats,
            platform,
            platformCaptured,
            nowSeconds
        );

        if (!detailedReportDue) {
            ++g_coalescedReports;
            log::warn(
                "[CulpritFinder] spike #{}: raw-gap={:.2f}ms adjusted={:.2f}ms "
                "draw={:.2f}ms; detailed report coalesced ({} since last report)",
                g_totalSpikes,
                sample.rawGapMs,
                sample.adjustedGapMs,
                sample.drawMs,
                g_coalescedReports
            );
            return;
        }

        g_lastDetailedReportAt = nowSeconds;
        std::string report = makeReport(
            sample,
            nowSeconds,
            frameStats,
            inputStats,
            platform,
            platformCaptured
        );

        if (g_coalescedReports > 0) {
            report += fmt::format(
                "\nNOTE: {} additional spike event(s) were coalesced during the "
                "log cooldown. They remain represented in the rolling frame history.",
                g_coalescedReports
            );
            g_coalescedReports = 0;
        }

        log::warn("{}", report);
    }

    void attachOverlay(PlayLayer* layer) {
        g_overlayLabel = nullptr;

        if (!layer) {
            return;
        }

        auto* label = CCLabelBMFont::create(
            "CULPRIT FINDER\nMonitoring frame pacing...",
            "chatFont.fnt"
        );

        if (!label) {
            log::warn("[CulpritFinder] Could not create the in-game diagnostic label");
            return;
        }

        const auto winSize = CCDirector::sharedDirector()->getWinSize();
        label->setAnchorPoint({0.0f, 1.0f});
        label->setPosition({12.0f, winSize.height - 18.0f});
        label->setScale(0.48f);
        label->setVisible(false);
        layer->addChild(label, 9999);

        g_overlayLabel = label;
    }

    void readSettings() {
        auto* mod = Mod::get();

        g_enabled = mod->getSettingValue<bool>("enabled");
        g_showOverlay = mod->getSettingValue<bool>("show-overlay");
        g_includePlatformSnapshot =
            mod->getSettingValue<bool>("platform-snapshot");
        g_verboseTimeline = mod->getSettingValue<bool>("frame-timeline");

        g_spikeThresholdMs = static_cast<int>(
            mod->getSettingValue<std::int64_t>("spike-threshold-ms")
        );
        g_reportCooldownMs = static_cast<int>(
            mod->getSettingValue<std::int64_t>("report-cooldown-ms")
        );

        g_spikeThresholdMs = std::clamp(g_spikeThresholdMs, 17, 80);
        g_reportCooldownMs = std::clamp(g_reportCooldownMs, 0, 2000);
    }
}

// PlayerObject calls can also be synthesized by other mods, so these records
// represent the player-input path, not guaranteed physical touchscreen input.
class $modify(CulpritFinderPlayer, PlayerObject) {
    bool pushButton(PlayerButton button) {
        const auto started = Clock::now();
        const double timestamp = secondsSinceBoot(started);

        auto* playLayer = PlayLayer::get();
        const bool isPlayer1 = playLayer && this == playLayer->m_player1;
        const bool isPlayer2 = playLayer && this == playLayer->m_player2;
        const bool isTrackedPlayer = isPlayer1 || isPlayer2;

        const bool result = PlayerObject::pushButton(button);

        if (isTrackedPlayer) {
            recordInput(
                timestamp,
                static_cast<int>(button),
                true,
                isPlayer2,
                milliseconds(Clock::now() - started)
            );
        }

        return result;
    }

    bool releaseButton(PlayerButton button) {
        const auto started = Clock::now();
        const double timestamp = secondsSinceBoot(started);

        auto* playLayer = PlayLayer::get();
        const bool isPlayer1 = playLayer && this == playLayer->m_player1;
        const bool isPlayer2 = playLayer && this == playLayer->m_player2;
        const bool isTrackedPlayer = isPlayer1 || isPlayer2;

        const bool result = PlayerObject::releaseButton(button);

        if (isTrackedPlayer) {
            recordInput(
                timestamp,
                static_cast<int>(button),
                false,
                isPlayer2,
                milliseconds(Clock::now() - started)
            );
        }

        return result;
    }
};

class $modify(CulpritFinderPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        resetLevelCapture();
        g_levelId = level
            ? static_cast<std::int64_t>(level->m_levelID)
            : 0;

        attachOverlay(this);

        log::info(
            "[CulpritFinder] monitoring started | level-id={} | target={:.1f}Hz | "
            "threshold={}ms | overlay={}",
            g_levelId,
            displayRefreshRate(),
            g_spikeThresholdMs,
            g_showOverlay
        );

        return true;
    }

    void onQuit() {
        g_overlayLabel = nullptr;
        g_havePreviousFrame = false;
        g_previousScene = nullptr;
        PlayLayer::onQuit();
    }
};

class $modify(CulpritFinderDirector, cocos2d::CCDirector) {
    void drawScene() {
        if (!g_enabled) {
            CCDirector::drawScene();
            return;
        }

        auto* playLayer = PlayLayer::get();
        if (!playLayer || this->isPaused()) {
            g_havePreviousFrame = false;
            g_previousScene = this->getRunningScene();
            g_previousCaptureCostMs = 0.0;
            CCDirector::drawScene();
            return;
        }

        const auto frameStart = Clock::now();
        const double nowSeconds = secondsSinceBoot(frameStart);
        auto* runningScene = this->getRunningScene();

        if (runningScene != g_previousScene) {
            g_previousScene = runningScene;
            g_previousFrameStart = frameStart;
            g_havePreviousFrame = true;
            g_frameHead = 0;
            g_frameCount = 0;
            g_previousCaptureCostMs = 0.0;
            CCDirector::drawScene();
            return;
        }

        double rawGapMs = 0.0;
        if (g_havePreviousFrame) {
            rawGapMs = milliseconds(frameStart - g_previousFrameStart);
        }

        g_previousFrameStart = frameStart;
        g_havePreviousFrame = true;

        const double priorDiagnosticMs = g_previousCaptureCostMs;
        g_previousCaptureCostMs = 0.0;

        const double rawEngineDelta = static_cast<double>(this->getDeltaTime());
        const double engineDeltaMs =
            std::isfinite(rawEngineDelta) && rawEngineDelta > 0.0
                ? rawEngineDelta * 1000.0
                : 0.0;

        const auto drawStarted = Clock::now();
        CCDirector::drawScene();
        const double drawMs = milliseconds(Clock::now() - drawStarted);

        ++g_levelFrame;

        if (rawGapMs <= 0.0 || !std::isfinite(rawGapMs)) {
            return;
        }

        const double threshold = static_cast<double>(g_spikeThresholdMs);
        const double budget = frameBudgetMs();
        const double adjustedGapMs =
            std::max(0.0, rawGapMs - priorDiagnosticMs);

        const bool longInterruption = rawGapMs >= 2000.0;
        const bool gapSpike = rawGapMs >= threshold;
        const bool drawSpike = drawMs >= budget;
        const bool isSpike = gapSpike || drawSpike || longInterruption;

        FrameSample sample{
            g_levelFrame,
            rawGapMs,
            adjustedGapMs,
            priorDiagnosticMs,
            drawMs,
            engineDeltaMs,
            gapSpike || longInterruption,
            drawSpike
        };

        recordFrame(sample);

        if (g_overlayLabel && g_overlayLabel->isVisible() &&
            nowSeconds >= g_overlayExpiresAt) {
            g_overlayLabel->setVisible(false);
        }

        if (isSpike) {
            if (longInterruption) {
                log::warn(
                    "[CulpritFinder] very long frame gap ({:.1f}ms): possible app "
                    "suspension, OS interruption, or severe main-thread hang",
                    rawGapMs
                );
            }

            const auto diagnosticStarted = Clock::now();
            captureSpike(sample, nowSeconds);
            g_previousCaptureCostMs = milliseconds(
                Clock::now() - diagnosticStarted
            );
        }
    }
};

$on_mod(Loaded) {
    readSettings();

    listenForSettingChanges<bool>("enabled", [](bool value) {
        g_enabled = value;
        g_havePreviousFrame = false;
        g_previousCaptureCostMs = 0.0;
        log::info("[CulpritFinder] capture {}", value ? "enabled" : "disabled");
    });

    listenForSettingChanges<bool>("show-overlay", [](bool value) {
        g_showOverlay = value;
        if (g_overlayLabel) {
            g_overlayLabel->setVisible(false);
        }
        log::info("[CulpritFinder] overlay {}", value ? "enabled" : "disabled");
    });

    listenForSettingChanges<bool>("platform-snapshot", [](bool value) {
        g_includePlatformSnapshot = value;
    });

    listenForSettingChanges<bool>("frame-timeline", [](bool value) {
        g_verboseTimeline = value;
    });

    listenForSettingChanges<std::int64_t>("spike-threshold-ms", [](std::int64_t value) {
        g_spikeThresholdMs = static_cast<int>(
            std::clamp<std::int64_t>(value, 17, 80)
        );
    });

    listenForSettingChanges<std::int64_t>("report-cooldown-ms", [](std::int64_t value) {
        g_reportCooldownMs = static_cast<int>(
            std::clamp<std::int64_t>(value, 0, 2000)
        );
    });

    log::info(
        "[CulpritFinder] loaded | diagnostic-only | display={:.1f}Hz | "
        "threshold={}ms | frame-history={} | input-history={}",
        displayRefreshRate(),
        g_spikeThresholdMs,
        kFrameHistory,
        kInputHistory
    );
}
