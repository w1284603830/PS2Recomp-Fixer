#pragma once

namespace ps2host
{
enum class PresentationPhase { Start, End };

// EE-thread-owned host submission policy. It never alters guest IRQs or cycles.
// Movie begin/end changes after Start affect the next field, not this field.
class FieldPresentation
{
    PresentationPhase phase = PresentationPhase::Start;
    bool movie = false;
    bool pendingEnd = false;
public:
    void configure(PresentationPhase value) { phase = value; }
    void setMovieActive(bool active) { movie = active; }
    void reset() { phase = PresentationPhase::Start; movie = pendingEnd = false; }
    void restartFieldState() { movie = pendingEnd = false; } // run restarts retain post-load configuration
    bool atStart()
    {
        pendingEnd = phase == PresentationPhase::End && !movie;
        return !pendingEnd;
    }
    bool atEnd()
    {
        const bool present = pendingEnd;
        pendingEnd = false;
        return present;
    }
};
}
