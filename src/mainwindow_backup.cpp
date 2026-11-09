// Backup and audit flows: backup-package export, the consistency check,
// the space preview and the restore rehearsal drill (LAY-003 split).
#include "mainwindow.h"
#include "compare_canvas.h"
#include "importer.h"
#include "preview.h"
#include "preview_canvas.h"
#include "sentinel.h"
#include "source_connection.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QFileDialog>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>


// P-516: export a consistent full backup package to a local or user-mounted
// NAS directory. The export runs on the background writer queue — that is the
// brief write pause (no import/capture/restore can interleave), and a slow NAS
// never blocks the UI. A COMPLETE marker is written only after every manifest
// object copied and verified, so a half-package is never reported as success;
// the live catalog is never copied (the manifest is a self-contained snapshot).
void MainWindow::exportBackup() {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("选择备份目标目录（须为空或新目录）"));
    if (dir.isEmpty()) return;
    // P-510: a backup target is a separate write acceptance from the source-read
    // state — "can be listed" is never a backup-capability verdict. Probe
    // writability and free space before confirming; a read-only share (or an
    // unreadable/unreachable path) is refused here, not discovered mid-export.
    const BackupTargetInfo target = probeBackupTarget(dir);
    if (!target.writable) {
        QMessageBox::warning(this, tr("备份目标不可用"),
                             tr("所选备份目标不可写：\n%1\n\n%2")
                                 .arg(dir, target.reason));
        return;
    }
    if (QMessageBox::question(this, tr("导出备份包"),
                              tr("将导出一致备份包到：\n%1\n\n"
                                 "备份目标可写，可用空间约 %2。\n"
                                 "导出期间暂停写入；清单与对象全部校验通过后才写入完成标记。\n"
                                 "目标目录必须为空（不静默覆盖既有备份）。\n"
                                 "备份包不共享运行中索引，可移植到其他机器。")
                                  .arg(dir)
                                  .arg(target.freeBytes / (1024.0 * 1024.0), 0, 'f', 1))
        != QMessageBox::Yes)
        return;
    statusBar()->showMessage(tr("正在导出备份包（后台进行，界面可继续操作）…"));
    auto result = std::make_shared<Library::BackupPackage>();
    auto error = std::make_shared<QString>();
    const qint64 id = captureQueue_->submit(QStringLiteral("导出备份包"),
        [this, dir, result, error](TaskQueue::Progress &) {
            try {
                *result = library_.exportBackupPackage(dir);
            } catch (const std::exception &e) {
                *error = QString::fromUtf8(e.what());
            }
        });
    QObject::connect(captureQueue_.get(), &TaskQueue::taskFinished, this,
        [this, id, result, error](qint64 finished, bool, const QString &) {
            if (finished != id) return;
            if (!error->isEmpty()) {
                statusBar()->showMessage(tr("备份包导出失败：%1").arg(*error));
                return;
            }
            statusBar()->showMessage(tr("备份包已导出：%1（%2 个素材，%3 个对象，约 %4 KiB；完成标记已写入）")
                                         .arg(result->packageDir)
                                         .arg(result->entries.size())
                                         .arg(result->objects)
                                         .arg(result->bytes / 1024));
        });
}

// P-601: report-only object/index consistency check. Runs on the background
// queue (it reads every object, so a large store must never block the UI) and
// reports counts plus the truncated problem lists. Nothing is repaired here:
// missing/corrupt/orphan/ioErrors are surfaced so the user can decide (e.g.
// restore from a backup package, or reconcile with a future maintenance step).
void MainWindow::checkConsistency() {
    statusBar()->showMessage(tr("正在执行一致性检查（只读，后台进行）…"));
    auto result = std::make_shared<Library::ConsistencyReport>();
    auto error = std::make_shared<QString>();
    const qint64 id = captureQueue_->submit(QStringLiteral("一致性检查"),
        [this, result, error](TaskQueue::Progress &) {
            try {
                *result = library_.checkConsistency();
            } catch (const std::exception &e) {
                *error = QString::fromUtf8(e.what());
            }
        });
    QObject::connect(captureQueue_.get(), &TaskQueue::taskFinished, this,
        [this, id, result, error](qint64 finished, bool, const QString &) {
            if (finished != id) return;
            if (!error->isEmpty()) {
                statusBar()->showMessage(tr("一致性检查失败：%1").arg(*error));
                return;
            }
            statusBar()->clearMessage();
            const Library::ConsistencyReport &r = *result;
            auto truncate = [](const QStringList &list, int limit) {
                QStringList shown = list.mid(0, limit);
                if (list.size() > limit)
                    shown << tr("…（共 %1 项）").arg(list.size());
                return shown;
            };
            const int problems = r.problems();
            QString body = tr("检查完成：%1 条修订引用，%2 个对象文件，%3 个对象校验通过。\n\n"
                              "问题：%4 项（缺失 %5，损坏 %6，孤立 %7，I/O %8）。")
                               .arg(r.revisions).arg(r.objects).arg(r.ok)
                               .arg(problems)
                               .arg(r.missing.size()).arg(r.corrupt.size())
                               .arg(r.orphan.size()).arg(r.ioErrors.size());
            if (problems == 0) {
                body += tr("\n\n未发现问题。");
            } else {
                if (!r.missing.isEmpty())
                    body += tr("\n\n缺失对象（被引用但无文件）：\n%1")
                                .arg(truncate(r.missing, 20).join('\n'));
                if (!r.corrupt.isEmpty())
                    body += tr("\n\n损坏对象（文件与哈希不符）：\n%1")
                                .arg(truncate(r.corrupt, 20).join('\n'));
                if (!r.orphan.isEmpty())
                    body += tr("\n\n孤立对象（无引用但存在文件）：\n%1")
                                .arg(truncate(r.orphan, 20).join('\n'));
                if (!r.ioErrors.isEmpty())
                    body += tr("\n\nI/O 错误（无法读取）：\n%1")
                                .arg(truncate(r.ioErrors, 20).join('\n'));
            }
            QMessageBox::information(this, tr("一致性检查"), body);
        });
}

// P-602: report-only space/retention preview. Nothing is cleaned here — the
// store keeps growing until a cleanup policy is separately approved (P-602
// explicitly makes that point). The preview separates logical size (naive
// manifest sum) from physical occupancy (object files on disk), counts shared
// objects and pinned (protected) versions, and estimates the theoretical
// reclaimable bytes from per-object reference counts rather than by summing
// file sizes naively (a shared object would be counted once per reference).
void MainWindow::showSpacePreview() {
    statusBar()->showMessage(tr("正在计算空间预演（只读，后台进行）…"));
    auto result = std::make_shared<Library::SpacePreview>();
    auto error = std::make_shared<QString>();
    const qint64 id = captureQueue_->submit(QStringLiteral("空间预演"),
        [this, result, error](TaskQueue::Progress &) {
            try {
                *result = library_.spacePreview();
            } catch (const std::exception &e) {
                *error = QString::fromUtf8(e.what());
            }
        });
    QObject::connect(captureQueue_.get(), &TaskQueue::taskFinished, this,
        [this, id, result, error](qint64 finished, bool, const QString &) {
            if (finished != id) return;
            if (!error->isEmpty()) {
                statusBar()->showMessage(tr("空间预演失败：%1").arg(*error));
                return;
            }
            statusBar()->clearMessage();
            const Library::SpacePreview &p = *result;
            auto fmt = [](qint64 bytes) {
                if (bytes >= 1024 * 1024)
                    return QStringLiteral("%1 MiB").arg(double(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
                if (bytes >= 1024)
                    return QStringLiteral("%1 KiB").arg(double(bytes) / 1024.0, 0, 'f', 1);
                return QStringLiteral("%1 B").arg(bytes);
            };
            const QString body =
                tr("素材：%1 个；修订：%2 条。\n\n"
                   "逻辑大小（清单合计，重复计数）：%3\n"
                   "物理占用（对象文件实际）：%4\n"
                   "共享对象：%5 个（去重节省 %6）\n"
                   "固定版本（受保护）：%7 条，占 %8\n"
                   "理论可回收：%9（%10 个孤立对象）\n\n"
                   "当前不做任何清理：磁盘会随版本持续增长。")
                    .arg(p.assets).arg(p.revisions)
                    .arg(fmt(p.logicalSize)).arg(fmt(p.physicalSize))
                    .arg(p.sharedObjects).arg(fmt(p.dedupSavings))
                    .arg(p.pinnedRevisions).arg(fmt(p.pinnedBytes))
                    .arg(fmt(p.reclaimableBytes)).arg(p.orphanObjects);
            QMessageBox::information(this, tr("空间预演"), body);
        });
}

// P-603: backup rehearsal — prove a package can actually be recovered, in an
// independent empty directory (never over the original library). This is the
// evidence that a backup succeeded; checking "the backup file exists" or
// counting archive members proves nothing about index/object correspondence.
// After the restore, the drill re-reads the recovered catalog and checks each
// manifest entry: asset id, folder/name path, revision count, content hash.
void MainWindow::restoreBackupDrill() {
    const QString packageDir = QFileDialog::getExistingDirectory(this, tr("选择备份包目录（含 COMPLETE 标记）"));
    if (packageDir.isEmpty()) return;
    const QString targetDir = QFileDialog::getExistingDirectory(this, tr("选择恢复演练目标目录（须为空目录）"));
    if (targetDir.isEmpty()) return;
    if (QMessageBox::question(this, tr("恢复演练"),
                              tr("将在独立目录完整恢复备份包并逐项核对：\n"
                                 "备份包：%1\n目标目录：%2\n\n"
                                 "不覆盖原素材库；核对 空目录恢复、内容哈希、资源 ID、版本数、路径。")
                                  .arg(packageDir, targetDir))
        != QMessageBox::Yes)
        return;
    statusBar()->showMessage(tr("正在执行恢复演练（后台进行）…"));
    auto summary = std::make_shared<QString>();
    auto error = std::make_shared<QString>();
    const qint64 id = captureQueue_->submit(QStringLiteral("恢复演练"),
        [packageDir, targetDir, summary, error](TaskQueue::Progress &) {
            try {
                Library drill(targetDir);
                const auto restored = drill.restoreBackupPackage(packageDir);
                int checked = 0;
                QStringList problems;
                const auto assets = drill.assets();
                for (const auto &e : restored) {
                    // Asset id present, path (folder/name) preserved.
                    const auto it = std::find_if(assets.cbegin(), assets.cend(),
                        [&e](const Asset &a) { return a.id == e.assetId; });
                    if (it == assets.cend()) {
                        problems << e.name;
                        continue;
                    }
                    if (it->name != e.name || it->folder != e.folder)
                        problems << e.name;
                    // Version count and content hash of the recovered revision.
                    const auto revs = drill.revisions(e.assetId);
                    if (revs.isEmpty() || revs.first().hash != e.hash)
                        problems << e.name;
                    // The recovered working file must hash to the same content.
                    QFile working(it->path);
                    if (!working.open(QIODevice::ReadOnly)
                        || QString::fromLatin1(QCryptographicHash::hash(working.readAll(),
                                                   QCryptographicHash::Sha256).toHex()) != e.hash)
                        problems << e.name;
                    ++checked;
                }
                *summary = problems.isEmpty()
                    ? QStringLiteral("OK %1").arg(restored.size())
                    : QStringLiteral("PARTIAL %1/%2 (%3)").arg(checked).arg(restored.size())
                                                     .arg(problems.join(','));
            } catch (const std::exception &e) {
                *error = QString::fromUtf8(e.what());
            }
        });
    QObject::connect(captureQueue_.get(), &TaskQueue::taskFinished, this,
        [this, id, summary, error, targetDir](qint64 finished, bool, const QString &) {
            if (finished != id) return;
            if (!error->isEmpty()) {
                statusBar()->showMessage(tr("恢复演练失败：%1").arg(*error));
                QMessageBox::warning(this, tr("恢复演练"),
                                     tr("恢复演练失败：%1\n（演练目录未通过，备份不可视为可用）").arg(*error));
                return;
            }
            statusBar()->showMessage(tr("恢复演练完成：%1（独立目录 %2）").arg(*summary).arg(targetDir));
            QMessageBox::information(this, tr("恢复演练"),
                                     tr("恢复演练完成：%1\n"
                                        "空目录恢复、内容哈希、资源 ID、版本数、路径已逐项核对。\n"
                                        "演练目录：%2").arg(*summary, targetDir));
        });
}
