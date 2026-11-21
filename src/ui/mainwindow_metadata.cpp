// Catalog metadata flows: tags (single and batch), rating and note,
// provenance and evidence, rename/move, deletion and the read-only
// per-field history dialog (LAY-003 split).
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


void MainWindow::reloadTags() {
    tagsList_->blockSignals(true);
    tagsList_->clear();
    for (const QString &tag : library_.tags(selected().id))
        tagsList_->addItem(tag);
    tagsList_->blockSignals(false);
}

// P-214: rating/note block reflects the selected asset. The rating combo maps
// -1 (no rating) to index 0, then 0..5 stars; the note editor holds the stored
// note verbatim. Loading blocks signals so a reload never triggers a save.
void MainWindow::reloadRatingNote() {
    const auto asset = selected();
    ratingCombo_->blockSignals(true);
    if (asset.id.isEmpty()) {
        ratingCombo_->setCurrentIndex(0);
    } else {
        const int value = library_.rating(asset.id);
        const int index = ratingCombo_->findData(value);
        ratingCombo_->setCurrentIndex(index >= 0 ? index : 0);
    }
    ratingCombo_->blockSignals(false);
    noteEdit_->blockSignals(true);
    noteEdit_->setPlainText(asset.id.isEmpty() ? QString() : library_.note(asset.id));
    noteEdit_->blockSignals(false);
    noteAssetId_ = asset.id;
}

void MainWindow::applyRating(int index) {
    const auto asset = selected();
    if (asset.id.isEmpty()) return;
    const int value = ratingCombo_->itemData(index).toInt();
    try {
        library_.setRating(asset.id, value);
        statusBar()->showMessage(value < 0 ? tr("已清除评分") : tr("已保存评分：%1 星").arg(value));
        // P-309: the change appended a metadata-only revision; refresh the timeline.
        reloadHistory();
    } catch (const std::exception &error) { report(error); }
}

// P-215: list the provenance records of the selected asset (newest first) and
// clear any pending (unrecorded) attachment. A record line omits empty fields,
// so "unknown" never renders as a value that was never entered.
void MainWindow::reloadProvenance() {
    const auto asset = selected();
    pendingEvidenceHash_.clear();
    pendingEvidenceName_.clear();
    evidenceLabel_->setText(tr("未附加证据"));
    provenanceList_->clear();
    for (const auto &entry : library_.provenance(asset.id)) {
        const auto date = QDateTime::fromString(entry.recorded, Qt::ISODateWithMs).toLocalTime();
        QStringList parts;
        parts << date.toString("MM-dd HH:mm:ss");
        if (!entry.source.isEmpty()) parts << entry.source;
        if (!entry.author.isEmpty()) parts << tr("作者：%1").arg(entry.author);
        if (!entry.license.isEmpty()) parts << tr("许可：%1").arg(entry.license);
        if (!entry.evidenceName.isEmpty()) parts << tr("证据：%1").arg(entry.evidenceName);
        auto *item = new QListWidgetItem(parts.join(" · "), provenanceList_);
        item->setData(Qt::UserRole, entry.evidence);
    }
    exportEvidenceBtn_->setEnabled(false);
}

void MainWindow::attachEvidence() {
    const QString path = QFileDialog::getOpenFileName(this, tr("选择授权证据文件"));
    if (path.isEmpty()) return;
    try {
        pendingEvidenceHash_ = library_.storeEvidence(path);
        pendingEvidenceName_ = QFileInfo(path).fileName();
        evidenceLabel_->setText(tr("待记录证据：%1").arg(pendingEvidenceName_));
        evidenceLabel_->setStyleSheet(QString());
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::recordProvenance() {
    const auto asset = selected();
    if (asset.id.isEmpty()) return;
    try {
        Provenance entry;
        entry.source = sourceInput_->text().trimmed();
        entry.author = authorInput_->text().trimmed();
        entry.license = licenseInput_->text().trimmed();
        entry.evidence = pendingEvidenceHash_;
        entry.evidenceName = pendingEvidenceName_;
        library_.addProvenance(asset.id, entry);
        sourceInput_->clear();
        authorInput_->clear();
        licenseInput_->clear();
        statusBar()->showMessage(tr("已记录来源与授权"));
        reloadProvenance();
        reloadHistory(); // P-309: the change appended metadata-only revisions
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::renameSelected() {
    const auto asset = selected();
    if (asset.id.isEmpty()) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("重命名"), tr("新名称"), QLineEdit::Normal, asset.name, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    try {
        library_.renameAsset(asset.id, name);
        statusBar()->showMessage(tr("已重命名：%1").arg(name));
        reloadAssets(asset.id);
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::moveSelected() {
    const auto asset = selected();
    if (asset.id.isEmpty()) return;
    bool ok = false;
    const QString folder = QInputDialog::getText(this, tr("移动到文件夹"),
                                                 tr("文件夹（库内相对路径，空为根目录）"),
                                                 QLineEdit::Normal, asset.folder, &ok);
    if (!ok) return;
    try {
        library_.moveAsset(asset.id, folder);
        const QString cleaned = folder.trimmed();
        statusBar()->showMessage(cleaned.isEmpty() ? tr("已移到根目录") : tr("已移动到：%1").arg(cleaned));
        reloadAssets(asset.id);
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::deleteSelected() {
    const auto asset = selected();
    if (asset.id.isEmpty()) return;
    // P-310: preview → confirm, exactly as the spec's three-state table demands
    // for an in-app delete. The preview makes explicit that this is not the
    // "missing working copy" or "storage unreachable" state.
    const auto revisions = library_.revisions(asset.id);
    const QStringList lines{
        tr("名称：%1").arg(asset.name),
        tr("路径：%1").arg(asset.logicalPath()),
        tr("版本：%1 个").arg(revisions.size()),
        tr("工作副本将被移除；历史与对象保留，可从时间线恢复。"),
    };
    QMessageBox box(QMessageBox::Warning, tr("删除素材"), lines.join(QLatin1Char('\n')), QMessageBox::NoButton, this);
    auto *confirmBtn = box.addButton(tr("确认删除"), QMessageBox::YesRole);
    box.addButton(tr("取消"), QMessageBox::NoRole);
    box.exec();
    if (box.clickedButton() != confirmBtn) return;
    try {
        library_.deleteAsset(asset.id);
        statusBar()->showMessage(tr("已删除：%1（历史与对象保留，可从时间线恢复）").arg(asset.name));
        reloadAssets();
        historyList_->clear();
        details_->clear();
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::exportEvidenceFile() {
    const int row = provenanceList_->currentRow();
    if (row < 0) return;
    const QString hash = provenanceList_->item(row)->data(Qt::UserRole).toString();
    if (hash.isEmpty()) return;
    const QString dest = QFileDialog::getSaveFileName(this, tr("导出授权证据"));
    if (dest.isEmpty()) return;
    try {
        library_.exportEvidence(hash, dest);
        statusBar()->showMessage(tr("授权证据已导出：%1").arg(QFileInfo(dest).fileName()));
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::saveNote() {
    // Save to the asset the editor represents, not to selected(): switching
    // assets while the editor is focused must not carry the old note over.
    if (noteAssetId_.isEmpty()) return;
    try {
        library_.setNote(noteAssetId_, noteEdit_->toPlainText());
        statusBar()->showMessage(tr("备注已保存"));
        // P-309: the change appended a metadata-only revision; refresh the timeline.
        reloadHistory();
    } catch (const std::exception &error) { report(error); }
}

QStringList MainWindow::visibleAssetIds() const {
    QStringList ids;
    for (int i = 0; i < assetsList_->count(); ++i)
        if (!assetsList_->item(i)->isHidden())
            ids << assetsList_->item(i)->data(Qt::UserRole).toString();
    return ids;
}

void MainWindow::addTagToSelected() {
    const auto asset = selected();
    if (asset.id.isEmpty()) return;
    const QString tag = tagInput_->text().trimmed();
    if (tag.isEmpty()) { statusBar()->showMessage(tr("标签不能为空")); return; }
    try {
        const int changed = library_.addTag(asset.id, tag);
        statusBar()->showMessage(changed ? tr("已添加标签：%1").arg(tag) : tr("标签已存在：%1").arg(tag));
        tagInput_->clear();
        reloadTags();
        if (changed) reloadHistory(); // P-309: the change appended a metadata-only revision
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::removeSelectedTag() {
    const auto asset = selected();
    if (asset.id.isEmpty()) return;
    const int row = tagsList_->currentRow();
    if (row < 0) { statusBar()->showMessage(tr("请先选择一个标签")); return; }
    const QString tag = tagsList_->item(row)->text();
    try {
        library_.removeTag(asset.id, tag);
        statusBar()->showMessage(tr("已移除标签：%1").arg(tag));
        reloadTags();
        reloadHistory(); // P-309: the change appended a metadata-only revision
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::batchAddTag() {
    bool ok = false;
    const QString tag = QInputDialog::getText(this, tr("为筛选结果添加标签"), tr("标签"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || tag.isEmpty()) return;
    const QStringList ids = visibleAssetIds();
    const int affected = library_.countAssetsWithoutTag(ids, tag);
    if (affected == 0) { statusBar()->showMessage(tr("筛选结果中的素材都已带此标签")); return; }
    if (QMessageBox::question(this, tr("批量添加标签"),
                              tr("将给 %1 个素材添加标签「%2」（已有该标签的不变）。\n不会覆盖其他标签。").arg(affected).arg(tag)) != QMessageBox::Yes) return;
    try {
        const int changed = library_.addTagToAssets(ids, tag);
        statusBar()->showMessage(tr("已为 %1 个素材添加标签：%2").arg(changed).arg(tag));
        reloadTags();
    } catch (const std::exception &error) { report(error); }
}

void MainWindow::batchRemoveTag() {
    bool ok = false;
    const QString tag = QInputDialog::getText(this, tr("从筛选结果移除标签"), tr("标签"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || tag.isEmpty()) return;
    const QStringList ids = visibleAssetIds();
    const int affected = library_.countAssetsWithTag(ids, tag);
    if (affected == 0) { statusBar()->showMessage(tr("筛选结果中没有素材带此标签")); return; }
    if (QMessageBox::question(this, tr("批量移除标签"),
                              tr("将从 %1 个素材移除标签「%2」。\n只移除这一个标签，不影响其他标签。").arg(affected).arg(tag)) != QMessageBox::Yes) return;
    try {
        const int changed = library_.removeTagFromAssets(ids, tag);
        statusBar()->showMessage(tr("已从 %1 个素材移除标签：%2").arg(changed).arg(tag));
        reloadTags();
    } catch (const std::exception &error) { report(error); }
}

// P-309: read-only per-field metadata history for the selected asset. Each
// change shows field, old → new, and the metadata-only revision it created.
// V1 is read-only: no per-field revert, no invented old values.
void MainWindow::showFieldHistory() {
    const auto asset = selected();
    QDialog dialog(this);
    dialog.setWindowTitle(tr("字段历史"));
    dialog.resize(480, 380);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(tr("逐字段只读回看（名称/目录/标签/评分/备注/来源/作者/许可）；字段变更与版本序列一致，内容不变。"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *list = new QListWidget;
    list->setWordWrap(true);
    layout->addWidget(list, 1);
    auto *closeBtn = new QPushButton(tr("关闭"));
    layout->addWidget(closeBtn);
    connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
    const auto history = library_.fieldHistory(asset.id);
    if (history.isEmpty()) {
        auto *item = new QListWidgetItem(tr("（无字段变更记录）"));
        item->setForeground(Qt::gray);
        list->addItem(item);
    }
    for (const auto &change : history) {
        const auto time = QDateTime::fromString(change.changed, Qt::ISODateWithMs).toLocalTime();
        const auto value = [](const QString &text) { return text.isEmpty() ? tr("（空）") : text; };
        list->addItem(QStringLiteral("%1\n%2：%3 → %4\n版本 #%5")
                          .arg(time.toString("MM-dd HH:mm:ss"), change.field,
                               value(change.oldValue), value(change.newValue))
                          .arg(change.revisionId));
    }
    dialog.exec();
}
