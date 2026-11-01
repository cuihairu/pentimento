#pragma once
#include <QDateTime>
#include <QString>

// P-305: runtime-only whole-tree snapshot schedule (no system service). Off by
// default; periods 15/30/60 minutes; the next run is the enable point plus one
// period. A pause records its reason (user or system — a system pause is lifted
// when the app returns to the foreground, a user pause only by the user). An
// overlap never stacks (one run in flight); missed periods collapse into a
// single catch-up run because the next time is always recomputed from now.
// Leaving the app pauses nothing here by itself — it simply stops ticking,
// which is why the UI states that exiting is not counted as continuous backup.
class SnapshotSchedule {
public:
    // Enable or disable. Enabling (or changing) the period restarts the clock:
    // the next run is `now` plus one period, and any pause state is cleared.
    void setPeriod(int minutes, const QDateTime &now) {
        periodMin_ = minutes;
        paused_ = false;
        systemPaused_ = false;
        reason_.clear();
        next_ = minutes > 0 ? now.addSecs(qint64(minutes) * 60) : QDateTime();
    }
    int period() const { return periodMin_; }
    bool enabled() const { return periodMin_ > 0; }
    bool paused() const { return paused_; }
    bool pausedBySystem() const { return systemPaused_; }
    const QString &pauseReason() const { return reason_; }
    QDateTime next() const { return next_; }
    bool inProgress() const { return inProgress_; }

    // A system pause carries a fixed reason; resuming on return to the
    // foreground only lifts a system pause, never a user pause.
    void pauseBySystem(const QString &reason) {
        if (periodMin_ <= 0) return;
        paused_ = true;
        systemPaused_ = true;
        reason_ = reason;
    }
    void pauseByUser(const QString &reason) {
        if (periodMin_ <= 0) return;
        paused_ = true;
        systemPaused_ = false;
        reason_ = reason;
    }
    // Resume restarts the clock from `now` regardless of who paused.
    void resume(const QDateTime &now) {
        if (periodMin_ <= 0) return;
        paused_ = false;
        systemPaused_ = false;
        reason_.clear();
        next_ = now.addSecs(qint64(periodMin_) * 60);
    }

    // A run is due when enabled, unpaused, past the next time, and no run is in
    // flight — an overlap merges (skips), never stacks.
    bool due(const QDateTime &now) const {
        return periodMin_ > 0 && !paused_ && !inProgress_ && now >= next_;
    }
    // Start the due run. The next time is recomputed from `now`, so several
    // missed periods (sleep/suspend) collapse into this one catch-up run.
    void beginRun(const QDateTime &now) {
        inProgress_ = true;
        next_ = now.addSecs(qint64(periodMin_) * 60);
    }
    void endRun() { inProgress_ = false; }

private:
    int periodMin_ = 0;
    QDateTime next_;
    bool paused_ = false;
    bool systemPaused_ = false;
    QString reason_;
    bool inProgress_ = false;
};
