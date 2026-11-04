#include "taskqueue.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// Background worker: pops submitted tasks off a FIFO and runs them serially, off
// the UI thread. The queue's mutex guards the task list and the job queue; the
// condition variable wakes the worker when work arrives or when it should stop.
//
// The worker is a std::thread so the owning TaskQueue can either join it on
// shutdown (blockOnShutdown=true, the default: all queued work drains before the
// queue dies) or detach it (blockOnShutdown=false: a task blocked in a long
// syscall such as statvfs on a dead mount never freezes the teardown thread).
// The detached worker keeps the shared Impl alive and is guarded by the alive
// flag so it never emits into a destroyed TaskQueue.
struct TaskQueue::Impl {
    std::mutex mutex;             // guards tasks_, nextId_, stop_ and queue_
    std::condition_variable cond;
    std::deque<std::function<void()>> queue_;
    QVector<Task> tasks_;
    qint64 nextId = 1;
    bool stop_ = false;
    std::thread worker;
    // Guards `alive` and any signal emission back into the owning TaskQueue.
    // The destructor sets alive=false under this mutex before the QObject dies,
    // so a detached worker never emits into a destroyed queue.
    std::mutex emitMutex;
    bool alive = true;
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

    void loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cond.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) break;
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            job();
        }
    }
};

TaskQueue::TaskQueue(QObject *parent, bool blockOnShutdown)
    : QObject(parent), impl_(std::make_shared<Impl>()), blockOnShutdown_(blockOnShutdown) {
    const std::shared_ptr<Impl> impl = impl_;
    impl->worker = std::thread([impl] { impl->loop(); });
}

TaskQueue::~TaskQueue() {
    // Prevent any in-flight or queued task from emitting into this QObject as it
    // dies. The emitMutex also serialises against the worker's own emit section,
    // so the worker never touches `this` after the object memory is freed.
    {
        std::lock_guard<std::mutex> lock(impl_->emitMutex);
        impl_->alive = false;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stop_ = true;
    }
    impl_->cond.notify_all();
    if (blockOnShutdown_) {
        if (impl_->worker.joinable()) impl_->worker.join();
    } else {
        if (impl_->worker.joinable()) impl_->worker.detach();
    }
}

qint64 TaskQueue::submit(const QString &label, TaskFn fn, bool write) {
    Q_UNUSED(write); // V1 serializes all tasks; writer-vs-read overlap is impossible by construction.
    qint64 id;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        id = impl_->nextId++;
        Task task;
        task.id = id;
        task.label = label;
        task.fn = std::move(fn);
        impl_->tasks_.push_back(std::move(task));
        const std::shared_ptr<Impl> impl = impl_;
        impl_->queue_.push_back([impl, self = this, id] { TaskQueue::runTask(impl, self, id); });
    }
    impl_->cond.notify_one();
    return id;
}

void TaskQueue::cancel(qint64 id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto &task : impl_->tasks_) {
        if (task.id == id) {
            task.cancelled->store(true);
            return;
        }
    }
}

void TaskQueue::runTask(const std::shared_ptr<Impl> &impl, TaskQueue *self, qint64 id) {
    Task task;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        for (auto &t : impl->tasks_) {
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
        std::lock_guard<std::mutex> lock(impl->mutex);
        for (auto &t : impl->tasks_) {
            if (t.id == id) {
                t.running = false;
                t.done = true;
                t.percent = progress.percent();
                t.items = progress.items();
            }
        }
        impl->pruneCompletedLocked();
    }
    // Emit only while the owning queue is alive; the emitMutex serialises this
    // against the destructor, so the worker never emits into a destroyed queue.
    std::lock_guard<std::mutex> emitLock(impl->emitMutex);
    if (!impl->alive || !self) return;
    emit self->taskUpdated(id, progress.percent(), progress.current());
    emit self->taskFinished(id, ok, error);
}

QVector<TaskQueue::Status> TaskQueue::statuses() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    QVector<Status> out;
    for (const auto &t : impl_->tasks_)
        out.push_back({t.id, t.label, t.percent, t.running, t.done, t.items});
    return out;
}
