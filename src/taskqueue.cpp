#include "taskqueue.h"

#include <QMutex>
#include <QThread>
#include <condition_variable>
#include <deque>
#include <mutex>

// Worker thread: pops submitted tasks off a FIFO and runs them serially, off the
// UI thread. The queue's mutex guards the task list; the condition variable
// wakes the worker when work arrives or when it should stop.
class TaskWorker : public QThread {
public:
    TaskWorker(std::function<qint64()> next, std::function<void(qint64)> run,
               std::function<void()> shutdown)
        : next_(std::move(next)), run_(std::move(run)), shutdown_(std::move(shutdown)) {}

protected:
    void run() override {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cond_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) break;
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            job();
        }
        shutdown_();
    }

public:
    std::mutex mutex_;
    std::condition_variable cond_;
    std::deque<std::function<void()>> queue_;
    bool stop_ = false;
    std::function<qint64()> next_;
    std::function<void(qint64)> run_;
    std::function<void()> shutdown_;
};

struct TaskQueue::Impl {
    QMutex mutex;             // guards tasks_ and nextId_
    QVector<Task> tasks_;
    qint64 nextId = 1;
    std::unique_ptr<TaskWorker> worker;
    // Retain only the most recent completed tasks so a long-running session
    // never accumulates unbounded result lists. Auto-record submits one task per
    // observed file change; each Task holds a lambda capturing this + asset
    // data, so unbounded retention is a slow memory leak (P-105 audit 2026-11-02).
    static constexpr int kMaxCompletedTasks = 64;

    // Called with mutex held. The auto-record path submits one task per change
    // and the UI only reads a task's status while it is still relevant (the
    // per-item result is drained on taskFinished), so keeping only a bounded
    // tail of completed tasks is safe and never drops a still-relevant status.
    void pruneCompletedLocked() {
        int completed = 0;
        for (auto it = tasks_.rbegin(); it != tasks_.rend(); ++it)
            if (it->done) ++completed;
        int excess = completed - kMaxCompletedTasks;
        if (excess <= 0) return;
        for (auto it = tasks_.begin(); it != tasks_.end() && excess > 0;) {
            if (it->done) { it = tasks_.erase(it); --excess; }
            else ++it;
        }
    }

    TaskWorker *makeWorker(TaskQueue *owner) {
        worker = std::make_unique<TaskWorker>(
            [this]() -> qint64 { QMutexLocker lock(&mutex); return nextId++; },
            [owner](qint64 id) { owner->runTask(id); },
            []() {});
        return worker.get();
    }
};

TaskQueue::TaskQueue(QObject *parent) : QObject(parent), impl_(new Impl) {
    auto *worker = impl_->makeWorker(this);
    worker->start();
}

TaskQueue::~TaskQueue() {
    {
        std::lock_guard<std::mutex> lock(impl_->worker->mutex_);
        impl_->worker->stop_ = true;
    }
    impl_->worker->cond_.notify_all();
    impl_->worker->wait();
}

qint64 TaskQueue::submit(const QString &label, TaskFn fn, bool write) {
    Q_UNUSED(write); // V1 serializes all tasks; writer-vs-read overlap is impossible by construction.
    qint64 id;
    {
        QMutexLocker lock(&impl_->mutex);
        id = impl_->nextId++;
        Task task;
        task.id = id;
        task.label = label;
        task.fn = std::move(fn);
        impl_->tasks_.push_back(std::move(task));
    }
    {
        std::lock_guard<std::mutex> lock(impl_->worker->mutex_);
        impl_->worker->queue_.push_back([this, id] { runTask(id); });
    }
    impl_->worker->cond_.notify_one();
    return id;
}

void TaskQueue::cancel(qint64 id) {
    QMutexLocker lock(&impl_->mutex);
    for (auto &task : impl_->tasks_) {
        if (task.id == id) {
            task.cancelled->store(true);
            return;
        }
    }
}

void TaskQueue::runTask(qint64 id) {
    Task task;
    {
        QMutexLocker lock(&impl_->mutex);
        for (auto &t : impl_->tasks_) {
            if (t.id == id) {
                task = t;
                t.running = true;
                break;
            }
        }
    }
    if (!task.fn) return;
    Progress progress;
    progress.cancelled_ = task.cancelled;
    QString error;
    bool ok = false;
    try {
        task.fn(progress);
        ok = !progress.cancelled();
        if (!ok) error = QStringLiteral("已取消");
    } catch (const std::exception &e) {
        ok = false;
        error = QString::fromLocal8Bit(e.what());
    } catch (...) {
        ok = false;
        error = QStringLiteral("未知错误");
    }
    {
        QMutexLocker lock(&impl_->mutex);
        for (auto &t : impl_->tasks_) {
            if (t.id == id) {
                t.running = false;
                t.done = true;
                t.percent = progress.percent();
                t.items = progress.items();
            }
        }
        impl_->pruneCompletedLocked();
    }
    emit taskUpdated(id, progress.percent(), progress.current());
    emit taskFinished(id, ok, error);
}

QVector<TaskQueue::Status> TaskQueue::statuses() const {
    QMutexLocker lock(&impl_->mutex);
    QVector<Status> out;
    for (const auto &t : impl_->tasks_)
        out.push_back({t.id, t.label, t.percent, t.running, t.done, t.items});
    return out;
}
