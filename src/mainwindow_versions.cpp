// Version flows: history reload and integrity probing, revision preview,
// import, manual/auto capture, restore and historical export, and the
// side-by-side version compare dialog (LAY-003 split).
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


void MainWindow::reloadHistory() {
    const auto asset = selected();
    // P-308: the timeline is ordered by the database's monotonic revision id
    // (revisions() returns newest id first) — local time is display-only, so a
    // clock change or timezone shift never reorders or mixes history.
    revisions_ = asset.id.isEmpty() ? QVector<Revision>{} : library_.revisions(asset.id);
    requestIntegrity(revisions_);
    rebuildHistory();
}

// Audit (2026-11-04): a history refresh never hashes objects on the UI thread.
// Any revision whose content hash is not yet in the integrity cache is queued
// on the background worker; the UI renders from the cache immediately (a
// not-yet-verified object shows the corrupt placeholder) and re-renders when
// the worker reports a result. Only one verify batch is in flight at a time.
void MainWindow::requestIntegrity(const QVector<Revision> &revisions) {
    QSet<QString> missing;
    {
        QMutexLocker lock(&integrityMutex_);
        for (const auto &revision : revisions)
            if (!integrity_.contains(revision.hash)) missing.insert(revision.hash);
    }
    if (missing.isEmpty() || integrityInFlight_) return;
    integrityInFlight_ = true;
    auto hashes = std::make_shared<QSet<QString>>(missing);
    integrityQueue_->submit(QStringLiteral("历史完整性校验"),
        [this, hashes](TaskQueue::Progress &) {
            // Audit (2026-11-04): verify each object without holding the cache
            // mutex — verifyObject is a streaming SHA-256 over a whole object
            // (up to 256 MiB) and must never block rebuildHistory's UI thread
            // for the entire batch. Only the final result merge takes the lock.
            QHash<QString, bool> results;
            for (const auto &hash : *hashes) {
                // Audit (2026-11-04): a malformed/non-hex hash makes
                // Library::objectPath throw before the file is opened. Catch
                // that here and record it as corrupt (false), otherwise the
                // completion callback would keep seeing a missing cache entry
                // and infinitely requeue the same broken hash.
                try {
                    results.insert(hash, library_.verifyObject(hash));
                } catch (...) {
                    results.insert(hash, false);
                }
            }
            QMutexLocker lock(&integrityMutex_);
            for (auto it = results.cbegin(); it != results.cend(); ++it)
                integrity_.insert(it.key(), it.value());
        });
}

void MainWindow::rebuildHistory() {
    const auto asset = selected();
    // Working-copy content markers: which revision matches the current content
    // ("当前内容"), and whether newer unrecorded changes sit in the working copy.
    QString currentHash;
    {
        QFile file(asset.path);
        if (file.exists() && file.open(QIODevice::ReadOnly)) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            constexpr qint64 kChunk = 1 << 20;
            while (!file.atEnd()) hash.addData(file.read(kChunk));
            currentHash = QString::fromLatin1(hash.result().toHex());
        }
    }
    qint64 currentId = 0; // newest revision whose content equals the working copy
    for (const auto &revision : revisions_) {
        if (!currentHash.isEmpty() && revision.hash == currentHash) { currentId = revision.id; break; }
    }
    const bool unrecorded = !currentHash.isEmpty()
        && (revisions_.isEmpty() || revisions_.constFirst().hash != currentHash);
    historyList_->blockSignals(true);
    historyList_->clear();
    for (int i = 0; i < revisions_.size(); ++i) {
        const auto &revision = revisions_.at(i);
        const auto date = QDateTime::fromString(revision.created, Qt::ISODateWithMs).toLocalTime();
        // P-207 integrity: read from the background-verified cache. A missing
        // cache entry means the object has not been verified yet — treat it as
        // corrupt for this frame (the placeholder re-renders when the result
        // lands), so a not-yet-verified object is never silently trusted.
        bool corrupt = true;
        {
            QMutexLocker lock(&integrityMutex_);
            if (integrity_.contains(revision.hash)) corrupt = !integrity_.value(revision.hash);
        }
        // P-307/P-308: per-row markers. A restore-formed record and a restore's
        // automatic safety save are marked by their system-written notes; every
        // older revision is plainly "历史".
        QStringList markers;
        if (i == 0 && unrecorded) markers << tr("未记录修改");
        if (revision.pinned) markers << tr("已固定");
        // P-310: a tombstone revision marks the in-app deletion point. It keeps
        // the last content hash, so a healthy object still renders; a missing
        // object just shows the standard corrupt warning. The asset stays
        // reachable here even though it is no longer listed as live.
        if (revision.tombstone) markers << tr("墓碑");
        if (!corrupt && revision.id == currentId) markers << tr("当前内容");
        else if (revision.note.startsWith(QStringLiteral("恢复历史版本 #"))) markers << tr("恢复");
        else if (revision.note == QStringLiteral("恢复前自动保存")) markers << tr("自动保存");
        else if (i != 0) markers << tr("历史");
        if (corrupt) markers << tr("损坏对象");
        const QString prefix = markers.isEmpty() ? QString()
            : markers.join(QStringLiteral(" · ")) + QStringLiteral(" · ");
        auto *item = new QListWidgetItem(QStringLiteral("%1%2\n%3\n%4")
            .arg(prefix, date.toString("MM-dd HH:mm:ss"), revision.note,
                 QStringLiteral("%1 KiB").arg(revision.size / 1024.0, 0, 'f', 1)), historyList_);
        if (revision.pinned) item->setToolTip(tr("固定原因：%1").arg(revision.pinReason));
        if (corrupt) {
            item->setIcon(style()->standardIcon(QStyle::SP_MessageBoxWarning));
        } else {
            try {
                const QImage thumb = cachedThumbnail(revision.hash, selected().name, QSize(80, 80));
                if (!thumb.isNull()) item->setIcon(QPixmap::fromImage(thumb));
            } catch (const std::exception &) {
                // Object vanished between the verify and the decode: show the
                // corrupt marker instead of failing the whole refresh.
                item->setIcon(style()->standardIcon(QStyle::SP_MessageBoxWarning));
            }
        }
    }
    historyList_->setCurrentRow(revisions_.isEmpty() ? -1 : 0);
    historyList_->blockSignals(false);
    showRevision();
    reloadTags();
    reloadRatingNote();
    reloadProvenance();
}


void MainWindow::showRevision() {
    const int row = historyList_->currentRow();
    const bool valid = row >= 0 && row < revisions_.size();
    // P-409: capability-driven history actions. A read-only backend (Git/SVN,
    // V1.x) must not offer a write-back restore button — the capability→action
    // mapping is enforced here and the unsupported reason is surfaced.
    const auto actions = historyActionsFor(library_.backendInfo(), valid);
    restore_->setEnabled(actions.restore);
    export_->setEnabled(actions.extract);
    compareBtn_->setEnabled(actions.extract); // comparison reads historical content
    restore_->setToolTip(actions.restoreReason);
    export_->setToolTip(actions.extractReason);
    compareBtn_->setToolTip(actions.extractReason);
    if (!valid) return;
    const auto &revision = revisions_.at(row);
    // P-212: technical metadata (format/size/fileSize/channels) from the header;
    // unknown fields are omitted, never invented.
    const auto meta = preview::metadata(library_.objectPath(revision.hash),
                                        QFileInfo(selected().name).suffix().toLower());
    QStringList facts;
    if (!meta.format.isEmpty()) facts << meta.format.toUpper();
    if (meta.width > 0 && meta.height > 0)
        facts << QStringLiteral("%1×%2").arg(meta.width).arg(meta.height);
    if (meta.fileSize > 0)
        facts << tr("%1 MiB").arg(meta.fileSize / 1048576.0, 0, 'f', 1);
    if (!meta.channels.isEmpty()) {
        QString ch = meta.channels;
        if (meta.bitsPerChannel > 0) ch += QStringLiteral(" %1bit").arg(meta.bitsPerChannel);
        facts << ch;
    }
    // P-310: working-copy health is shown alongside the version facts. Missing
    // (externally removed) and unreachable (unplugged mount / dropped share /
    // permissions) are recoverable states, never deletions — no tombstone is
    // written for either, and the message says so explicitly.
    QString healthLine;
    const auto health = library_.workingCopyHealth(selected());
    if (health == Library::Health::Missing)
        healthLine = tr("\n工作副本缺失（可从历史恢复，不视为删除）");
    else if (health == Library::Health::Unreachable)
        healthLine = tr("\n存储不可达（拔盘或断网不算删除；恢复挂载后自动恢复）");
    details_->setText(QStringLiteral("%1\n%2 · %3 · #%4%5%6")
        .arg(selected().name)
        .arg(row == 0 ? tr("当前版本") : tr("历史版本"))
        .arg(QDateTime::fromString(revision.created, Qt::ISODateWithMs).toLocalTime().toString("yyyy-MM-dd HH:mm:ss"))
        .arg(revision.id)
        .arg(facts.isEmpty() ? QString() : QStringLiteral(" · ") + facts.join(QStringLiteral(" · ")))
        .arg(healthLine));
    QByteArray content;
    try {
        content = library_.readObject(revision.hash);
    } catch (const std::exception &error) {
        preview_->setText(tr("内容损坏或校验失败\n\n%1\n\n原始对象已保留，可导出排查。")
            .arg(QString::fromUtf8(error.what())));
        previewStack_->setCurrentIndex(1);
        return;
    }
    // P-209/P-210: decode is bounded (huge images downscale on read, never
    // stretching), the canvas zoom stays uniform, and the asset's own extension
    // is authoritative for signature-less formats (TGA) that sniffing mislabels.
    const auto decoded = preview::decode(library_.objectPath(revision.hash),
                                         QFileInfo(selected().name).suffix().toLower());
    if (!decoded.valid) {
        // P-211: PSD composite preview was evaluated (no PSD codec in Qt), so
        // Photoshop files degrade explicitly: no composite view is claimed, and
        // the original bytes stay fully preserved and exportable.
        preview_->setText(preview::isPsdSignature(content)
            ? tr("原文件已保留，暂无合成预览（PSD 解析未支持）\n\nPhotoshop 合成图解析不在当前版本范围内（Qt 6.10.2 无 PSD 解码插件）。\n原始文件和历史版本已完整保留，可以导出查看。")
            : tr("此格式暂不支持画面预览\n\n原始文件和历史版本已完整保留，可以导出查看。"));
        previewStack_->setCurrentIndex(1);
        return;
    }
    canvas_->setImage(decoded.image);
    previewStack_->setCurrentIndex(0);
    syncZoomButtons(canvas_->isFit());
    updateZoomLabel();
    if (decoded.reduced)
        statusBar()->showMessage(tr("预览已降采样显示（解码尺寸超出内存预算），原始文件未修改"));
}


void MainWindow::importFiles() {
    const auto files = QFileDialog::getOpenFileNames(this, tr("导入素材（保留原文件，创建工作副本）"));
    QString last;
    for (const auto &file : files) {
        try { last = library_.importFile(file).id; }
        catch (const std::exception &error) { report(error); break; }
    }
    reloadAssets(last);
    // Re-arm the sentinel: the app's own import is legitimate, not ransomware.
    if (sentinel_) sentinel_->resetBaseline();
}

void MainWindow::capture() {
    const auto asset = selected();
    if (asset.id.isEmpty()) return;
    bool ok = false;
    const auto note = QInputDialog::getText(this, tr("记录版本"), tr("版本说明"), QLineEdit::Normal, QStringLiteral("手动快照"), &ok);
    if (!ok) return;
    try {
        bool changed = false;
        const qint64 revisionId = library_.captureRevision(asset, note, &changed);
        // P-303: surface the version id either way, so an unchanged capture is
        // an explicit answer ("current version #N") rather than a silent no-op.
        statusBar()->showMessage(changed
            ? tr("已记录新版本 #%1").arg(revisionId)
            : tr("内容未变化（当前版本 #%1），无需重复保存").arg(revisionId));
        reloadHistory();
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::restore() {
    const int row = historyList_->currentRow();
    if (row < 0) return;
    const auto asset = selected();
    const qint64 revisionId = revisions_.at(row).id;
    // P-315: the preview comes first — the exact operation list is computed
    // before any target write, and the preview itself never writes a file.
    Library::RestorePlan plan;
    try { plan = library_.planRestore(asset, revisionId); }
    catch (const std::exception &error) { report(error); return; }
    QString detail;
    bool confirmable = true;
    if (plan.action == QStringLiteral("overwrite")) {
        // P-316: the on-disk unrecorded content is protected as a version before
        // the overwrite. Only disk-saved content is protected — an editor's
        // unsaved buffer is never claimed to be saved.
        detail = tr("将覆盖当前工作副本：未记录的当前内容会先保存为保护版本。\n"
                    "空间需求：约 %1 KiB（保护 %2 KiB + 写入 %3 KiB）。\n"
                    "保护的是磁盘上已保存的内容；外部编辑器未写盘的内容不受保护，请先关闭编辑器。")
            .arg(plan.spaceNeeded / 1024.0, 0, 'f', 1)
            .arg(plan.protectBytes / 1024.0, 0, 'f', 1)
            .arg(plan.targetBytes / 1024.0, 0, 'f', 1);
    } else if (plan.action == QStringLiteral("create")) {
        detail = tr("当前工作副本缺失——将新增该文件（无内容可保护，请确认）。\n"
                    "空间需求：约 %1 KiB。")
            .arg(plan.spaceNeeded / 1024.0, 0, 'f', 1);
    } else if (plan.action == QStringLiteral("skip")) {
        detail = tr("当前内容已与目标一致——无需恢复。");
        confirmable = false;
    } else {
        detail = tr("无法恢复：%1").arg(plan.conflictReason);
        confirmable = false;
    }
    QMessageBox box(this);
    box.setWindowTitle(tr("恢复预演"));
    box.setIcon(confirmable ? QMessageBox::Question : QMessageBox::Warning);
    box.setText(detail);
    if (!confirmable) {
        box.addButton(QMessageBox::Ok);
        box.exec();
        return;
    }
    QAbstractButton *confirm = box.addButton(tr("确认恢复"), QMessageBox::YesRole);
    box.addButton(tr("取消"), QMessageBox::NoRole);
    box.exec();
    if (box.clickedButton() != confirm) return;
    // P-315: execution re-checks the plan binding — a working copy that changed
    // since the preview makes the plan stale, and the user is asked to refresh.
    try { library_.restore(asset, plan); reloadAssets(asset.id); }
    catch (const std::exception &error) { report(error); return; }
    // Re-arm the sentinel: restore is an app-initiated write, not ransomware.
    if (sentinel_) sentinel_->resetBaseline();
}

void MainWindow::exportFile() {
    const int row = historyList_->currentRow();
    if (row < 0) return;
    const auto path = QFileDialog::getSaveFileName(this, tr("导出历史版本"), selected().name);
    if (path.isEmpty()) return;
    // P-319: an existing destination is only overwritten after explicit confirmation.
    bool overwrite = false;
    if (QFileInfo::exists(path)) {
        if (QMessageBox::question(this, tr("导出历史版本"),
                                  tr("目标文件已存在：%1\n覆盖？").arg(path)) != QMessageBox::Yes)
            return;
        overwrite = true;
    }
    try { library_.exportRevision(selected().id, revisions_.at(row).id, path, overwrite); }
    catch (const std::exception &error) { report(error); }
}

void MainWindow::compareVersions() {
    const int row = historyList_->currentRow();
    if (row < 0 || row >= revisions_.size() || revisions_.isEmpty()) return;
    const auto &current = revisions_.constFirst();   // newest = current content
    const auto &selected = revisions_.at(row);
    const QString suffix = QFileInfo(this->selected().name).suffix().toLower();

    // P-313: one shared image-coordinate transform (same zoom scale, same
    // origin) for both panes. Default is top-left aligned, never stretched;
    // "fit window" applies a single unified scale. Pan and zoom move both
    // panes together, and differing sizes read as explicit empty regions.
    auto decodeRevision = [this, suffix](qint64 revisionId) {
        for (const auto &rev : revisions_)
            if (rev.id == revisionId)
                return preview::decode(library_.objectPath(rev.hash), suffix).image;
        return QImage();
    };
    const QImage leftImage = decodeRevision(current.id);
    const QImage rightImage = decodeRevision(selected.id);

    QDialog dialog(this);
    dialog.setWindowTitle(tr("双版对比"));
    dialog.resize(860, 560);
    auto *layout = new QVBoxLayout(&dialog);
    auto *canvas = new CompareCanvas(&dialog);
    canvas->setImages(leftImage, rightImage);
    auto *bar = new QWidget(&dialog);
    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(0, 0, 0, 0);
    auto *leftLabel = new QLabel(tr("左：当前版本 #%1").arg(current.id), bar);
    auto *rightLabel = new QLabel(tr("右：所选版本 #%1").arg(selected.id), bar);
    auto *zoomLabel = new QLabel(bar);
    auto *fitCheck = new QCheckBox(tr("适配窗口（统一比例）"), bar);
    fitCheck->setChecked(true);
    connect(fitCheck, &QCheckBox::toggled, canvas, &CompareCanvas::setFit);
    // P-314: sliding-divider wipe over the same shared transform. The divider
    // is dragged with the mouse, stepped with Left/Right, and its step size is
    // adjusted with [ / ].
    auto *slidingCheck = new QCheckBox(tr("滑动对比"), bar);
    connect(slidingCheck, &QCheckBox::toggled, canvas, &CompareCanvas::setSliding);
    connect(canvas, &CompareCanvas::zoomChanged, bar, [zoomLabel](qreal factor) {
        zoomLabel->setText(QStringLiteral("%1%").arg(qRound(factor * 100)));
    });
    zoomLabel->setText(QStringLiteral("%1%").arg(qRound(canvas->zoom() * 100)));
    barLayout->addWidget(leftLabel);
    barLayout->addStretch(1);
    barLayout->addWidget(zoomLabel);
    barLayout->addWidget(fitCheck);
    barLayout->addWidget(slidingCheck);
    barLayout->addStretch(1);
    barLayout->addWidget(rightLabel);
    // P-313: differing sizes are stated explicitly — never auto-stretched.
    if (leftImage.size() != rightImage.size()) {
        auto *sizeNote = new QLabel(tr("两版尺寸不同：%1×%2 与 %3×%4（左上对齐，不拉伸；空白为无内容区域）")
            .arg(leftImage.width()).arg(leftImage.height())
            .arg(rightImage.width()).arg(rightImage.height()), &dialog);
        sizeNote->setStyleSheet("color:#b26a00;");
        layout->addWidget(sizeNote);
    }
    layout->addWidget(canvas, 1);
    layout->addWidget(bar);
    // P-314: divider interaction is discoverable and explicit.
    auto *dividerHint = new QLabel(tr("滑动对比：拖动分界线；左右键步进；[ / ] 调整步进；缩放后分界线保持同一图像坐标。"), &dialog);
    dividerHint->setStyleSheet("color:#888;");
    layout->addWidget(dividerHint);
    auto *closeBtn = new QPushButton(tr("关闭"), &dialog);
    connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(closeBtn);
    dialog.exec();
}
