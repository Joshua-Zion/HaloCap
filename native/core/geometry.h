#pragma once

#include <algorithm>

namespace halocap::core {

struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

struct Size {
    float width = 0.0f;
    float height = 0.0f;
};

// 设备像素坐标：裁剪与脏区用，和上面浮点几何分开是因为取整方向不能含糊。
struct IntRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    bool IsEmpty() const { return right <= left || bottom <= top; }
};

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    bool operator==(const Color& other) const {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
};

struct Rect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    static Rect FromTopLeft(Point origin, Size size) {
        return Rect{ origin.x, origin.y, origin.x + size.width, origin.y + size.height };
    }

    bool IsEmpty() const { return right <= left || bottom <= top; }

    float Width() const { return right - left; }

    float Height() const { return bottom - top; }

    Rect Normalized() const {
        return Rect{ std::min(left, right), std::min(top, bottom),
                     std::max(left, right), std::max(top, bottom) };
    }

    bool Contains(Point p) const {
        const Rect r = Normalized();
        return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
    }

    Rect Union(Rect other) const {
        const Rect a = Normalized();
        const Rect b = other.Normalized();
        if (a.IsEmpty()) {
            return b.IsEmpty() ? Rect{} : b;
        }
        if (b.IsEmpty()) {
            return a;
        }
        return Rect{ std::min(a.left, b.left), std::min(a.top, b.top),
                     std::max(a.right, b.right), std::max(a.bottom, b.bottom) };
    }

    Rect Intersect(Rect other) const {
        const Rect a = Normalized();
        const Rect b = other.Normalized();
        Rect result{ std::max(a.left, b.left), std::max(a.top, b.top),
                     std::min(a.right, b.right), std::min(a.bottom, b.bottom) };
        if (result.IsEmpty()) {
            return Rect{};
        }
        return result;
    }

    bool operator==(const Rect& other) const {
        return left == other.left && top == other.top && right == other.right && bottom == other.bottom;
    }
};

}
