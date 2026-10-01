#include "RefreshRate.hpp"

#if defined(GEODE_IS_WINDOWS)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace dashboost {
    double platformRefreshRate() {
        static const double refreshRate = [] {
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);

            if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
                return 60.0;
            }

            double rate = static_cast<double>(mode.dmDisplayFrequency);
            if (mode.dmDisplayFlags & DM_INTERLACED) {
                rate *= 2.0;
            }

            return rate > 0.0 ? rate : 60.0;
        }();

        return refreshRate;
    }
}

#elif !defined(GEODE_IS_IOS)

namespace dashboost {
    double platformRefreshRate() {
        return 60.0;
    }
}

#endif
