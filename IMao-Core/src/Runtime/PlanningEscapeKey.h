#pragma once

namespace AutoRoute {
enum class EscapeAction { PassThrough, Consume, CancelGesture, ReturnToPan };

// Ownership follows the first key-down. Never steal repeats of an Esc which
// began in another app/mode, and always consume an owned matching key-up.
class PlanningEscapeKey {
public:
    EscapeAction Handle(bool down, bool planning, bool focused, bool hasGesture) {
        if (!down) {
            pressed_ = false;
            const bool owned = owned_;
            owned_ = false;
            return owned ? EscapeAction::Consume : EscapeAction::PassThrough;
        }
        const bool first = !pressed_;
        pressed_ = true;
        if (owned_) return EscapeAction::Consume;
        if (!first || !planning || !focused) return EscapeAction::PassThrough;
        owned_ = true;
        return hasGesture ? EscapeAction::CancelGesture : EscapeAction::ReturnToPan;
    }
    bool IsPressed() const { return pressed_; }
    void Reset() { pressed_ = owned_ = false; }
private:
    bool pressed_ = false, owned_ = false;
};
} // namespace AutoRoute
