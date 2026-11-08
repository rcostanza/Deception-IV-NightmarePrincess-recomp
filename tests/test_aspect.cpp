// SPDX-License-Identifier: GPL-2.0-or-later
// The aspect ratio setting (deception_settings.h): which shape a size is, the game's width for it, and
// what the fullscreen modes pick from a screen.
#include <cassert>
#include <cstdio>
#include <utility>
#include <vector>

#include "../gpu/shim/deception_settings.h"

using namespace DeceptionSettings;
using Size = std::pair<int, int>;

int main() {
    // Shapes: 2560x1080 and 3440x1440 count as 21:9, 1366x768 as 16:9, 16:10 as none.
    assert(AspectRatioOf(1920, 1080) == Aspect16x9 && AspectRatioOf(1366, 768) == Aspect16x9);
    assert(AspectRatioOf(2560, 1080) == Aspect21x9 && AspectRatioOf(3440, 1440) == Aspect21x9);
    assert(AspectRatioOf(3840, 1080) == Aspect32x9 && AspectRatioOf(5120, 1440) == Aspect32x9);
    assert(AspectRatioOf(1600, 1200) == Aspect4x3 && AspectRatioOf(1920, 1200) == -1);
    assert(AspectLabel(2560, 1080) == "21:9" && AspectLabel(1920, 1200) == "16:10");

    // The game's width at 2160 lines: 16:9 stays the PS4 Pro's, others follow the size.
    assert(WideWidth4k(1920, 1080) == 3840 && WideWidth4k(1366, 768) == 3840);
    assert(WideWidth4k(2560, 1080) == 5120 && WideWidth4k(3840, 1080) == 7680);
    assert(WideWidth4k(1600, 1200) == 2880 && WideWidth4k(1920, 1200) == 3456);
    assert(WideWidth4k(1280, 1024) == 2880); /* 5:4: no narrower than 4:3 */

    // Fitting a shape in a screen: the screen itself when it has it, else bars.
    assert((FitAspect(2560, 1440, Aspect16x9) == Size{2560, 1440}));
    assert((FitAspect(2560, 1440, Aspect21x9) == Size{2560, 1098}));
    assert((FitAspect(2560, 1440, Aspect32x9) == Size{2560, 720}));
    assert((FitAspect(3440, 1440, Aspect16x9) == Size{2560, 1440}));
    assert((FitAspect(3440, 1440, Aspect21x9) == Size{3440, 1440}));
    assert((FitAspect(1920, 1200, Aspect16x9) == Size{1920, 1080}));
    assert((FitAspect(2560, 1440, Aspect4x3) == Size{1920, 1440}));

    // Borderless keeps the desktop; exclusive takes the largest mode of the shape, else the desktop.
    const std::vector<Size> modes169 = {{2560, 1440}, {1920, 1080}, {1280, 720}};
    const std::vector<Size> modes219 = {{3440, 1440}, {2560, 1080}, {1920, 1080}};
    FullscreenChoice c = PickFullscreen(false, Aspect21x9, {2560, 1440}, modes169);
    assert((c.mode == Size{0, 0} && c.picture == Size{2560, 1098}));
    c = PickFullscreen(true, Aspect21x9, {2560, 1440}, modes169);
    assert((c.mode == Size{0, 0} && c.picture == Size{2560, 1098}));
    c = PickFullscreen(true, Aspect16x9, {3440, 1440}, modes219);
    assert((c.mode == Size{1920, 1080} && c.picture == Size{1920, 1080}));
    c = PickFullscreen(true, Aspect21x9, {3440, 1440}, modes219);
    assert((c.mode == Size{3440, 1440} && c.picture == Size{3440, 1440}));
    c = PickFullscreen(false, Aspect16x9, {3440, 1440}, modes219);
    assert((c.mode == Size{0, 0} && c.picture == Size{2560, 1440}));

    // A window moving to another shape takes the size nearest in height.
    assert((NearestHeight({{3440, 1440}, {2560, 1080}, {1720, 720}}, 1080) == Size{2560, 1080}));
    assert((NearestHeight({{3840, 1080}, {2560, 720}}, 1440) == Size{3840, 1080}));
    assert((NearestHeight({}, 1080) == Size{0, 0}));
    std::puts("aspect: ok");
    return 0;
}
