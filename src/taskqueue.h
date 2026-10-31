#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

// Background task queue (P-105). Long-running work (import, hashing, thumbnails,
// restore) runs off the UI thread; the UI never blocks. One worker thread runs
// tasks strictly serially — the simplest correct model: a single writer (import/
// snapshot/restore/backup) is trivially guaranteed, and no read ever overlaps an
// unfinished write. Parallel reads are a future optimization, not a V1 promise.
// Cancellation is cooperative: the task function checks `cancelled` at safe
// per-item boundaries and stops; completed items are kept, never silently lost.
class TaskQueue : public QObject {
    Q_OBJECT
public:
    struct Item { QString label; bool ok = false; QString error; };

    // Live progress passed to the running task function (worker thread).
    class Progress {
    public:
        void setPercent(int percent) { percent_ = percent; }
        int percent() const { return percent_; }
        void setCurrent(const QString &label) { current_ = label; }
        QString current() const { return current_; }
        void addItem(const Item &item) { items_.push_back(item); }
        QVector<Item> items() const { return items_; }
        bool cancelled() const { return cancelled_->load(); }
    private:
        friend class TaskQueue;
        std::shared_ptr<std::atomic_bool> cancelled_ = std::make_shared<std::atomic_bool>(false);
        int percent_ = 0;
        QString current_;
        QVector<Item> items_;
    };

    using TaskFn = std::function<void(Progress &)>;

    explicit TaskQueue(QObject *parent = nullptr);
    ~TaskQueue();

    // Queue a task. write=true marks a writer task (import/snapshot/restore/
    // backup); the queue serializes all tasks, so a writer never overlaps a read.
    qint64 submit(const QString &label, TaskFn fn, bool write = true);
    // Cooperative cancel: observed at the next item boundary by the task fn.
    void cancel(qint64 id);
    // Snapshot of every submitted task's status/results (the result list).
    struct Status { qint64 id = 0; QString label; int percent = 0; bool running = false; bool done = false; QVector<Item> items; };
    QVector<Status> statuses() const;

signals:
    void taskUpdated(qint64 id, int percent, const QString &currentItem);
    void taskFinished(qint64 id, bool ok, const QString &error);

private:
    struct Task {
        qint64 id = 0;
        QString label;
        TaskFn fn;
        std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
        int percent = 0;
        QString current;
        QVector<Item> items;
        bool running = false;
        bool done = false;
    };
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void runTask(qint64 id);
};
