#include <Geode/Geode.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/loader/Hook.hpp>
#include <Geode/loader/Loader.hpp>
#include <Geode/loader/Mod.hpp>
#include <Geode/loader/Priority.hpp>

#include "RefreshRate.hpp"
#include "PlatformDiagnostics.hpp"

#include <OpenGLES/ES2/gl.h>
#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace geode::prelude;

namespace {
    using Clock = std::chrono::steady_clock;

    constexpr std::size_t kFrameHistory = 120;
    constexpr std::size_t kInputHistory = 64;
    constexpr std::size_t kGpuHistory = 120;
    constexpr std::size_t kGpuQueryCount = 6;
    constexpr std::size_t kTimelineFrames = 12;
    constexpr std::size_t kRecentInputTrail = 10;
    constexpr std::size_t kRecentSpikeFrames = 64;
    constexpr std::size_t kMaxQueuedFileRecords = 512;
    constexpr double kInputCorrelationWindowMs = 150.0;

    constexpr GLenum kGlTimeElapsedExt = 0x88BF;
    constexpr GLenum kGlQueryResultExt = 0x8866;
    constexpr GLenum kGlQueryResultAvailableExt = 0x8867;
    constexpr GLenum kGlGpuDisjointExt = 0x8FBB;

    bool g_enabled = true;
    bool g_showOverlay = true;
    bool g_includePlatformSnapshot = true;
    bool g_verboseTimeline = true;
    bool g_includeHookInventory = true;
    bool g_gpuTimerEnabled = true;
    int g_spikeThresholdMs = 20;
    int g_reportCooldownMs = 300;

    enum class HookTarget : std::size_t {
        DirectorDraw,
        PlayLayerUpdate,
        GameObjectUpdate,
        PlayerUpdate,
        PlayerPress,
        PlayerRelease,
        Count
    };

    constexpr std::array<const char*, static_cast<std::size_t>(HookTarget::Count)>
        kHookTargetNames = {
            "CCDirector::drawScene",
            "PlayLayer::update",
            "GameObject::update",
            "PlayerObject::update",
            "PlayerObject::pushButton",
            "PlayerObject::releaseButton"
        };

    std::array<std::uintptr_t, static_cast<std::size_t>(HookTarget::Count)>
        g_hookTargetAddresses{};

    enum class TimedFunction : std::size_t {
        PlayLayerUpdate,
        GameObjectUpdate,
        PlayerObjectUpdate,
        PlayerPress,
        PlayerRelease,
        Count
    };

    constexpr std::array<const char*, static_cast<std::size_t>(TimedFunction::Count)>
        kTimedFunctionNames = {
            "PlayLayer::update",
            "GameObject::update",
            "PlayerObject::update",
            "PlayerObject::pushButton hook-chain",
            "PlayerObject::releaseButton hook-chain"
        };

    struct CallTiming {
        std::uint32_t calls = 0;
        std::uint32_t overOneMs = 0;
        double totalMs = 0.0;
        double maxMs = 0.0;
        double lastMs = 0.0;
    };

    using CallTimingArray =
        std::array<CallTiming, static_cast<std::size_t>(TimedFunction::Count)>;

    CallTimingArray g_liveCallTimings{};

    struct FrameSample {
        std::uint64_t frame = 0;
        double rawGapMs = 0.0;
        double adjustedGapMs = 0.0;
        double priorDiagnosticMs = 0.0;
        double drawSceneMs = 0.0;
        double engineDeltaMs = 0.0;
        double gpuPollMs = 0.0;
        double gpuMs = -1.0;
        std::uint64_t gpuFrame = 0;
        bool gapSpike = false;
        bool drawSpike = false;
        CallTimingArray calls{};
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
        double averageDrawSceneMs = 0.0;
        double p50GapMs = 0.0;
        double p95GapMs = 0.0;
        double p99GapMs = 0.0;
        double maxGapMs = 0.0;
        double maxDrawSceneMs = 0.0;
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

    struct GpuSample {
        std::uint64_t frame = 0;
        double elapsedMs = 0.0;
    };

    struct GpuStats {
        std::size_t samples = 0;
        double latestMs = -1.0;
        std::uint64_t latestFrame = 0;
        double averageMs = 0.0;
        double p95Ms = 0.0;
        double maxMs = 0.0;
        std::uint64_t disjointCount = 0;
        std::uint64_t skippedQueries = 0;
    };

    struct GpuQuerySlot {
        GLuint id = 0;
        bool pending = false;
        std::uint64_t frame = 0;
    };

    using GenQueriesFn = void (*)(GLsizei, GLuint*);
    using DeleteQueriesFn = void (*)(GLsizei, const GLuint*);
    using BeginQueryFn = void (*)(GLenum, GLuint);
    using EndQueryFn = void (*)(GLenum);
    using GetQueryObjectUivFn = void (*)(GLuint, GLenum, GLuint*);
    using GetQueryObjectUi64vFn = void (*)(GLuint, GLenum, unsigned long long*);

    struct GpuQueryApi {
        GenQueriesFn genQueries = nullptr;
        DeleteQueriesFn deleteQueries = nullptr;
        BeginQueryFn beginQuery = nullptr;
        EndQueryFn endQuery = nullptr;
        GetQueryObjectUivFn getQueryObjectUiv = nullptr;
        GetQueryObjectUi64vFn getQueryObjectUi64v = nullptr;
        bool initialized = false;
        bool extensionAdvertised = false;
        bool available = false;
        bool queryActive = false;
        int activeSlot = -1;
        std::string status = "not initialized";
        std::string vendor = "unknown";
        std::string renderer = "unknown";
        std::string version = "unknown";
        std::uint64_t disjointCount = 0;
        std::uint64_t skippedQueries = 0;
        std::array<GpuQuerySlot, kGpuQueryCount> slots{};
    };

    std::array<FrameSample, kFrameHistory> g_frames{};
    std::size_t g_frameHead = 0;
    std::size_t g_frameCount = 0;

    std::array<InputEvent, kInputHistory> g_inputs{};
    std::size_t g_inputHead = 0;
    std::size_t g_inputCount = 0;

    std::array<GpuSample, kGpuHistory> g_gpuSamples{};
    std::size_t g_gpuHead = 0;
    std::size_t g_gpuCount = 0;

    std::array<std::uint64_t, kRecentSpikeFrames> g_recentSpikeFrames{};
    std::size_t g_recentSpikeHead = 0;
    std::size_t g_recentSpikeCount = 0;

    Clock::time_point g_previousFrameStart{};
    bool g_havePreviousFrame = false;
    cocos2d::CCScene* g_previousScene = nullptr;

    std::uint64_t g_levelFrame = 0;
    std::uint64_t g_totalSpikes = 0;
    std::uint64_t g_framesCaptured = 0;
    std::uint64_t g_gpuResultsAttachedToSpikes = 0;
    double g_levelStartedAt = 0.0;
    double g_lastDetailedReportAt = -10000.0;
    double g_previousCaptureCostMs = 0.0;
    std::uint64_t g_coalescedReports = 0;
    std::int64_t g_levelId = 0;
    std::string g_levelName = "unknown";

    GpuQueryApi g_gpu{};
    CCLabelBMFont* g_overlayLabel = nullptr;
    double g_overlayExpiresAt = 0.0;

    class AsyncCaptureWriter {
    public:
        explicit AsyncCaptureWriter(std::filesystem::path path)
          : m_path(std::move(path)) {
            m_worker = std::jthread([this](std::stop_token stopToken) {
                this->run(stopToken);
            });
        }

        ~AsyncCaptureWriter() {
            if (m_worker.joinable()) {
                m_worker.request_stop();
                m_cv.notify_all();
                m_worker.join();
            }
        }

        void append(std::string record) {
            try {
                {
                    std::lock_guard lock(m_mutex);
                    if (m_queue.size() >= kMaxQueuedFileRecords) {
                        m_queue.pop_front();
                        ++m_droppedRecords;
                    }
                    m_queue.emplace_back(std::move(record));
                }
                m_cv.notify_one();
            }
            catch (...) {
                ++m_droppedRecords;
            }
        }

        std::uint64_t droppedRecords() const {
            return m_droppedRecords.load();
        }

        std::string pathString() const {
            return m_path.string();
        }

    private:
        void run(std::stop_token stopToken) {
            std::ofstream output(m_path, std::ios::out | std::ios::app);
            if (!output) {
                m_openFailed.store(true);
                return;
            }

            m_opened.store(true);

            for (;;) {
                std::string record;

                {
                    std::unique_lock lock(m_mutex);
                    m_cv.wait(lock, stopToken, [this] {
                        return !m_queue.empty();
                    });

                    if (m_queue.empty() && stopToken.stop_requested()) {
                        break;
                    }

                    if (m_queue.empty()) {
                        continue;
                    }

                    record = std::move(m_queue.front());
                    m_queue.pop_front();
                }

                output << record << "\n";
                output.flush();
            }
        }

        std::filesystem::path m_path;
        mutable std::mutex m_mutex;
        std::condition_variable_any m_cv;
        std::deque<std::string> m_queue;
        std::atomic<std::uint64_t> m_droppedRecords{0};
        std::atomic<bool> m_opened{false};
        std::atomic<bool> m_openFailed{false};
        std::jthread m_worker;
    };

    std::unique_ptr<AsyncCaptureWriter> g_captureWriter;
    std::string g_captureFilePath;
    bool g_captureFileReady = false;

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

    std::string glString(GLenum name) {
        const auto* value = glGetString(name);
        if (!value) {
            return "unavailable";
        }
        return reinterpret_cast<const char*>(value);
    }

    bool containsExtension(const char* extensions, const char* wanted) {
        if (!extensions || !wanted || !*wanted) {
            return false;
        }

        const std::size_t wantedLength = std::strlen(wanted);
        const char* cursor = extensions;

        while ((cursor = std::strstr(cursor, wanted)) != nullptr) {
            const bool leftBoundary =
                cursor == extensions || cursor[-1] == ' ';
            const char after = cursor[wantedLength];
            const bool rightBoundary = after == '\0' || after == ' ';

            if (leftBoundary && rightBoundary) {
                return true;
            }

            cursor += wantedLength;
        }

        return false;
    }

    void* resolveGlSymbol(const char* name, void* frameworkHandle) {
        void* symbol = dlsym(RTLD_DEFAULT, name);
        if (!symbol && frameworkHandle) {
            symbol = dlsym(frameworkHandle, name);
        }
        return symbol;
    }

    void initializeGpuTimer() {
        if (g_gpu.initialized) {
            return;
        }

        g_gpu.initialized = true;
        g_gpu.vendor = glString(GL_VENDOR);
        g_gpu.renderer = glString(GL_RENDERER);
        g_gpu.version = glString(GL_VERSION);

        const auto* extensionsRaw = glGetString(GL_EXTENSIONS);
        const char* extensions =
            reinterpret_cast<const char*>(extensionsRaw);

        g_gpu.extensionAdvertised =
            containsExtension(extensions, "GL_EXT_disjoint_timer_query");

        if (!g_gpu.extensionAdvertised) {
            g_gpu.status = "unavailable: GL_EXT_disjoint_timer_query not advertised";
            return;
        }

        void* framework = dlopen(
            "/System/Library/Frameworks/OpenGLES.framework/OpenGLES",
            RTLD_LAZY
        );

        g_gpu.genQueries = reinterpret_cast<GenQueriesFn>(
            resolveGlSymbol("glGenQueriesEXT", framework)
        );
        g_gpu.deleteQueries = reinterpret_cast<DeleteQueriesFn>(
            resolveGlSymbol("glDeleteQueriesEXT", framework)
        );
        g_gpu.beginQuery = reinterpret_cast<BeginQueryFn>(
            resolveGlSymbol("glBeginQueryEXT", framework)
        );
        g_gpu.endQuery = reinterpret_cast<EndQueryFn>(
            resolveGlSymbol("glEndQueryEXT", framework)
        );
        g_gpu.getQueryObjectUiv = reinterpret_cast<GetQueryObjectUivFn>(
            resolveGlSymbol("glGetQueryObjectuivEXT", framework)
        );
        g_gpu.getQueryObjectUi64v = reinterpret_cast<GetQueryObjectUi64vFn>(
            resolveGlSymbol("glGetQueryObjectui64vEXT", framework)
        );

        if (!g_gpu.genQueries || !g_gpu.deleteQueries ||
            !g_gpu.beginQuery || !g_gpu.endQuery ||
            !g_gpu.getQueryObjectUiv || !g_gpu.getQueryObjectUi64v) {
            g_gpu.status = "unavailable: extension advertised but timer-query entry points missing";
            return;
        }

        std::array<GLuint, kGpuQueryCount> ids{};
        g_gpu.genQueries(static_cast<GLsizei>(ids.size()), ids.data());

        bool anyId = false;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            g_gpu.slots[i].id = ids[i];
            anyId = anyId || ids[i] != 0;
        }

        if (!anyId) {
            g_gpu.status = "unavailable: glGenQueriesEXT returned no query objects";
            return;
        }

        g_gpu.available = true;
        g_gpu.status = "active: asynchronous EXT_disjoint_timer_query";
    }

    bool isRecentSpikeFrame(std::uint64_t frame) {
        for (std::size_t offset = 0; offset < g_recentSpikeCount; ++offset) {
            const std::size_t index =
                (g_recentSpikeHead + kRecentSpikeFrames - g_recentSpikeCount + offset) %
                kRecentSpikeFrames;

            if (g_recentSpikeFrames[index] == frame) {
                return true;
            }
        }
        return false;
    }

    void rememberSpikeFrame(std::uint64_t frame) {
        g_recentSpikeFrames[g_recentSpikeHead] = frame;
        g_recentSpikeHead = (g_recentSpikeHead + 1) % kRecentSpikeFrames;
        g_recentSpikeCount =
            std::min(g_recentSpikeCount + 1, kRecentSpikeFrames);
    }

    void appendFileRecord(std::string record) {
        if (g_captureWriter) {
            g_captureWriter->append(std::move(record));
        }
    }

    void addGpuSample(std::uint64_t frame, double elapsedMs) {
        g_gpuSamples[g_gpuHead] = {frame, elapsedMs};
        g_gpuHead = (g_gpuHead + 1) % kGpuHistory;
        g_gpuCount = std::min(g_gpuCount + 1, kGpuHistory);

        for (std::size_t offset = 0; offset < g_frameCount; ++offset) {
            const std::size_t index =
                (g_frameHead + kFrameHistory - g_frameCount + offset) %
                kFrameHistory;

            if (g_frames[index].frame == frame) {
                g_frames[index].gpuMs = elapsedMs;
                g_frames[index].gpuFrame = frame;
                break;
            }
        }

        if (isRecentSpikeFrame(frame)) {
            ++g_gpuResultsAttachedToSpikes;

            appendFileRecord(fmt::format(
                "[GPU RESULT - ASYNCHRONOUS]\n"
                "The GPU timer query for spike-related frame {} is now available.\n"
                "GPU elapsed command time: {:.4f}ms\n"
                "This is GPU execution time for the command interval bracketed "
                "around CCDirector::drawScene; it is not CPU time or display scanout.\n"
                "==============================================================",
                frame,
                elapsedMs
            ));
        }
    }

    void pollGpuQueries() {
        if (!g_gpu.available || !g_gpu.getQueryObjectUiv ||
            !g_gpu.getQueryObjectUi64v) {
            return;
        }

        GLint disjoint = 0;
        glGetIntegerv(kGlGpuDisjointExt, &disjoint);

        if (disjoint != 0) {
            ++g_gpu.disjointCount;

            for (auto& slot : g_gpu.slots) {
                slot.pending = false;
                slot.frame = 0;
            }

            return;
        }

        for (auto& slot : g_gpu.slots) {
            if (!slot.pending || slot.id == 0) {
                continue;
            }

            GLuint available = 0;
            g_gpu.getQueryObjectUiv(
                slot.id,
                kGlQueryResultAvailableExt,
                &available
            );

            if (available == 0) {
                continue;
            }

            unsigned long long elapsedNanoseconds = 0;
            g_gpu.getQueryObjectUi64v(
                slot.id,
                kGlQueryResultExt,
                &elapsedNanoseconds
            );

            const std::uint64_t completedFrame = slot.frame;
            slot.pending = false;
            slot.frame = 0;

            if (elapsedNanoseconds > 0) {
                const double elapsedMs =
                    static_cast<double>(elapsedNanoseconds) / 1000000.0;
                addGpuSample(completedFrame, elapsedMs);
            }
        }
    }

    void beginGpuQuery(std::uint64_t frame) {
        if (!g_gpu.available || !g_gpu.beginQuery || g_gpu.queryActive) {
            return;
        }

        for (std::size_t i = 0; i < g_gpu.slots.size(); ++i) {
            auto& slot = g_gpu.slots[i];

            if (slot.id == 0 || slot.pending) {
                continue;
            }

            g_gpu.beginQuery(kGlTimeElapsedExt, slot.id);
            slot.pending = true;
            slot.frame = frame;
            g_gpu.activeSlot = static_cast<int>(i);
            g_gpu.queryActive = true;
            return;
        }

        ++g_gpu.skippedQueries;
    }

    void endGpuQuery() {
        if (!g_gpu.available || !g_gpu.queryActive || !g_gpu.endQuery) {
            return;
        }

        g_gpu.endQuery(kGlTimeElapsedExt);
        g_gpu.queryActive = false;
        g_gpu.activeSlot = -1;
    }

    void recordTimedCall(TimedFunction function, double elapsedMs) {
        auto& timing = g_liveCallTimings[static_cast<std::size_t>(function)];
        ++timing.calls;
        timing.totalMs += elapsedMs;
        timing.lastMs = elapsedMs;
        timing.maxMs = std::max(timing.maxMs, elapsedMs);

        if (elapsedMs >= 1.0) {
            ++timing.overOneMs;
        }
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
        ++g_framesCaptured;
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
                drawSum += sample.drawSceneMs;
                ++stats.samples;
                stats.maxGapMs = std::max(stats.maxGapMs, sample.rawGapMs);
                stats.maxDrawSceneMs =
                    std::max(stats.maxDrawSceneMs, sample.drawSceneMs);
                stats.maxEngineDeltaMs =
                    std::max(stats.maxEngineDeltaMs, sample.engineDeltaMs);
            }

            if (sample.gapSpike) {
                ++stats.spikes;
            }
        }

        if (stats.samples > 0) {
            stats.averageGapMs = gapSum / static_cast<double>(stats.samples);
            stats.averageDrawSceneMs =
                drawSum / static_cast<double>(stats.samples);
            stats.p50GapMs = percentile(gaps, gapCount, 0.50);
            stats.p95GapMs = percentile(gaps, gapCount, 0.95);
            stats.p99GapMs = percentile(gaps, gapCount, 0.99);
        }

        return stats;
    }

    double gpuPercentile(std::array<double, kGpuHistory> values,
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

    GpuStats collectGpuStats() {
        GpuStats stats{};
        std::array<double, kGpuHistory> times{};
        double sum = 0.0;

        for (std::size_t offset = 0; offset < g_gpuCount; ++offset) {
            const std::size_t index =
                (g_gpuHead + kGpuHistory - g_gpuCount + offset) %
                kGpuHistory;
            const auto& sample = g_gpuSamples[index];

            times[offset] = sample.elapsedMs;
            sum += sample.elapsedMs;
            ++stats.samples;
            stats.latestMs = sample.elapsedMs;
            stats.latestFrame = sample.frame;
            stats.maxMs = std::max(stats.maxMs, sample.elapsedMs);
        }

        if (stats.samples > 0) {
            stats.averageMs = sum / static_cast<double>(stats.samples);
            stats.p95Ms = gpuPercentile(times, stats.samples, 0.95);
        }

        stats.disjointCount = g_gpu.disjointCount;
        stats.skippedQueries = g_gpu.skippedQueries;
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
                timeline += "\n";
            }

            timeline += fmt::format(
                "  F{} raw={:.2f} adj={:.2f} profiler-prev={:.2f} "
                "drawScene={:.2f} engine-dt={:.2f} gpu={}{}",
                sample.frame,
                sample.rawGapMs,
                sample.adjustedGapMs,
                sample.priorDiagnosticMs,
                sample.drawSceneMs,
                sample.engineDeltaMs,
                sample.gpuMs >= 0.0
                    ? fmt::format("{:.3f}ms@F{}", sample.gpuMs, sample.gpuFrame)
                    : "pending/unavailable",
                sample.gapSpike ? " SPIKE" : ""
            );
        }

        return timeline.empty() ? "  not enough frames captured" : timeline;
    }

    std::string buildInputTrail(double nowSeconds) {
        std::string trail;
        const std::size_t count = std::min(g_inputCount, kRecentInputTrail);

        for (std::size_t offset = 0; offset < count; ++offset) {
            const std::size_t index =
                (g_inputHead + kInputHistory - count + offset) %
                kInputHistory;
            const auto& event = g_inputs[index];
            const double ageMs = (nowSeconds - event.timeSeconds) * 1000.0;

            if (!trail.empty()) {
                trail += "\n";
            }

            trail += fmt::format(
                "  {} button={} player={} age={:.1f}ms hook-chain={:.3f}ms",
                event.down ? "PRESS" : "RELEASE",
                event.button,
                event.player2 ? 2 : 1,
                ageMs,
                event.hookMs
            );
        }

        return trail.empty() ? "  no PlayerObject press/release hooks observed yet" : trail;
    }

    std::string buildCallTimingReport(const CallTimingArray& timings) {
        std::string report;

        for (std::size_t i = 0; i < timings.size(); ++i) {
            const auto& timing = timings[i];

            if (!report.empty()) {
                report += "\n";
            }

            const double averageMs =
                timing.calls > 0
                    ? timing.totalMs / static_cast<double>(timing.calls)
                    : 0.0;

            report += fmt::format(
                "  {}: calls={} total={:.3f}ms avg={:.4f}ms max={:.3f}ms "
                "calls>=1ms={}",
                kTimedFunctionNames[i],
                timing.calls,
                timing.totalMs,
                averageMs,
                timing.maxMs,
                timing.overOneMs
            );
        }

        return report;
    }

    std::string buildHookInventory() {
        if (!g_includeHookInventory) {
            return "Hook inventory disabled in settings.";
        }

        struct HookRecord {
            std::size_t target = 0;
            std::string modId;
            std::string modName;
            std::string displayName;
            std::string modifyClass;
            std::uintptr_t address = 0;
            int priority = 0;
            bool enabled = false;
        };

        std::vector<HookRecord> records;
        auto mods = Loader::get()->getAllMods();

        for (auto* mod : mods) {
            if (!mod) {
                continue;
            }

            for (auto* hook : mod->getHooks()) {
                if (!hook) {
                    continue;
                }

                const auto address = hook->getAddress();

                for (std::size_t target = 0;
                     target < g_hookTargetAddresses.size();
                     ++target) {
                    if (g_hookTargetAddresses[target] == 0 ||
                        address != g_hookTargetAddresses[target]) {
                        continue;
                    }

                    records.push_back({
                        target,
                        std::string(mod->getID().view()),
                        std::string(mod->getName().view()),
                        std::string(hook->getDisplayName()),
                        "not exposed by Geode 5.10.1",
                        address,
                        hook->getPriority(),
                        hook->isEnabled()
                    });
                }
            }
        }

        if (records.empty()) {
            return "No other registered hooks were found at the monitored function addresses.";
        }

        std::stable_sort(
            records.begin(),
            records.end(),
            [](const HookRecord& a, const HookRecord& b) {
                if (a.target != b.target) {
                    return a.target < b.target;
                }
                return a.priority < b.priority;
            }
        );

        std::string report;
        report += fmt::format(
            "Registered hook inventory: {} hook(s) at monitored target address(es).\n",
            records.size()
        );

        for (const auto& record : records) {
            report += fmt::format(
                "  target={} | {} ({}) | hook={} | modify={} | enabled={} | "
                "priority={} | address=0x{:x}\n",
                kHookTargetNames[record.target],
                record.modName,
                record.modId,
                record.displayName,
                record.modifyClass,
                record.enabled ? "YES" : "NO",
                record.priority,
                record.address
            );
        }

        report +=
            "  Interpretation: enabled hooks registered at the same target address "
            "are in that function's detour chain. Culprit Finder sets its own hooks "
            "to Priority::First, so the function-level timers include the downstream "
            "hook chain and original function. Geode's public API does not expose an "
            "individual runtime stopwatch for every third-party hook callback; "
            "these entries identify chain participants, not per-mod execution costs.";

        return report;
    }

    std::string classifySpike(
        const FrameSample& sample,
        const FrameStats& frameStats,
        const GpuStats& gpuStats,
        const InputStats& inputStats,
        const geode::culpritfinder::PlatformSnapshot& platform,
        bool platformCaptured
    ) {
        const double budget = frameBudgetMs();
        std::string reasons;

        auto addReason = [&](std::string value) {
            if (!reasons.empty()) {
                reasons += "\n";
            }
            reasons += std::move(value);
        };

        if (sample.priorDiagnosticMs >= 1.0 &&
            sample.rawGapMs >= static_cast<double>(g_spikeThresholdMs) &&
            sample.adjustedGapMs < static_cast<double>(g_spikeThresholdMs)) {
            addReason(fmt::format(
                "HIGH CONFIDENCE - PROFILER SELF-OVERHEAD CONTRIBUTED: the previous "
                "report took {:.3f}ms on the game thread. Raw gap was {:.3f}ms; "
                "subtracting measured profiler work gives {:.3f}ms. File writing is "
                "asynchronous; this cost is formatting, hook inventory, queueing, "
                "and overlay work.",
                sample.priorDiagnosticMs,
                sample.rawGapMs,
                sample.adjustedGapMs
            ));
        }

        if (sample.gpuMs >= 0.0 && sample.gpuMs >= budget) {
            addReason(fmt::format(
                "GPU CONFIRMED - GPU COMMAND INTERVAL EXCEEDED FRAME BUDGET: "
                "frame {} measured {:.3f}ms on the GPU versus {:.3f}ms budget. "
                "This is asynchronous EXT_disjoint_timer_query data, not a CPU estimate.",
                sample.gpuFrame,
                sample.gpuMs,
                budget
            ));
        }
        else if (gpuStats.latestMs >= budget && gpuStats.latestFrame + 1 >= sample.frame) {
            addReason(fmt::format(
                "GPU SUSPECT - the most recently completed GPU query measured "
                "{:.3f}ms on frame {}, above the {:.3f}ms budget. It is close in "
                "time to this hitch but is not necessarily the exact same frame.",
                gpuStats.latestMs,
                gpuStats.latestFrame,
                budget
            ));
        }

        if (sample.drawSceneMs >= budget * 0.80) {
            addReason(fmt::format(
                "CPU DRAW PATH - drawScene hook chain consumed {:.3f}ms wall time "
                "against a {:.3f}ms frame budget. This includes drawScene work and "
                "any downstream hooks on that function. GPU timing above separates "
                "GPU execution when the extension is supported.",
                sample.drawSceneMs,
                budget
            ));
        }

        addReason(
            "GAMEPLAY UPDATE AND INPUT HOOKS DISABLED - this safe observer does not "
            "detour PlayerObject, GameObject, or PlayLayer::update. It cannot attribute "
            "a gameplay-object or collision problem to an individual mod."
        );

        if (sample.engineDeltaMs >= budget * 1.50) {
            addReason(fmt::format(
                "ENGINE DELTA JUMP - Cocos director delta reached {:.3f}ms.",
                sample.engineDeltaMs
            ));
        }

        if (sample.adjustedGapMs >= static_cast<double>(g_spikeThresholdMs) &&
            sample.drawSceneMs < budget * 0.40 &&
            playUpdate.totalMs < budget * 0.30 &&
            objectUpdate.totalMs < budget * 0.30) {
            addReason(
                "PRE-DRAW / SCHEDULER SUSPECT - the frame arrived late, but the "
                "measured PlayLayer/GameObject update and drawScene hooks were not "
                "large enough to explain it. Possible OS scheduling, other "
                "unmonitored callbacks, or work outside the instrumented functions."
            );
        }

        if (platformCaptured && platform.thermalState >= 2) {
            addReason(fmt::format(
                "THERMAL CLUE - iOS reports {} thermal state. This can coincide "
                "with CPU/GPU throttling, but does not prove throttling caused this hitch.",
                thermalName(platform.thermalState)
            ));
        }

        if (platformCaptured && platform.memoryValid &&
            platform.physicalMemoryBytes > 0) {
            const double residentRatio =
                static_cast<double>(platform.residentBytes) /
                static_cast<double>(platform.physicalMemoryBytes);

            if (residentRatio >= 0.50) {
                addReason(fmt::format(
                    "MEMORY CLUE - resident memory is {:.1f}% of physical device "
                    "memory. This is only a snapshot, not an allocation trace or "
                    "proof of memory pressure.",
                    residentRatio * 100.0
                ));
            }
        }

        if (!g_gpu.available) {
            addReason(fmt::format(
                "GPU MEASUREMENT UNAVAILABLE - {}. CPU drawScene timing is still "
                "captured, but no GPU-time conclusion can be made on this renderer.",
                g_gpu.status
            ));
        }
        else if (gpuStats.samples == 0) {
            addReason(
                "GPU TIMER ACTIVE - no asynchronous GPU result has completed yet; "
                "the current spike's GPU query may arrive in a later file entry."
            );
        }

        if (sample.rawGapMs >= 2000.0) {
            addReason(
                "LONG INTERRUPTION - frame gap is at least 2 seconds; possible "
                "app suspension, OS interruption, or severe main-thread stall."
            );
        }

        if (reasons.empty()) {
            reasons =
                "No single monitored subsystem explains this event yet. Compare "
                "the detailed timing, call counts, hook inventory, and asynchronous "
                "GPU result for the same frame.";
        }

        (void) frameStats;
        (void) inputStats;
        return reasons;
    }

    std::string makeReport(
        const FrameSample& sample,
        double nowSeconds,
        const FrameStats& frameStats,
        const GpuStats& gpuStats,
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

        std::string report = fmt::format(
            "\n\n"
            "######################################################################\n"
            "CULPRIT FINDER - FRAME SPIKE #{}\n"
            "LEVEL: {} | id={} | elapsed={:.2f}s | level-frame={}\n"
            "SESSION: total-spikes={} | captured-frames={} | file={}\n"
            "DISPLAY: {:.1f}Hz | expected-frame-budget={:.3f}ms\n"
            "FRAME: raw-gap={:.3f}ms | adjusted-gap={:.3f}ms | "
            "previous-profiler-cost={:.3f}ms | instant-fps={:.2f} | severity={:.2f}x\n"
            "CPU DRAW: drawScene-hook-chain={:.3f}ms | engine-delta={:.3f}ms | "
            "GPU-poll-cost={:.4f}ms\n"
            "FRAME HISTORY: samples={} | avg-gap={:.3f}ms | p50={:.3f}ms | "
            "p95={:.3f}ms | p99={:.3f}ms | max-gap={:.3f}ms | "
            "avg-drawScene={:.3f}ms | max-drawScene={:.3f}ms | "
            "max-engine-delta={:.3f}ms | spikes-in-window={}\n"
            "GAMEPLAY HOOKS: disabled in safe-observer mode; no PlayerObject, "
            "GameObject, or PlayLayer::update detours are installed.\n"
            "POSSIBLE CAUSES / EVIDENCE:\n{}\n"
            "INPUT MONITORING: disabled in safe-observer mode; no PlayerObject "
            "input detours are installed.\n"
            "GPU TIMER: status={} | extension-advertised={} | completed-samples={} | "
            "latest={:.4f}ms @ frame {} | avg={:.4f}ms | p95={:.4f}ms | "
            "max={:.4f}ms | disjoint-events={} | skipped-queries={}\n"
            "GPU FOR THIS FRAME: {}",
            g_totalSpikes,
            g_levelName,
            g_levelId,
            levelElapsed,
            g_levelFrame,
            g_totalSpikes,
            g_framesCaptured,
            g_captureFilePath.empty() ? "FILE NOT AVAILABLE" : g_captureFilePath,
            displayRefreshRate(),
            budget,
            sample.rawGapMs,
            sample.adjustedGapMs,
            sample.priorDiagnosticMs,
            fps,
            severity,
            sample.drawSceneMs,
            sample.engineDeltaMs,
            sample.gpuPollMs,
            frameStats.samples,
            frameStats.averageGapMs,
            frameStats.p50GapMs,
            frameStats.p95GapMs,
            frameStats.p99GapMs,
            frameStats.maxGapMs,
            frameStats.averageDrawSceneMs,
            frameStats.maxDrawSceneMs,
            frameStats.maxEngineDeltaMs,
            frameStats.spikes,
            classifySpike(sample, frameStats, gpuStats, inputStats, platform, platformCaptured),
            g_gpu.status,
            g_gpu.extensionAdvertised,
            gpuStats.samples,
            gpuStats.latestMs,
            gpuStats.latestFrame,
            gpuStats.averageMs,
            gpuStats.p95Ms,
            gpuStats.maxMs,
            gpuStats.disjointCount,
            gpuStats.skippedQueries,
            sample.gpuMs >= 0.0
                ? fmt::format("same-frame result available: {:.4f}ms", sample.gpuMs)
                : "pending/unavailable; a completed query for this frame will be appended as a later GPU RESULT entry"
        );

        report += fmt::format(
            "\nGPU DEVICE: vendor={} | renderer={} | GL version={}",
            g_gpu.vendor,
            g_gpu.renderer,
            g_gpu.version
        );

        report += "\n\nREGISTERED HOOKS ON THE MONITORED FUNCTION ADDRESSES:\n";
        report += buildHookInventory();

        if (platformCaptured) {
            report += fmt::format(
                "\n\nIOS SNAPSHOT: memory-valid={} | resident={:.1f}MiB | "
                "virtual={:.1f}MiB | physical-device-memory={:.1f}MiB | "
                "thermal={} | active-processors={}",
                platform.memoryValid,
                static_cast<double>(platform.residentBytes) / (1024.0 * 1024.0),
                static_cast<double>(platform.virtualBytes) / (1024.0 * 1024.0),
                static_cast<double>(platform.physicalMemoryBytes) / (1024.0 * 1024.0),
                thermalName(platform.thermalState),
                platform.activeProcessorCount
            );
        }
        else {
            report += "\n\nIOS SNAPSHOT: skipped by settings or detailed-report cooldown.";
        }

        report += "\n\nRECENT FRAME TIMELINE:\n";
        report += g_verboseTimeline ? buildTimeline() : "  disabled in settings";

        report +=
            "\n\nMEASUREMENT LIMITS:\n"
            "- CPU times are wall-clock timings around the named hook and its "
            "downstream hook chain; they are not per-mod callback times.\n"
            "- Hook inventory lists registered hooks attached to the same target "
            "addresses. Enabled entries are expected to participate when that "
            "function is called, but individual detour durations are not exposed "
            "by Geode's public hook API.\n"
            "- GPU time is collected asynchronously with EXT_disjoint_timer_query "
            "only when the active OpenGL ES renderer advertises and exports it. "
            "No glFinish or synchronous GPU wait is used.\n"
            "- GPU time covers GPU command execution in the interval around "
            "drawScene; it is not display scanout, touch latency, or full system "
            "presentation time.\n"
            "- A recent click is a correlation clue, not proof of causation.\n"
            "- Memory and thermal values are snapshots, not allocation or "
            "frequency traces.\n"
            "- File writing is performed on a background writer thread; formatting "
            "and queueing cost is measured and included in the next frame sample.\n"
            "######################################################################";

        return report;
    }

    void updateOverlay(
        const FrameSample& sample,
        const InputStats& input,
        const GpuStats& gpu,
        double nowSeconds
    ) {
        if (!g_overlayLabel || !g_showOverlay) {
            return;
        }

        const double budget = frameBudgetMs();
        std::string primary =
            sample.drawSceneMs >= budget * 0.80
                ? "CPU DRAW PATH IS SLOW"
                : "STALL BEFORE / OUTSIDE DRAW PATH";

        if (sample.gpuMs >= 0.0 && sample.gpuMs >= budget) {
            primary = "GPU COMMAND TIME EXCEEDED BUDGET";
        }
        else if (sample.priorDiagnosticMs >= 1.0 &&
                 sample.adjustedGapMs < static_cast<double>(g_spikeThresholdMs)) {
            primary = "PROFILER OVERHEAD CONTRIBUTED";
        }

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

        std::string gpuLine = "GPU: " + g_gpu.status;
        if (gpu.latestMs >= 0.0) {
            gpuLine = fmt::format(
                "GPU: {:.2f}ms on F{} (async)",
                gpu.latestMs,
                gpu.latestFrame
            );
        }

        const std::string overlay = fmt::format(
            "CULPRIT FINDER - SPIKE #{}\n"
            "gap {:.1f}ms (adjusted {:.1f}) / budget {:.1f}ms\n"
            "drawScene {:.2f}ms | prior profiler {:.2f}ms\n"
            "{}\n{}\n{}\n"
            "File: CulpritFinder-Capture.txt",
            g_totalSpikes,
            sample.rawGapMs,
            sample.adjustedGapMs,
            budget,
            sample.drawSceneMs,
            sample.priorDiagnosticMs,
            primary,
            inputLine,
            gpuLine
        );

        g_overlayLabel->setString(overlay.c_str());
        g_overlayLabel->setVisible(true);
        g_overlayExpiresAt = nowSeconds + 7.0;
    }

    void captureSpike(const FrameSample& sample, double nowSeconds) {
        ++g_totalSpikes;
        rememberSpikeFrame(sample.frame);

        const auto frameStats = collectFrameStats();
        const auto gpuStats = collectGpuStats();
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

        if (!detailedReportDue) {
            ++g_coalescedReports;

            appendFileRecord(fmt::format(
                "[SPIKE EVENT #{} - COALESCED]\n"
                "frame={} raw-gap={:.3f}ms adjusted-gap={:.3f}ms "
                "drawScene={:.3f}ms gpu-latest={:.3f}ms@F{}\n"
                "Full detailed report is cooldown-limited; the event remains "
                "counted in rolling history.\n"
                "==============================================================",
                g_totalSpikes,
                sample.frame,
                sample.rawGapMs,
                sample.adjustedGapMs,
                sample.drawSceneMs,
                gpuStats.latestMs,
                gpuStats.latestFrame
            ));

            return;
        }

        g_lastDetailedReportAt = nowSeconds;

        std::string report = makeReport(
            sample,
            nowSeconds,
            frameStats,
            gpuStats,
            inputStats,
            platform,
            platformCaptured
        );

        if (g_coalescedReports > 0) {
            report += fmt::format(
                "\n\nNOTE: {} additional spike event(s) occurred during the "
                "detailed-report cooldown. Each event was separately written as "
                "a coalesced record and remains represented in rolling history.",
                g_coalescedReports
            );
            g_coalescedReports = 0;
        }

        if (g_captureWriter && g_captureWriter->droppedRecords() > 0) {
            report += fmt::format(
                "\n\nFILE QUEUE WARNING: {} old queued record(s) were dropped "
                "because the background file writer queue reached its safety cap.",
                g_captureWriter->droppedRecords()
            );
        }

        appendFileRecord(std::move(report));
    }

    void attachOverlay(PlayLayer* layer) {
        g_overlayLabel = nullptr;

        if (!layer || !g_showOverlay) {
            return;
        }

        auto* label = CCLabelBMFont::create(
            "CULPRIT FINDER\nMonitoring frame pacing...",
            "chatFont.fnt"
        );

        if (!label) {
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

    void initializeCaptureFile() {
        if (g_captureWriter) {
            return;
        }

        try {
            const auto savePath =
                std::filesystem::path(Mod::get()->getSaveDir().string());

            std::error_code error;
            std::filesystem::create_directories(savePath, error);

            if (error) {
                g_captureFileReady = false;
                g_captureFilePath.clear();
                return;
            }

            const auto filePath = savePath / "CulpritFinder-Capture.txt";

            // Create/touch the file at mod-load time (not during gameplay), so
            // there is an actual file in the per-mod writable save directory.
            {
                std::ofstream touch(filePath, std::ios::out | std::ios::app);
                if (!touch) {
                    g_captureFileReady = false;
                    g_captureFilePath.clear();
                    return;
                }
            }

            g_captureFilePath = filePath.string();
            g_captureWriter =
                std::make_unique<AsyncCaptureWriter>(filePath);
            g_captureFileReady = true;

            appendFileRecord(fmt::format(
                "======================================================================\n"
                "CULPRIT FINDER SESSION START\n"
                "Version: 0.2.0-safe-observer-patch\n"
                "Target: Geometry Dash 2.2081 / Geode 5.10.1 / iOS arm64\n"
                "Capture file: {}\n"
                "SAFE OBSERVER MODE: no PlayerObject, GameObject, or PlayLayer::update detours; no gameplay overlay; no hook-priority changes.\n"
                "No gameplay, physics, graphics quality, cache, or prediction "
                "settings are modified by this diagnostic mod.\n"
                "GPU timer query uses asynchronous GL_EXT_disjoint_timer_query "
                "when available. No glFinish is used.\n"
                "======================================================================",
                g_captureFilePath
            ));
        }
        catch (...) {
            g_captureFileReady = false;
            g_captureFilePath.clear();
            g_captureWriter.reset();
        }
    }

    void readSettings() {
        auto* mod = Mod::get();

        g_enabled = mod->getSettingValue<bool>("enabled");
        g_showOverlay = mod->getSettingValue<bool>("show-overlay");
        g_includePlatformSnapshot =
            mod->getSettingValue<bool>("platform-snapshot");
        g_verboseTimeline = mod->getSettingValue<bool>("frame-timeline");
        g_includeHookInventory =
            mod->getSettingValue<bool>("hook-inventory");
        g_gpuTimerEnabled =
            mod->getSettingValue<bool>("gpu-timer");

        g_spikeThresholdMs = static_cast<int>(
            mod->getSettingValue<std::int64_t>("spike-threshold-ms")
        );
        g_reportCooldownMs = static_cast<int>(
            mod->getSettingValue<std::int64_t>("report-cooldown-ms")
        );

        g_spikeThresholdMs = std::clamp(g_spikeThresholdMs, 17, 100);
        g_reportCooldownMs = std::clamp(g_reportCooldownMs, 0, 2000);
    }

    void resetLevelCapture() {
        g_frames = {};
        g_inputs = {};
        g_gpuSamples = {};
        g_recentSpikeFrames = {};
        g_frameHead = 0;
        g_frameCount = 0;
        g_inputHead = 0;
        g_inputCount = 0;
        g_gpuHead = 0;
        g_gpuCount = 0;
        g_recentSpikeHead = 0;
        g_recentSpikeCount = 0;
        g_liveCallTimings = {};
        g_previousFrameStart = {};
        g_havePreviousFrame = false;
        g_previousScene = nullptr;
        g_levelFrame = 0;
        g_totalSpikes = 0;
        g_framesCaptured = 0;
        g_gpuResultsAttachedToSpikes = 0;
        g_levelStartedAt = secondsSinceBoot();
        g_lastDetailedReportAt = -10000.0;
        g_previousCaptureCostMs = 0.0;
        g_coalescedReports = 0;
    }

    void registerOwnHookAddress(
        auto& self,
        std::string_view displayName,
        HookTarget target
    ) {
        auto result = self.getHook(displayName);
        if (!result) {
            return;
        }

        auto* hook = result.unwrap();
        g_hookTargetAddresses[static_cast<std::size_t>(target)] =
            hook->getAddress();
    }

    void setFirstPriority(auto& self, std::string_view displayName) {
        (void) self.setHookPriorityPre(displayName, Priority::First);
    }
}

class $modify(CulpritFinderPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        resetLevelCapture();
        g_levelId = level
            ? static_cast<std::int64_t>(level->m_levelID)
            : 0;
        g_levelName = level
            ? std::string(level->m_levelName)
            : std::string("unknown");

        appendFileRecord(fmt::format(
            "\n\nLEVEL START: {} | id={} | target={:.1f}Hz | threshold={}ms",
            g_levelName,
            g_levelId,
            displayRefreshRate(),
            g_spikeThresholdMs
        ));

        return true;
    }

    void onQuit() {
        g_overlayLabel = nullptr;
        g_havePreviousFrame = false;
        g_previousScene = nullptr;

        appendFileRecord(fmt::format(
            "LEVEL END: {} | id={} | total-spikes={} | captured-frames={} | "
            "completed-gpu-results-attached-to-spikes={}",
            g_levelName,
            g_levelId,
            g_totalSpikes,
            g_framesCaptured,
            g_gpuResultsAttachedToSpikes
        ));

        PlayLayer::onQuit();
    }
};

class $modify(CulpritFinderDirector, cocos2d::CCDirector) {
    static void onModify(auto& self) {
        registerOwnHookAddress(
            self, "CCDirector::drawScene", HookTarget::DirectorDraw
        );
    }

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
            g_liveCallTimings = {};
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

        // Update hooks run before drawScene in the normal Cocos frame loop.
        // Snapshot their per-frame totals before the next update begins.
        const CallTimingArray frameCalls = g_liveCallTimings;
        g_liveCallTimings = {};

        double gpuPollMs = 0.0;
        const auto gpuPollStarted = Clock::now();

        if (g_gpuTimerEnabled) {
            initializeGpuTimer();
            pollGpuQueries();
        }

        gpuPollMs = milliseconds(Clock::now() - gpuPollStarted);

        const double rawEngineDelta =
            static_cast<double>(this->getDeltaTime());
        const double engineDeltaMs =
            std::isfinite(rawEngineDelta) && rawEngineDelta > 0.0
                ? rawEngineDelta * 1000.0
                : 0.0;

        const std::uint64_t thisFrame = g_levelFrame + 1;

        if (g_gpuTimerEnabled) {
            beginGpuQuery(thisFrame);
        }

        const auto drawStarted = Clock::now();
        CCDirector::drawScene();
        const double drawSceneMs =
            milliseconds(Clock::now() - drawStarted);

        if (g_gpuTimerEnabled) {
            endGpuQuery();
        }

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
        const bool drawSpike = drawSceneMs >= budget;
        const bool isSpike = gapSpike || longInterruption;

        FrameSample sample{
            g_levelFrame,
            rawGapMs,
            adjustedGapMs,
            priorDiagnosticMs,
            drawSceneMs,
            engineDeltaMs,
            gpuPollMs,
            -1.0,
            0,
            gapSpike || longInterruption,
            drawSpike,
            frameCalls
        };

        recordFrame(sample);

        if (isSpike) {
            const auto diagnosticStarted = Clock::now();
            captureSpike(sample, nowSeconds);
            g_previousCaptureCostMs =
                milliseconds(Clock::now() - diagnosticStarted);
        }
    }
};

$on_mod(Loaded) {
    readSettings();
    initializeCaptureFile();

    listenForSettingChanges<bool>("enabled", [](bool value) {
        g_enabled = value;
        g_havePreviousFrame = false;
        g_previousCaptureCostMs = 0.0;
        appendFileRecord(fmt::format(
            "SETTING: frame capture {}",
            value ? "enabled" : "disabled"
        ));
    });

    listenForSettingChanges<bool>("show-overlay", [](bool value) {
        g_showOverlay = value;
        if (g_overlayLabel) {
            g_overlayLabel->setVisible(false);
        }
    });

    listenForSettingChanges<bool>("platform-snapshot", [](bool value) {
        g_includePlatformSnapshot = value;
    });

    listenForSettingChanges<bool>("frame-timeline", [](bool value) {
        g_verboseTimeline = value;
    });

    listenForSettingChanges<bool>("hook-inventory", [](bool value) {
        g_includeHookInventory = value;
    });

    listenForSettingChanges<bool>("gpu-timer", [](bool value) {
        g_gpuTimerEnabled = value;
    });

    listenForSettingChanges<std::int64_t>("spike-threshold-ms", [](std::int64_t value) {
        g_spikeThresholdMs = static_cast<int>(
            std::clamp<std::int64_t>(value, 17, 100)
        );
    });

    listenForSettingChanges<std::int64_t>("report-cooldown-ms", [](std::int64_t value) {
        g_reportCooldownMs = static_cast<int>(
            std::clamp<std::int64_t>(value, 0, 2000)
        );
    });

    log::info(
        "[CulpritFinder] loaded | threshold={}ms | file-ready={} | "
        "GPU timer is runtime-detected and asynchronous",
        g_spikeThresholdMs,
        g_captureFileReady
    );
}
