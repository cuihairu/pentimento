// P-004 / D-23 evidence: end-to-end search latency through the real MainWindow
// search box (keystroke -> visible results), plus a coarse UI-thread responsiveness
// probe. The core harness (bench_matrix.cpp) times a cold fact rebuild per query;
// the UI caches facts on reload and applies filters in memory. D-23 asks for both
// the core measurement and the user-visible wait, so this harness supplies the
// latter. Offscreen only; deliberately NOT registered with ctest (needs a
// prebuilt sample library with 2,000+ assets).
//
// Build: cmake --build --preset dev --target pentimento_bench_ui
// Run:   ./build/dev/pentimento_bench_ui bench/run/lib [queries]

#include "library.h"
#include "mainwindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QListWidget>
#include <QLineEdit>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

QLineEdit *findSearchBox(QWidget &window) {
    const auto edits = window.findChildren<QLineEdit *>();
    for (auto *edit : edits)
        if (edit->accessibleName() == QStringLiteral("搜索素材")
            || edit->placeholderText() == QStringLiteral("搜索素材名称"))
            return edit;
    return nullptr;
}

QListWidget *findAssetList(QWidget &window) {
    const auto lists = window.findChildren<QListWidget *>();
    for (auto *list : lists)
        if (list->accessibleName() == QStringLiteral("素材列表"))
            return list;
    return nullptr;
}

double percentile95(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t index = size_t(0.95 * double(values.size() - 1) + 0.5);
    return values[index];
}

double average(const std::vector<double> &values) {
    if (values.empty()) return 0.0;
    double sum = 0.0;
    for (double v : values) sum += v;
    return sum / double(values.size());
}

} // namespace

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <library-root> [queries]\n", argv[0]);
        return 2;
    }
    const QString root = QString::fromLocal8Bit(argv[1]);
    const int queries = argc >= 3 ? qMax(1, atoi(argv[2])) : 120;

    QElapsedTimer startupTimer;
    startupTimer.start();
    Library library(root);
    MainWindow window(library);
    printf("startup (open library + reload assets) %.1f s\n",
           double(startupTimer.elapsed()) / 1000.0);

    QLineEdit *search = findSearchBox(window);
    QListWidget *list = findAssetList(window);
    if (!search || !list) {
        fprintf(stderr, "bench ui: required widgets not found\n");
        return 1;
    }
    printf("%d assets in list\n", list->count());
    if (list->count() < 2000) {
        fprintf(stderr, "bench ui: expected 2000+ assets for D-23\n");
        return 1;
    }

    // Probe prefixes taken from real logical paths; every 4th query is a miss.
    std::vector<QString> probes;
    for (int i = 0; i < list->count() && int(probes.size()) < queries; ++i) {
        QString name = list->item(i)->text();
        const int slash = name.lastIndexOf(QLatin1Char('/'));
        if (slash >= 0) name = name.mid(slash + 1);
        if (name.size() < 7) continue;
        probes.push_back(name.left(6 + (i % 5)));
    }
    if (probes.empty()) {
        fprintf(stderr, "bench ui: no usable probe names\n");
        return 1;
    }

    // A 16 ms ticker records how late the UI event loop was allowed to run.
    // It cannot fire while applyFilter blocks the thread; its lateness is a
    // coarse responsiveness signal (no threshold in the draft budget yet).
    std::vector<double> tickDelays;
    QElapsedTimer lastTick;
    lastTick.start();
    QTimer ticker;
    ticker.setInterval(16);
    QObject::connect(&ticker, &QTimer::timeout, [&] {
        tickDelays.push_back(double(lastTick.elapsed()));
        lastTick.restart();
    });
    ticker.start();

    // MainWindow starts two maintenance timers in its constructor (2 s change
    // scan, 5 s sentinel poll). They are real user-visible work on the UI
    // thread, but they obscure the search path itself. Measure both: isolated
    // (timers paused) and full app (timers running) over the same queries.
    QVector<QTimer *> appTimers;
    for (QTimer *timer : window.findChildren<QTimer *>())
        if (timer->isActive()) appTimers << timer;
    printf("  %d active MainWindow timers (change scan / sentinel)\n", appTimers.size());

    const auto runQueries = [&](bool timersOn) {
        for (QTimer *timer : appTimers) {
            if (timersOn) timer->start();
            else timer->stop();
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        tickDelays.clear();
        lastTick.restart();
        std::vector<double> waits;
        std::vector<double> visible;
        for (int i = 0; i < queries; ++i) {
            const QString query = (i % 4 == 3)
                ? QStringLiteral("zzz-no-such-")
                : probes[size_t(i) % probes.size()];
            QElapsedTimer t;
            t.start();
            search->setText(query);      // textChanged -> MainWindow::applyFilter
            QCoreApplication::processEvents(QEventLoop::AllEvents);
            list->repaint();             // include paint of the updated list
            waits.push_back(double(t.elapsed()));
            int hits = 0;
            for (int row = 0; row < list->count(); ++row)
                if (!list->item(row)->isHidden()) ++hits;
            visible.push_back(double(hits));
            // Give the ticker a short window so thread stalls are observable.
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        printf("== end-to-end search (%s, real UI, cached facts) ==\n",
               timersOn ? "full app" : "isolated");
        printf("  %d queries over %d assets\n", queries, list->count());
        printf("  keystroke-to-display average            %7.1f ms\n", average(waits));
        printf("  keystroke-to-display P95                %7.1f ms (draft budget < 200) -> %s\n",
               percentile95(waits), percentile95(waits) < 200.0 ? "PASS" : "MISS");
        printf("  visible results per query (avg/min/max) %7.1f / %d / %d\n",
               average(visible), int(*std::min_element(visible.begin(), visible.end())),
               int(*std::max_element(visible.begin(), visible.end())));
        printf("  timer ticks observed                    %d\n", int(tickDelays.size()));
        if (!tickDelays.empty())
            printf("  tick delay P95 / max                    %7.1f / %7.1f ms\n",
                   percentile95(tickDelays),
                   *std::max_element(tickDelays.begin(), tickDelays.end()));
    };

    runQueries(false); // search path alone
    std::vector<double>().swap(tickDelays);
    runQueries(true);  // real app: periodic maintenance competes with typing
    ticker.stop();
    printf("  (no draft threshold for tick delay; budget awaits user approval)\n");
    return 0;
}
