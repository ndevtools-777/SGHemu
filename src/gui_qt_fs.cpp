#include "gui_qt.h"

extern "C" {
#include "port.h"
}

#include <QBuffer>
#include <QCheckBox>
#include <QDialog>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QImageReader>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <cstring>
#include <functional>

class FilesystemDialog final : public QDialog {
public:
    using AddFiles = std::function<void(const QStringList &)>;
    FilesystemDialog(QWidget *parent, AddFiles addFiles) : QDialog(parent), addFiles_(addFiles)
    {
        setAcceptDrops(true);
    }

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasUrls()) event->acceptProposedAction();
    }

    void dropEvent(QDropEvent *event) override
    {
        QStringList paths;
        for (const QUrl &url : event->mimeData()->urls())
            if (url.isLocalFile()) paths << url.toLocalFile();
        if (paths.isEmpty()) return;
        addFiles_(paths);
        event->acceptProposedAction();
    }

private:
    AddFiles addFiles_;
};

static const char *contentKind(unsigned type)
{
    switch (type) {
    case 6: return "Sound";
    case 7: return "Picture";
    case 10: return "Java";
    default: return "System";
    }
}

static bool pictureExtension(const QString &ext)
{
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "bmp";
}

static int decodePicture(ContentImport *im)
{
    /* Qt reads the first frame. The C importer still builds the phone's
       RGB565 container and submits it to the guest's own storage code. */
    QBuffer buffer;
    buffer.setData(QByteArray::fromRawData(reinterpret_cast<const char *>(im->file), im->size));
    QImageReader reader(&buffer);
    const QSize size = reader.size();
    if (size.isEmpty() || qint64(size.width()) * size.height() > 64 * 1024 * 1024) {
        snprintf(im->error, sizeof(im->error), "Cannot decode picture or its dimensions are too large");
        im->failed = 1;
        return 0;
    }
    if (size.width() > 128 || size.height() > 128) {
        const QSize scaled = size.scaled(128, 128, Qt::KeepAspectRatio);
        reader.setScaledSize(QSize(qMax(1, scaled.width()), qMax(1, scaled.height())));
    }
    QImage image = reader.read().convertToFormat(QImage::Format_RGB888);
    if (image.isNull()) {
        snprintf(im->error, sizeof(im->error), "Cannot decode picture: %.130s", reader.errorString().toUtf8().constData());
        im->failed = 1;
        return 0;
    }
    return content_picture_rgb(im, image.width(), image.height(), image.constBits(), image.bytesPerLine());
}

static void setupTable(QTableWidget *table, const QStringList &headings)
{
    table->setColumnCount(headings.size());
    table->setHorizontalHeaderLabels(headings);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
}

static void setRow(QTableWidget *table, int row, const QStringList &values)
{
    for (int column = 0; column < values.size(); ++column)
        table->setItem(row, column, new QTableWidgetItem(values[column]));
}

void PhoneWindow::showFilesystem()
{
    if (!filesystem_) {
        filesystem_ = new FilesystemDialog(this, [this](const QStringList &paths) { addImportFiles(paths); });
        filesystem_->setWindowTitle("SGHemu - phone filesystem manager");
        filesystem_->resize(1000, 535);
        auto *layout = new QVBoxLayout(filesystem_);
        auto *panes = new QSplitter(Qt::Horizontal, filesystem_);
        layout->addWidget(panes, 1);

        auto *left = new QWidget(panes);
        auto *leftLayout = new QVBoxLayout(left);
        leftLayout->addWidget(new QLabel("Import list (bulk loading)", left));
        importList_ = new QTableWidget(left);
        setupTable(importList_, {"Import file", "Type", "Bytes", "Status"});
        leftLayout->addWidget(importList_);
        auto *queueButtons = new QHBoxLayout;
        leftLayout->addLayout(queueButtons);
        auto *addFiles = new QPushButton("Add files...", left);
        removeFiles_ = new QPushButton("Remove", left);
        startQueue_ = new QPushButton("Import queue >>", left);
        stopQueue_ = new QPushButton("Stop", left);
        for (auto *button : {addFiles, removeFiles_, startQueue_, stopQueue_}) queueButtons->addWidget(button);

        auto *right = new QWidget(panes);
        auto *rightLayout = new QVBoxLayout(right);
        auto *showSystem = new QCheckBox("Show system records", right);
        showSystem->setChecked(showSystem_);
        rightLayout->addWidget(showSystem);
        phoneFiles_ = new QTableWidget(right);
        setupTable(phoneFiles_, {"Phone file", "Type", "Bytes", "ID"});
        rightLayout->addWidget(phoneFiles_);
        auto *phoneButtons = new QHBoxLayout;
        rightLayout->addLayout(phoneButtons);
        auto *refresh = new QPushButton("Refresh", right);
        javaGames_ = new QPushButton("Java games", right);
        exportList_ = new QPushButton("Export list...", right);
        backupNvm_ = new QPushButton("Back up NVM...", right);
        for (auto *button : {refresh, javaGames_, exportList_, backupNvm_}) phoneButtons->addWidget(button);
        fsStatus_ = new QLabel(filesystem_);
        fsStatus_->setWordWrap(true);
        fsStatus_->setTextFormat(Qt::PlainText);
        layout->addWidget(fsStatus_);

        connect(addFiles, &QPushButton::clicked, this, [this] {
            addImportFiles(QFileDialog::getOpenFileNames(filesystem_, "Add content to the import list", {},
                "Phone content (*.mmf *.mid *.imy *.png *.jpg *.jpeg *.gif *.bmp *.jar);;All files (*)"));
        });
        connect(removeFiles_, &QPushButton::clicked, this, [this] {
            const auto selected = importList_->selectionModel()->selectedRows();
            for (int row = imports_.size() - 1; row >= 0; --row)
                if (selected.contains(importList_->model()->index(row, 0))) imports_.remove(row);
            refreshImportQueue();
        });
        connect(startQueue_, &QPushButton::clicked, this, [this] { importRunning_ = true; startNextImport(); });
        connect(stopQueue_, &QPushButton::clicked, this, [this] {
            importRunning_ = false;
            importMessage_ = "Queue stopped. The active phone write can finish; pending files stay queued.";
            refreshFilesystem();
        });
        connect(showSystem, &QCheckBox::toggled, this, [this](bool enabled) { showSystem_ = enabled; refreshFilesystem(); });
        connect(refresh, &QPushButton::clicked, this, &PhoneWindow::refreshFilesystem);
        connect(javaGames_, &QPushButton::clicked, this, &PhoneWindow::openJavaGames);
        connect(exportList_, &QPushButton::clicked, this, [this] { exportFilesystem(false); });
        connect(backupNvm_, &QPushButton::clicked, this, [this] { exportFilesystem(true); });
    }
    refreshFilesystem();
    filesystem_->show();
    filesystem_->raise();
    filesystem_->activateWindow();
}

void PhoneWindow::refreshFilesystem()
{
    if (!filesystem_) return;
    UserDirectory directory{};
    phoneFiles_->setRowCount(0);
    const bool valid = userfs_directory(&machine_, &directory);
    if (valid) {
        for (unsigned i = 0; i < directory.count; ++i) {
            const UserFile &file = directory.files[i];
            const bool java = !strncmp(file.name, "FFS_JAVA_", 9);
            if (!showSystem_ && file.type != 6 && file.type != 7 && !java) continue;
            QByteArray name(file.name);
            for (char &ch : name)
                if (static_cast<unsigned char>(ch) < 32 || ch == 127) ch = '?';
            const int row = phoneFiles_->rowCount();
            phoneFiles_->insertRow(row);
            setRow(phoneFiles_, row, {QString::fromLatin1(name), contentKind(java ? 10 : file.type),
                                     QString::number(file.size), QString::number(file.id, 16).toUpper().rightJustified(4, '0')});
        }
    }
    QString status = valid ? QString("%1 visible files; %2 total records. Imports use the phone's own storage.")
                                 .arg(phoneFiles_->rowCount()).arg(directory.count) : QString::fromUtf8(directory.error);
    if (!importMessage_.isEmpty()) status += '\n' + importMessage_;
    fsStatus_->setText(status);
    exportList_->setEnabled(valid);
    refreshImportQueue();
}

void PhoneWindow::refreshImportQueue()
{
    if (!filesystem_) return;
    const QStringList statuses = {"Queued", "Starting", "Waiting for phone storage", "Imported", "Failed"};
    bool queued = false;
    importList_->setRowCount(imports_.size());
    for (int row = 0; row < imports_.size(); ++row) {
        const ImportItem &item = imports_[row];
        const QString status = item.error.isEmpty() ? statuses[static_cast<int>(item.status)] : item.error;
        queued |= item.status == ImportStatus::Queued;
        setRow(importList_, row, {QFileInfo(item.path).fileName(), contentKind(item.type), QString::number(item.size), status});
        importList_->item(row, 3)->setToolTip(status);
    }
    const bool busy = importCurrent_ >= 0 || content_.active || importRunning_;
    const bool ready = storage_.active && !storage_.provision && !storage_.defaults_boot && !machine_.cpu.halted;
    startQueue_->setEnabled(queued && ready && machine_.import_trigger && !busy);
    stopQueue_->setEnabled(importRunning_);
    javaGames_->setEnabled(ready && machine_.java_ready && !busy);
    removeFiles_->setEnabled(!busy && !imports_.isEmpty());
    backupNvm_->setEnabled(machine_.loaded && machine_.nvram_data && !importDiscard_ && !busy &&
                           !storage_.provision && !storage_.defaults_boot);
}

void PhoneWindow::addImportFiles(const QStringList &paths)
{
    for (const QString &path : paths) {
        if (imports_.size() == 256) break;
        QFileInfo file(path);
        if (!file.isFile() || file.size() > UINT32_MAX) continue;
        const QString ext = file.suffix().toLower();
        ImportItem item{};
        item.path = file.absoluteFilePath();
        item.size = static_cast<uint32_t>(file.size());
        item.type = ext == "jar" ? 10 : pictureExtension(ext) ? 7 : 6;
        imports_.append(item);
    }
    refreshImportQueue();
}

static const UserFile *javaRegistry(const UserDirectory &directory)
{
    for (unsigned i = 0; i < directory.count; ++i)
        if (!strcmp(directory.files[i].name, "FFS_JAVA_03")) return &directory.files[i];
    return nullptr;
}

void PhoneWindow::startNextImport()
{
    if (!importRunning_) return;
    int row = 0;
    while (row < imports_.size() && imports_[row].status != ImportStatus::Queued) ++row;
    if (row == imports_.size()) {
        importRunning_ = false;
        importMessage_ = "Import queue complete.";
        refreshFilesystem();
        return;
    }
    UserDirectory directory{};
    uint32_t registryId = 0;
    if (userfs_directory(&machine_, &directory)) {
        if (const UserFile *registry = javaRegistry(directory)) registryId = registry->id;
    }
    if (!loadFirmware(firmware_, State::Paused)) {
        importRunning_ = false;
        refreshImportQueue();
        return;
    }

    importCurrent_ = row;
    ImportItem &item = imports_[row];
    if (!content_import_open_decoded(&content_, &machine_, QFile::encodeName(item.path).constData(), decodePicture)) {
        importFailed(QString::fromUtf8(content_.error));
        return;
    }
    item.name = QString::fromLatin1(content_.name);
    item.size = content_.size;
    item.type = content_.type;
    item.registryId = registryId;
    item.status = ImportStatus::Starting;
    importStartNs_ = machine_.clock.nanoseconds;
    importStartInstructions_ = machine_.cpu.instructions;
    importMessage_ = "Booting the phone to start the next import.";
    setState(State::Running, "Running - importing queue");
    refreshFilesystem();
}

void PhoneWindow::openJavaGames()
{
    if (!loadFirmware(firmware_, State::Paused)) return;
    if (!content_java_menu(&content_, &machine_)) {
        importFailed(QString::fromUtf8(content_.error));
        return;
    }
    importStartNs_ = machine_.clock.nanoseconds;
    importStartInstructions_ = machine_.cpu.instructions;
    importMessage_ = "Booting the phone to open Java games.";
    setState(State::Running);
    refreshFilesystem();
}

void PhoneWindow::importAccepted()
{
    if (importCurrent_ < 0) importMessage_ = "Native Java menu opened on the phone screen.";
    else {
        ImportItem &item = imports_[importCurrent_];
        /* The core can add a suffix if this filename is already in use. */
        item.name = QString::fromLatin1(content_.name);
        item.status = ImportStatus::Storing;
        item.accepted = machine_.cpu.instructions;
        importMessage_ = item.type == 10 ?
            "Native Java installer running; wait for Installed, then press Close on the phone. Large apps may need Extended Java buffers." :
            "Import accepted by the guest; waiting for the flash task to store it.";
    }
    content_import_release(&content_);
    refreshFilesystem();
}

void PhoneWindow::importFailed(const QString &reason)
{
    if (importCurrent_ >= 0) {
        ImportItem &item = imports_[importCurrent_];
        importDiscard_ |= item.status == ImportStatus::Storing || content_.stage;
        item.error = reason;
        item.status = ImportStatus::Failed;
    }
    if (machine_.guest_busy) machine_.cpu.halted = 1; /* Reset before resuming a borrowed guest task. */
    content_import_release(&content_);
    importCurrent_ = -1;
    importRunning_ = false;
    importMessage_ = reason;
    refreshFilesystem();
}

void PhoneWindow::checkImport()
{
    if (importCurrent_ < 0 && !content_.active) return;
    const bool expired = machine_.clock.attached ? machine_.clock.nanoseconds - importStartNs_ >= UINT64_C(300000000000) :
                         machine_.cpu.instructions - importStartInstructions_ >= UINT64_C(600000000);
    if (expired) {
        importFailed("Phone did not finish within five emulated minutes. Reset before retrying.");
        setState(State::Stopped, importMessage_);
        return;
    }
    if (importCurrent_ < 0) return;
    ImportItem &item = imports_[importCurrent_];
    if (item.status != ImportStatus::Storing || machine_.cpu.instructions - item.accepted < 8000000) return;
    UserDirectory directory{};
    if (!userfs_directory(&machine_, &directory)) return;

    bool stored = false;
    if (item.type == 10) {
        const UserFile *registry = javaRegistry(directory);
        stored = registry && registry->id != item.registryId;
    } else {
        for (unsigned i = 0; i < directory.count; ++i) {
            const UserFile &file = directory.files[i];
            if (item.name == QString::fromLatin1(file.name) && file.type == item.type && file.size == item.size) {
                stored = true;
                break;
            }
        }
    }
    if (!stored) return;
    if (!saveStorage(true)) {
        importFailed("Could not commit the imported file to persistent NVM");
        return;
    }
    item.status = ImportStatus::Imported;
    importCurrent_ = -1;
    importMessage_ = "The phone stored the file and NVM was saved.";
    refreshFilesystem();
    startNextImport();
}

void PhoneWindow::exportFilesystem(bool backup)
{
    QFileDialog dialog(filesystem_, backup ? "Back up phone NVM" : "Export phone directory");
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilter(backup ? "NVM backup (*.snv)" : "Directory listing (*.tsv)");
    dialog.setDefaultSuffix(backup ? "snv" : "tsv");
    if (dialog.exec() != QDialog::Accepted) return;
    const QByteArray path = QFile::encodeName(dialog.selectedFiles().constFirst());
    if (storage_.active && plat_same_file(path.constData(), storage_.path)) {
        QMessageBox::warning(filesystem_, "Phone filesystem", "Choose a file other than the active phone snapshot.");
        return;
    }
    if (backup) {
        if (!nvram_save(&machine_, path.constData()))
            QMessageBox::warning(filesystem_, "NVM backup", QString::fromUtf8(machine_.error));
    } else {
        UserDirectory directory{};
        if (!userfs_directory(&machine_, &directory) || !userfs_export_directory(&directory, path.constData()))
            QMessageBox::warning(filesystem_, "Directory export", *directory.error ?
                                 QString::fromUtf8(directory.error) : "Could not export the phone directory.");
    }
}
