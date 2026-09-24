#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

class RouteGamepadControls {
public:
    struct Result {
        int direction = 0; // left=-1, right=1, up=-2, down=2
        bool activate = false, exit = false, beginDraw = false, commitDraw = false, cancelDraw = false,
            togglePoint = false;
        double dx = 0, dy = 0;
    };
    void Reset() { *this = {}; }
    static double CursorAxis(double value) {
        constexpr double deadzone = .35;
        const double magnitude = std::abs(value);
        if (magnitude <= deadzone) return 0;
        return std::copysign(std::clamp((magnitude - deadzone) / (1.0 - deadzone), 0.0, 1.0), value);
    }
    Result Update(unsigned buttons, double x, double y, bool otherInput, bool cursorMode,
        bool manualStart, std::int64_t now, bool pointMode = false) {
        constexpr unsigned A = 0x1000, B = 0x2000, directions = 15, leftStickClick = 0x0040;
        Result result;
        const bool neutral = !buttons && !otherInput && std::abs(x) < .35 && std::abs(y) < .35;
        if ((lastAt_ >= 0 && (now < lastAt_ || now - lastAt_ > 250)) ||
            cursorMode != cursorMode_ || pointMode != pointMode_) {
            result.cancelDraw = drawing_ || pendingPoint_; waiting_ = true;
            drawing_ = pendingA_ = pendingB_ = pendingPoint_ = false;
        }
        const auto delta = lastAt_ < 0 ? 0 : std::clamp<std::int64_t>(now - lastAt_, 0, 100);
        lastAt_ = now; cursorMode_ = cursorMode; pointMode_ = pointMode;
        if (waiting_) {
            if (neutral) { waiting_ = false; pendingPoint_ = false; }
            return result;
        }
        const auto allowedButtons = A | B | directions | (cursorMode && pointMode ? leftStickClick : 0);
        if (otherInput || (buttons & ~allowedButtons) || ((buttons & A) && (buttons & B))) {
            result.cancelDraw = drawing_ || pendingPoint_;
            drawing_ = pendingA_ = pendingB_ = pendingPoint_ = false; waiting_ = true; return result;
        }
        if (buttons & B) {
            pendingB_ = true; result.cancelDraw = drawing_ || pendingPoint_;
            drawing_ = pendingA_ = pendingPoint_ = false; return result;
        }
        if (pendingB_) { pendingB_ = false; waiting_ = true; result.exit = true; return result; }
        if (cursorMode) {
            result.dx = CursorAxis(x) * delta / 1000.0;
            result.dy = -CursorAxis(y) * delta / 1000.0;
            if (buttons & 4) result.dx = -delta / 1000.0;
            if (buttons & 8) result.dx = delta / 1000.0;
            if (buttons & 1) result.dy = -delta / 1000.0;
            if (buttons & 2) result.dy = delta / 1000.0;
            if (pointMode) {
                if (buttons & A) {
                    if (!pendingPoint_) { pendingPoint_ = true; result.togglePoint = true; }
                } else pendingPoint_ = false;
                return result;
            }
            if (buttons & A) {
                if (!drawing_) { drawing_ = true; beganAt_ = now; result.beginDraw = true; }
            } else if (drawing_) {
                drawing_ = false; result.commitDraw = manualStart || now - beganAt_ >= 250;
                result.cancelDraw = !result.commitDraw; waiting_ = true;
            }
            return result;
        }
        int direction = 0;
        if (buttons & 4 || x <= -.55) direction = -1;
        else if (buttons & 8 || x >= .55) direction = 1;
        else if (buttons & 1 || y >= .55) direction = -2;
        else if (buttons & 2 || y <= -.55) direction = 2;
        if (buttons & A) {
            if (direction) { pendingA_ = false; waiting_ = true; }
            else pendingA_ = true;
            return result;
        }
        if (pendingA_) { pendingA_ = false; waiting_ = true; result.activate = neutral; return result; }
        if (!direction) { repeatedDirection_ = 0; return result; }
        if (direction != repeatedDirection_ || now >= repeatAt_) {
            result.direction = direction; repeatAt_ = now + (direction != repeatedDirection_ ? 380 : 140);
            repeatedDirection_ = direction;
        }
        return result;
    }
private:
    bool waiting_ = true, cursorMode_ = false, pointMode_ = false, pendingA_ = false, pendingB_ = false,
        pendingPoint_ = false, drawing_ = false;
    std::int64_t lastAt_ = -1, beganAt_ = 0, repeatAt_ = 0;
    int repeatedDirection_ = 0;
};
