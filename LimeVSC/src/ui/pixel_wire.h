#pragma once

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace lime {

inline constexpr float kLevelTolerance = 6.0f;

template <class EmitRect>
void buildPixelWire(ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, float cell,
                    float thickness, EmitRect emit) {
    if (!(cell > 0.0f)) cell = 1.0f;
    const float fat = std::max(0.0f, (std::max(cell, thickness) - cell) * 0.5f);

    if (p3.x > p0.x && std::fabs(p3.y - p0.y) <= kLevelTolerance)
        p0.y = p1.y = p2.y = p3.y = (p0.y + p3.y) * 0.5f;

    bool have = false;
    int  x0 = 0, x1 = 0, y0 = 0, y1 = 0;

    const auto flush = [&] {
        if (!have) return;
        emit(ImVec2(static_cast<float>(x0) * cell - fat,
                    static_cast<float>(y0) * cell - fat),
             ImVec2(static_cast<float>(x1 + 1) * cell + fat,
                    static_cast<float>(y1 + 1) * cell + fat));
        have = false;
    };

    const auto push = [&](int x, int y) {
        if (!have) {
            x0 = x1 = x;
            y0 = y1 = y;
            have = true;
            return;
        }
        if (x >= x0 && x <= x1 && y >= y0 && y <= y1) return;

        const bool single = (x0 == x1 && y0 == y1);
        if ((single || y0 == y1) && y == y0) {
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            return;
        }
        if ((single || x0 == x1) && x == x0) {
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
            return;
        }
        flush();
        x0 = x1 = x;
        y0 = y1 = y;
        have = true;
    };

    bool hasPrev = false;
    int  prevX = 0, prevY = 0;
    const auto walkTo = [&](int x, int y) {
        if (!hasPrev) {
            push(x, y);
            prevX = x;
            prevY = y;
            hasPrev = true;
            return;
        }
        const int dx = std::abs(x - prevX);
        const int dy = std::abs(y - prevY);
        const int sx = prevX < x ? 1 : -1;
        const int sy = prevY < y ? 1 : -1;
        int err = dx - dy;
        while (prevX != x || prevY != y) {
            const int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; prevX += sx; }
            if (e2 < dx)  { err += dx; prevY += sy; }
            push(prevX, prevY);
        }
    };

    const float extent = std::fabs(p1.x - p0.x) + std::fabs(p1.y - p0.y)
                         + std::fabs(p2.x - p1.x) + std::fabs(p2.y - p1.y)
                         + std::fabs(p3.x - p2.x) + std::fabs(p3.y - p2.y);
    const int steps = std::clamp(static_cast<int>(extent / cell), 32, 4096);

    const auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float ax = mix(p0.x, p1.x, t), ay = mix(p0.y, p1.y, t);
        const float bx = mix(p1.x, p2.x, t), by = mix(p1.y, p2.y, t);
        const float cx = mix(p2.x, p3.x, t), cy = mix(p2.y, p3.y, t);
        const float dx = mix(ax, bx, t), dy = mix(ay, by, t);
        const float ex = mix(bx, cx, t), ey = mix(by, cy, t);
        walkTo(static_cast<int>(std::floor(mix(dx, ex, t) / cell)),
               static_cast<int>(std::floor(mix(dy, ey, t) / cell)));
    }
    flush();
}

}
