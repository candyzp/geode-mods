#import <UIKit/UIKit.h>

#include "RefreshRate.hpp"

namespace dashboost {
    double platformRefreshRate() {
        static const double refreshRate = [] {
            UIScreen* screen = UIScreen.mainScreen;
            NSInteger fps = screen.maximumFramesPerSecond;
            return fps > 0 ? static_cast<double>(fps) : 60.0;
        }();

        return refreshRate;
    }
}
