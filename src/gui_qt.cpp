/* Qt Widgets frontend for the same C emulator core used by the other frontends. */
#include "gui_qt.h"

extern "C" {
#include "firmware.h"
#include "host.h"
}

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

#include <cmath>
#include <cstring>

/* The array index is the model's guest keypad number. */
static const struct {
    const char *label;
    int hostKey;
} phoneKeys[] = {
    {"Soft left", Qt::Key_F1}, {"Up", Qt::Key_Up}, {"Soft right", Qt::Key_F2},
    {"Left", Qt::Key_Left}, {"OK", Qt::Key_Return}, {"Right", Qt::Key_Right},
    {"Call", Qt::Key_F3}, {"Down", Qt::Key_Down}, {"End", Qt::Key_F4},
    {"1", Qt::Key_1}, {"2", Qt::Key_2}, {"3", Qt::Key_3},
    {"4", Qt::Key_4}, {"5", Qt::Key_5}, {"6", Qt::Key_6},
    {"7", Qt::Key_7}, {"8", Qt::Key_8}, {"9", Qt::Key_9},
    {"*", Qt::Key_Asterisk}, {"0", Qt::Key_0}, {"#", Qt::Key_NumberSign},
    {"C / Back", Qt::Key_Backspace}
};
static_assert(sizeof(phoneKeys) / sizeof(phoneKeys[0]) == SGH_MAX_KEYS);

/* An executable under build/linux/ can also be started from a file manager. */
static QString modelsDirectory()
{
    const QDir executable(QApplication::applicationDirPath());
    const QStringList locations = {QDir::currentPath(), executable.absolutePath(),
                                   executable.filePath(".."), executable.filePath("../..")};
    for (const QString &location : locations) {
        const QString models = QDir(location).absoluteFilePath("models");
        if (QFileInfo(models).isDir()) return models;
    }
    return {};
}

/* Use the physical key when available: releasing Shift before '*' or '#'
   can change the logical key name on the release event. */
static quint32 keyIdentity(const QKeyEvent *event)
{
    const quint32 scanCode = event->nativeScanCode();
    return scanCode ? scanCode : (0x80000000u | quint32(event->key()));
}

class LcdView final : public QWidget {
public:
    LcdView(const Machine &machine, QWidget *parent) : QWidget(parent), machine_(machine)
    {
        setAccessibleName("Phone LCD");
        setAttribute(Qt::WA_OpaquePaintEvent);
    }

    void reset()
    {
        pattern_ = {};
        target_ = {};
        display_ = {};
        pending_ = false;
        capture(true);
        resetBlend();
    }

    void setPattern(bool enabled)
    {
        if (enabled && pattern_.isNull()) {
            pattern_ = QImage(target_.size(), QImage::Format_RGB32);
            if (pattern_.isNull()) return;
            Machine test{};
            test.width = machine_.width;
            test.height = machine_.height;
            test.pixels = reinterpret_cast<uint32_t *>(pattern_.bits());
            machine_pattern(&test, 1);
            for (int row = 0; row < pattern_.height(); ++row) {
                auto *line = reinterpret_cast<QRgb *>(pattern_.scanLine(row));
                for (int column = 0; column < pattern_.width(); ++column)
                    line[column] |= 0xff000000u;
            }
        }
        if (!enabled) pattern_ = {};
        update();
    }

    void setGhosting(bool enabled)
    {
        ghosting_ = enabled;
        resetBlend();
    }

    /* Prefer a settled redraw, but capture within 33 ms of guest time when
       drawing never stops. The paint event only sees complete snapshots. */
    void capture(bool force = false)
    {
        if (!machine_.pixels) return;
        const uint64_t now = machine_.clock.nanoseconds;
        if (writes_ != machine_.lcd.writes || commands_ != machine_.lcd.commands) {
            writes_ = machine_.lcd.writes;
            commands_ = machine_.lcd.commands;
            if (!pending_) firstChange_ = now;
            lastChange_ = now;
            pending_ = true;
        }
        if (!force && machine_.clock.attached &&
            (!pending_ || (now - lastChange_ < 3000000 && now - firstChange_ < 33333333)))
            return;

        if (target_.isNull()) target_ = QImage(machine_.width, machine_.height, QImage::Format_RGB32);
        if (target_.isNull()) return;
        for (int row = 0; row < target_.height(); ++row) {
            const uint32_t *pixels = machine_.pixels + row * machine_.width;
            auto *line = reinterpret_cast<QRgb *>(target_.scanLine(row));
            for (int column = 0; column < target_.width(); ++column)
                line[column] = 0xff000000u | pixels[column];
        }
        pending_ = false;
        if (!ghosting_ && pattern_.isNull()) update();
    }

    void blend()
    {
        const QImage &target = pattern_.isNull() ? target_ : pattern_;
        if (!ghosting_ || target.isNull()) return;
        if (display_.size() != target.size()) resetBlend();
        const unsigned retain = static_cast<unsigned>(256 * std::exp(-blendClock_.restart() / 75.0));
        for (int row = 0; row < target.height(); ++row) {
            const auto *input = reinterpret_cast<const QRgb *>(target.constScanLine(row));
            auto *output = reinterpret_cast<QRgb *>(display_.scanLine(row));
            for (int column = 0; column < target.width(); ++column) {
                QRgb color = 0xff000000u;
                for (unsigned shift = 0; shift < 24; shift += 8) {
                    const int from = (output[column] >> shift) & 255;
                    const int to = (input[column] >> shift) & 255;
                    const unsigned value = qAbs(from - to) <= 2 ? to :
                        (from * retain + to * (256 - retain) + 128) >> 8;
                    color |= value << shift;
                }
                output[column] = color;
            }
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const QImage &frame = ghosting_ ? display_ : pattern_.isNull() ? target_ : pattern_;
        QPainter painter(this);
        if (frame.isNull()) painter.fillRect(rect(), Qt::black);
        else painter.drawImage(rect(), frame); /* Integer zoom, nearest neighbor. */
    }

private:
    void resetBlend()
    {
        display_ = pattern_.isNull() ? target_ : pattern_;
        blendClock_.start();
        update();
    }

    const Machine &machine_;
    QImage target_, display_, pattern_;
    QElapsedTimer blendClock_;
    uint64_t writes_ = 0, commands_ = 0, firstChange_ = 0, lastChange_ = 0;
    bool pending_ = false, ghosting_ = false;
};

PhoneWindow::PhoneWindow() : settings_("state/emulator.ini", QSettings::IniFormat),
                             audioSettings_("state/audio.ini", QSettings::IniFormat)
{
    setWindowTitle("SGHemu");
    setFocusPolicy(Qt::StrongFocus);
    wallClock_.start();
    zoom_ = qBound(1, settings_.value("look_and_feel/lcd_zoom", 2).toInt(), 3);
    ghosting_ = settings_.value("look_and_feel/lcd_ghosting", false).toBool();
    detune_ = settings_.value("look_and_feel/c100_ma_detune", false).toBool();
    textInput_ = settings_.value("emulation/keyboard_text_input", false).toBool();
    hostTime_ = settings_.value("emulation/sync_time", true).toBool();
    javaExtended_ = settings_.value("emulation/extended_java_buffers", false).toBool();
    highPriority_ = settings_.value("emulation/high_priority", false).toBool() && host_high_priority(1);
    batteryPercent_ = qBound(0, settings_.value("battery/percent", 100).toInt(), 100);
    charging_ = settings_.value("battery/charging", false).toBool();
    cellEnabled_ = settings_.value("cellular/enabled", true).toBool();
    networkName_ = settings_.value("cellular/network", "Tele2").toString();
    gsmMode_ = settings_.value("cellular/gsm_mode", 0).toUInt();
    if (gsmMode_ > 2) gsmMode_ = 0;
    audioEnabled_ = audioSettings_.value("audio/enabled", true).toBool();
    audioTicks_ = audioSettings_.value("audio/ticks_hz", MA3_DEFAULT_TICKS_HZ).toUInt();
    if (audioTicks_ < 48000 || audioTicks_ > 100000000) audioTicks_ = MA3_DEFAULT_TICKS_HZ;

    auto *file = menuBar()->addMenu("&File");
    file->addAction("Open &firmware folder...", this, &PhoneWindow::openFirmwareFolder, QKeySequence::Open);
    file->addAction("Open &model config (advanced)...", this, &PhoneWindow::openModel);
    file->addAction("Open first firmware image (advanced)...", this, [this] { openRom(false); });
    file->addAction("Open second firmware image (advanced)...", this, [this] { openRom(true); });
    snapshot_ = file->addAction("Import NVM snapshot and reset...", this, &PhoneWindow::openSnapshot);
    file->addAction("Phone filesystem manager...", this, &PhoneWindow::showFilesystem);

    auto *emulation = menuBar()->addMenu("&Emulation");
    run_ = emulation->addAction("&Run", this, [this] { setState(State::Running); }, Qt::Key_F5);
    pause_ = emulation->addAction("&Pause", this, [this] {
        setState(State::Paused);
        saveStorage();
    }, Qt::Key_F6);
    step_ = emulation->addAction("Step &instruction", this, [this] {
        if (emulate(1, State::Paused)) {
            setState(State::Paused, "Paused - stepped");
            saveStorage();
        }
    }, Qt::Key_F10);
    emulation->addSeparator();
    addToolMenus(file, emulation);
    stop_ = file->addAction("&Stop / Reset", this, [this] {
        loadFirmware(firmware_, State::Stopped, storage_.defaults_boot != 0);
    }, QKeySequence("Ctrl+R"));
    file->addSeparator();
    file->addAction("&Quit", this, &QWidget::close, QKeySequence::Quit);

    auto *view = menuBar()->addMenu("&View");
    auto *zoomActions = new QActionGroup(this);
    for (int zoom = 1; zoom <= 3; ++zoom) {
        auto *action = view->addAction(QString("LCD zoom &%1x").arg(zoom));
        zoomActions->addAction(action);
        action->setCheckable(true);
        action->setChecked(zoom_ == zoom);
        connect(action, &QAction::triggered, this, [this, zoom] {
            zoom_ = zoom;
            settings_.setValue("look_and_feel/lcd_zoom", zoom);
            fitLcd();
        });
    }
    view->addSeparator();
    patternAction_ = view->addAction("LCD &test pattern");
    patternAction_->setCheckable(true);
    connect(patternAction_, &QAction::toggled, this, [this](bool enabled) { lcd_->setPattern(enabled); });
    view->addAction("&Diagnostics...", this, &PhoneWindow::showDiagnostics);

    auto *look = menuBar()->addMenu("L&&F");
    auto *ghosting = look->addAction("LCD ghosting");
    ghosting->setCheckable(true);
    ghosting->setChecked(ghosting_);
    connect(ghosting, &QAction::triggered, this, [this](bool enabled) {
        ghosting_ = enabled;
        settings_.setValue("look_and_feel/lcd_ghosting", enabled);
        lcd_->setGhosting(enabled);
    });
    auto *water = look->addAction("Water (not accurate)");
    water->setCheckable(true);
    connect(water, &QAction::triggered, this, [this](bool enabled) {
        water_ = enabled;
        ma3_set_water(&machine_.ma3, enabled);
        updateUi();
    });
    auto *detune = look->addAction("C100 MA detune (-12.5 cents)");
    detune->setCheckable(true);
    detune->setChecked(detune_);
    connect(detune, &QAction::triggered, this, [this](bool enabled) {
        detune_ = enabled;
        settings_.setValue("look_and_feel/c100_ma_detune", enabled);
        ma3_set_detune(&machine_.ma3, enabled && strstr(machine_.name, "C100"));
    });
    auto *help = menuBar()->addMenu("&Help");
    help->addAction("&About SGHemu", this, [this] {
        QMessageBox::about(this, "About SGHemu", "Samsung SGH-C100 emulator workbench (Qt 5)\n\n"
            "Ctrl+O opens a firmware folder. F5 runs, F6 pauses, F10 steps and Ctrl+R resets.\n"
            "Use arrows, Enter, digits, * and #, F1/F2 (soft keys), F3/F4 (Call/End) and Backspace (Clear).\n"
            "Ctrl+T toggles English abc/ABC text input.");
    });
    for (auto *menu : menuBar()->findChildren<QMenu *>())
        connect(menu, &QMenu::aboutToShow, this, &PhoneWindow::releaseKeys);

    auto *controls = addToolBar("Emulation controls");
    controls->setMovable(false);
    controls->setFloatable(false);
    controls->setFocusPolicy(Qt::NoFocus);
    controls->setToolButtonStyle(Qt::ToolButtonTextOnly);
    controls->addActions({run_, pause_, stop_});

    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    lcd_ = new LcdView(machine_, panel);
    lcd_->setGhosting(ghosting_);
    layout->addWidget(lcd_, 0, Qt::AlignHCenter);

    auto *keypad = new QGridLayout;
    layout->addLayout(keypad);
    for (int column = 0; column < 3; ++column) keypad->setColumnStretch(column, 1);
    for (unsigned key = 0; key < SGH_MAX_KEYS; ++key) {
        auto *button = new QPushButton(phoneKeys[key].label, panel);
        buttons_[key] = button;
        button->setFocusPolicy(Qt::NoFocus); /* Route host keys to the whole phone. */
        connect(button, &QPushButton::pressed, this, [this, key] {
            mouseKeys_ |= 1u << key;
            updateKey(key);
        });
        connect(button, &QPushButton::released, this, [this, key] {
            mouseKeys_ &= ~(1u << key);
            updateKey(key);
        });
        /* Clear/Back fills row 3; move the digit rows down to leave room. */
        const int row = key / 3;
        if (key == SGH_MAX_KEYS - 1)
            keypad->addWidget(button, 3, 0, 1, 3);
        else
            keypad->addWidget(button, row < 3 ? row : row + 1, key % 3);
    }
    layout->addStretch();
    setCentralWidget(panel);
    speedLabel_ = new QLabel;
    speedLabel_->setToolTip("Emulated time / elapsed host time");
    statusBar()->addPermanentWidget(speedLabel_);

    /* Keep each callback short without adding idle time when audio falls behind. */
    runTimer_.setInterval(4);
    runTimer_.setTimerType(Qt::PreciseTimer);
    connect(&runTimer_, &QTimer::timeout, this, &PhoneWindow::runSlice);
    uiTimer_.setInterval(33);
    connect(&uiTimer_, &QTimer::timeout, this, &PhoneWindow::refresh);
    uiTimer_.start();
    fitLcd();
    setState(State::Empty);

    if (settings_.value("diagnostics/logging", false).toBool()) applyLogging(true);
    QTimer::singleShot(0, this, &PhoneWindow::openRecentFirmware);
}

PhoneWindow::~PhoneWindow()
{
    applyLogging(false);
    content_import_release(&content_);
    nvram_store_release(&storage_);
    machine_destroy(&machine_);
}

void PhoneWindow::openFirmwareFolder()
{
    const QString models = modelsDirectory();
    if (models.isEmpty()) {
        QMessageBox::warning(this, "Firmware folder", "Cannot find models/. Run SGHemu from the repository root.");
        return;
    }
    if (runTimer_.isActive()) setState(State::Paused);
    const QString folder = QFileDialog::getExistingDirectory(this, "Open firmware folder");
    if (folder.isEmpty()) return;

    const QByteArray romFolder = QFile::encodeName(folder);
    const QByteArray modelsRoot = QFile::encodeName(models);
    FirmwareMatch matches[FIRMWARE_MAX_MATCHES];
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const unsigned count = firmware_scan(romFolder.constData(), modelsRoot.constData(),
                                         matches, FIRMWARE_MAX_MATCHES);
    QApplication::restoreOverrideCursor();
    if (!count) {
        QMessageBox::information(this, "Firmware folder",
            "No supported ROM pair found. Both files must match a known firmware version.");
        return;
    }

    int choice = 0;
    if (count > 1) {
        QStringList versions;
        for (unsigned i = 0; i < count; ++i)
            versions << QString("%1. %2").arg(i + 1).arg(QString::fromUtf8(matches[i].name));
        bool accepted = false;
        const QString version = QInputDialog::getItem(this, "Select firmware", "Firmware version:",
                                                      versions, 0, false, &accepted);
        if (!accepted) return;
        choice = versions.indexOf(version);
    }
    const FirmwareMatch &match = matches[choice];
    loadFirmware({match.model, match.rom[0], match.rom[1]}, State::Running);
}

void PhoneWindow::openModel()
{
    if (runTimer_.isActive()) setState(State::Paused);
    const QString path = QFileDialog::getOpenFileName(this, "Open model config", modelsDirectory(),
                                                      "SGH model configs (*.cfg);;All files (*)");
    if (!path.isEmpty()) loadFirmware({QFile::encodeName(path), {}, {}}, State::Stopped);
}

void PhoneWindow::openRom(bool second)
{
    if (runTimer_.isActive()) setState(State::Paused);
    Firmware next = firmware_;
    if (next.model.isEmpty()) {
        const QString models = modelsDirectory();
        if (models.isEmpty()) {
            QMessageBox::warning(this, "Firmware image", "Cannot find models/. Open a model config first.");
            return;
        }
        next.model = QFile::encodeName(QDir(models).filePath("c100.cfg"));
    }
    QByteArray &image = second ? next.secondRom : next.firstRom;
    const QString path = QFileDialog::getOpenFileName(this,
        second ? "Open second firmware image" : "Open first firmware image", QFile::decodeName(image),
        "Firmware images (*.bin *.rom);;All files (*)");
    if (path.isEmpty()) return;
    image = QFile::encodeName(path);
    loadFirmware(next, State::Stopped);
}

void PhoneWindow::openSnapshot()
{
    if (runTimer_.isActive()) setState(State::Paused);
    const QString path = QFileDialog::getOpenFileName(this, "Import NVM snapshot and reset phone", {},
                                                      "SGH NVM snapshots (*.snv);;All files (*)");
    if (!path.isEmpty()) loadFirmware(firmware_, State::Stopped, false, QFile::encodeName(path));
}

void PhoneWindow::openRecentFirmware()
{
    const QString model = settings_.value("recent_firmware/model").toString();
    const QString rom = settings_.value("recent_firmware/rom1").toString();
    const QString second = settings_.value("recent_firmware/rom2").toString();
    if (!machine_.loaded && QFileInfo(model).isFile() &&
        (rom.isEmpty() || QFileInfo(rom).isFile()) && (second.isEmpty() || QFileInfo(second).isFile()))
        loadFirmware({QFile::encodeName(model), QFile::encodeName(rom), QFile::encodeName(second)}, State::Running);
}

bool PhoneWindow::loadFirmware(Firmware firmware, State next, bool initializeDefaults,
                               const QByteArray &snapshot)
{
    if (runTimer_.isActive()) setState(State::Paused);
    if (!saveStorage()) return false;
    if (content_.active || importCurrent_ >= 0) importFailed("Phone reset before the import finished");

    /* Save before releasing the lock. Loading the next model tears down the
       old machine, so the store must be closed before loading or resetting. */
    const bool logging = loggingAction_->isChecked();
    applyLogging(false);
    nvram_store_release(&storage_);
    importDiscard_ = false;
    simStorage_.clear();
    /* Keep the selections if loading fails. An advanced ROM override may
       need both images before the selected config can boot. */
    firmware_ = firmware;

    if (!machine_load_pair(&machine_, firmware.model.constData(),
                           firmware.firstRom.isEmpty() ? nullptr : firmware.firstRom.constData(),
                           firmware.secondRom.isEmpty() ? nullptr : firmware.secondRom.constData(), nullptr) ||
        !nvram_store_open(&storage_, &machine_, nullptr)) {
        next = State::Empty;
        stateDetail_ = QString::fromUtf8(machine_.error);
    } else if (machine_.sim.attached && storage_.active) {
        simStorage_ = QByteArray(storage_.path) + ".sim";
        if (!sim_card_load_storage(&machine_.sim.card, simStorage_.constData())) {
            next = State::Empty;
            stateDetail_ = QString::fromUtf8(machine_.sim.card.error);
        }
    }
    if (next == State::Empty) {
        const QString error = stateDetail_;
        nvram_store_release(&storage_);
        machine_destroy(&machine_);
        simStorage_.clear();
        setWindowTitle("SGHemu");
        lcd_->reset();
        fitLcd();
        patternAction_->setChecked(false);
        callRevision_ = ~0u;
        setState(State::Empty);
        updateCall();
        refreshFilesystem();
        if (logging) applyLogging(true);
        QMessageBox::warning(this, "Cannot load firmware", error);
        return false;
    }

    /* The source stays untouched. A bad snapshot leaves the existing store
       loaded and the phone stopped so the user can retry the import. */
    QString snapshotError;
    if (!snapshot.isEmpty() && !nvram_store_import(&storage_, &machine_, snapshot.constData()))
        snapshotError = QString::fromUtf8(machine_.error);
    storage_.defaults_boot = initializeDefaults;
    if (simOverride_ >= 0 && machine_.sim.attached) machine_.sim.present = simOverride_;
    continuation_ = machine_.bringup;
    battery_set(&machine_.battery, batteryPercent_, charging_);
    machine_.java_extended = javaExtended_;
    if (machine_.cellular.ready) {
        const QByteArray name = networkName_.toUtf8();
        if (!cellular_set_network(&machine_.cellular, &machine_.bus, cellEnabled_, name.constData())) {
            networkName_ = "Tele2";
            cellular_set_network(&machine_.cellular, &machine_.bus, cellEnabled_, "Tele2");
        }
        machine_.cellular.mode = gsmMode_;
        phone_net_set_mode(&machine_.network, cellEnabled_ ? gsmMode_ : 0);
    }
    callRevision_ = ~0u;
    rtc_set_sync(&machine_.rtc, hostTime_);
    syncClock();
    openAudio();
    if (logging) applyLogging(true);
    lastSave_ = wallClock_.elapsed();
    speed_ = {};
    lcd_->reset();
    patternAction_->setChecked(false);
    fitLcd();
    setWindowTitle(QString("SGHemu - %1").arg(QString::fromUtf8(machine_.name)));
    settings_.setValue("recent_firmware/model", QFileInfo(QFile::decodeName(firmware.model)).absoluteFilePath());
    settings_.setValue("recent_firmware/rom1", QFile::decodeName(firmware.firstRom));
    settings_.setValue("recent_firmware/rom2", QFile::decodeName(firmware.secondRom));
    setState(next);
    updateCall();
    refreshFilesystem();
    if (!snapshotError.isEmpty()) QMessageBox::warning(this, "Cannot import NVM snapshot", snapshotError);
    return snapshotError.isEmpty();
}

bool PhoneWindow::saveStorage(bool commitImport)
{
    if (!storage_.active) return true;
    QString error;
    /* During the first boot, the core saves the defaults only when the guest
       finishes initializing them. A pause or close must not save half of them. */
    if (!storage_.defaults_boot && !importDiscard_ && (importCurrent_ < 0 || commitImport) &&
        !nvram_store_flush(&storage_, &machine_))
        error = QString::fromUtf8(machine_.error);
    else if (!simStorage_.isEmpty() && !sim_card_save_storage(&machine_.sim.card, simStorage_.constData()))
        error = QString::fromUtf8(machine_.sim.card.error);
    else {
        lastSave_ = wallClock_.elapsed();
        return true;
    }
    if (runTimer_.isActive()) setState(State::Paused);
    QMessageBox::warning(this, "Cannot save phone state", error);
    return false;
}

void PhoneWindow::setState(State state, const QString &detail)
{
    state_ = state;
    stateDetail_ = detail;
    const bool running = state == State::Running;
    if (running && !runTimer_.isActive()) {
        audioGuestStart_ = machine_.clock.attached ? machine_.clock.nanoseconds : machine_.cpu.cycles;
        audioClock_.start();
        runTimer_.start();
    } else if (!running) {
        runTimer_.stop();
        releaseKeys();
        lcd_->capture(true);
    }
    phone_io_pause(&machine_.phone_io, !running);
    audio_output_pause(machine_.ma3.output, !running);
    updateUi();
}

void PhoneWindow::updateUi()
{
    const bool running = state_ == State::Running;
    const bool canRun = machine_.loaded && !running && !machine_.cpu.halted;
    run_->setEnabled(canRun);
    pause_->setEnabled(running);
    stop_->setEnabled(machine_.loaded);
    step_->setEnabled(canRun);
    snapshot_->setEnabled(storage_.active);
    continuationAction_->setChecked(continuation_);
    simAction_->setEnabled(machine_.sim.attached);
    simAction_->setChecked(machine_.sim.present);
    clockAction_->setEnabled(machine_.rtc.attached);
    clockAction_->setChecked(hostTime_);
    priorityAction_->setChecked(highPriority_);
    javaAction_->setEnabled(machine_.java_buffer_get != 0);
    batteryLevel_->setEnabled(machine_.battery.enabled);
    batteryCable_->setEnabled(machine_.battery.enabled || machine_.battery.charger_port);
    cellAction_->setEnabled(machine_.cellular.ready != 0);
    cellAction_->setChecked(cellEnabled_);
    networkAction_->setEnabled(cellAction_->isEnabled());
    smsAction_->setEnabled(machine_.cellular.enabled && machine_.cellular.name_ptr && machine_.sim.present);
    callAction_->setEnabled(smsAction_->isEnabled());
    for (auto *gsm : gsmActions_) gsm->setEnabled(machine_.network.txn_start != 0);
    audioAction_->setChecked(audioEnabled_);
    patternAction_->setEnabled(machine_.loaded);
    for (auto *button : buttons_) button->setEnabled(machine_.loaded);

    QString message = stateDetail_;
    if (message.isEmpty()) {
        switch (state_) {
        case State::Empty: message = "No firmware - Ctrl+O to open a folder"; break;
        case State::Running:
            message = storage_.provision || storage_.defaults_boot ? "Preparing phone storage..." : "Running";
            break;
        case State::Paused: message = "Paused"; break;
        case State::Stopped: message = "Stopped"; break;
        }
    }
    if (water_) message += " | Water";
    statusBar()->showMessage(message);
    const QString audioError = *machine_.ma3.output_error ? QString::fromUtf8(machine_.ma3.output_error) : audioError_;
    statusBar()->setToolTip(audioError.isEmpty() ? message : message + '\n' + audioError);
    emu_speed_sample(&speed_, wallClock_.nsecsElapsed(), 1000000000, machine_.clock.nanoseconds,
                     running && machine_.clock.attached);
    speedLabel_->setText(!running ? "0.0%" : speed_.valid ? QString::number(speed_.percent, 'f', 1) + '%' : "--%");
}

bool PhoneWindow::emulate(unsigned steps, State afterReset)
{
    for (unsigned done = 0; done < steps;) {
        const int provision = nvram_store_step(&storage_, &machine_);
        if (!provision) {
            machine_.cpu.halted = 1;
            setState(State::Stopped, QString::fromUtf8(machine_.error));
            return false;
        }
        if (provision == 2) {
            /* After formatting, boot to create the UI defaults. The next
               reset starts the phone with the finished, saved snapshot. */
            loadFirmware(firmware_, afterReset, storage_.provision && storage_.stage == 3);
            return false;
        }
        if (!machine_.cellular.stage && (machine_.battery.stage || !content_.active) &&
            !battery_step(&machine_.battery, &machine_.cpu)) {
            setState(State::Stopped, "Guest battery event failed");
            return false;
        }
        if (content_.active && !machine_.cellular.stage && !machine_.battery.stage) {
            const int imported = content_import_step(&content_, &machine_);
            if (!imported) {
                const QString error = QString::fromUtf8(content_.error);
                importFailed(error);
                setState(State::Paused, error);
                QMessageBox::warning(this, "Phone import failed", error);
                return false;
            }
            if (imported == 2) importAccepted();
        }
        /* Provisioning and host-requested guest calls watch for exact PCs. */
        const bool singleStep = storage_.provision || storage_.defaults_boot || content_.active ||
                                (machine_.battery.enabled && !machine_.battery.adc);
        const unsigned batch = singleStep ? 1 : qMin(steps - done, 64u);
        if (machine_run(&machine_, batch, continuation_) != batch) {
            const char *reason = machine_.cpu.error ? machine_.cpu.error : "Unmapped hardware access";
            logStop(reason);
            if (importCurrent_ >= 0 || content_.active) importFailed(QString::fromUtf8(reason));
            const QString pc = QString::number(machine_.cpu.r[15], 16);
            setState(State::Stopped, QString("Stopped at 0x%1: %2").arg(pc, QString::fromUtf8(reason)));
            return false;
        }
        done += batch;
    }
    return true;
}

void PhoneWindow::runSlice()
{
    /* Keep a small PCM lead, then yield to Qt. Silent execution is unpaced. */
    const bool paced = audio_output_live(machine_.ma3.output) || phone_io_live(&machine_.phone_io);
    const auto guestTime = [this] {
        return machine_.clock.attached ? machine_.clock.nanoseconds :
               (machine_.cpu.cycles - audioGuestStart_) * UINT64_C(1000000000) / audioTicks_;
    };
    uint64_t start = machine_.clock.attached ? audioGuestStart_ : 0;
    const uint64_t due = start + audioClock_.nsecsElapsed();
    if (paced && due > guestTime() + 100000000) {
        audioGuestStart_ = machine_.clock.attached ? machine_.clock.nanoseconds : machine_.cpu.cycles;
        start = machine_.clock.attached ? audioGuestStart_ : 0;
        audioClock_.restart();
    }
    QElapsedTimer slice;
    slice.start();
    do {
        if (paced && guestTime() > start + uint64_t(audioClock_.nsecsElapsed()) + 20000000) break;
        updateTextKeys();
        if (profileLog_ && (profilePcs_.size() < 4096 || profilePcs_.contains(machine_.cpu.r[15])))
            ++profilePcs_[machine_.cpu.r[15]];
        if (!emulate(512, State::Running)) return;
        if (machine_.clock.attached) lcd_->capture();
    } while (slice.elapsed() < runTimer_.interval());
}

void PhoneWindow::refresh()
{
    const qint64 now = wallClock_.elapsed();
    if (hostTime_ && machine_.rtc.attached && now - lastClockSync_ >= 1000) syncClock();
    if (!machine_.clock.attached && runTimer_.isActive()) lcd_->capture();
    lcd_->blend();

    unsigned led = machine_.phone_io.led & 7u;
    if (machine_.ma3.led_tick > machine_.phone_io.led_tick)
        led = machine_.ma3.led_on ? (machine_.ma3.led_color & 7u) : 0;
    if (led != led_) {
        led_ = led;
        QPixmap icon(16, 16);
        icon.fill(QColor((led & 1) ? 255 : 0, (led & 2) ? 255 : 0, (led & 4) ? 255 : 0));
        setWindowIcon(QIcon(icon));
    }
    updateUi();
    updateDiagnostics();
    updateCall();
    checkImport();
    logProgress();
    if (runTimer_.isActive() && storage_.active && now - lastSave_ >= 5000) saveStorage();
}

void PhoneWindow::fitLcd()
{
    lcd_->setFixedSize((machine_.loaded ? machine_.width : 128) * zoom_,
                       (machine_.loaded ? machine_.height : 128) * zoom_);
    adjustSize();
}

void PhoneWindow::openAudio()
{
    audioError_.clear();
    if (machine_.ma3.attached && !machine_.clock.attached)
        ma3_set_clock(&machine_.ma3, &machine_.cpu.cycles, audioTicks_);
    if (audioEnabled_ && machine_.ma3.attached && !ma3_open_audio(&machine_.ma3, 1, nullptr)) {
        audioError_ = QString::fromUtf8(machine_.ma3.output_error);
        audioEnabled_ = false;
    }
    ma3_set_detune(&machine_.ma3, detune_ && strstr(machine_.name, "C100"));
    ma3_set_water(&machine_.ma3, water_);
    if (!machine_.ma3.output && !phone_io_open(&machine_.phone_io, 1, nullptr) && audioError_.isEmpty())
        audioError_ = "Could not open the host audio device for phone tones";
}

void PhoneWindow::syncClock()
{
    lastClockSync_ = wallClock_.elapsed();
    if (!hostTime_ || !machine_.rtc.attached || host_sync_rtc(&machine_.rtc)) return;
    hostTime_ = false;
    rtc_set_sync(&machine_.rtc, 0);
    settings_.setValue("emulation/sync_time", false);
    QMessageBox::warning(this, "Phone clock", "The host date is outside this model's RTC range.");
}

void PhoneWindow::updateKey(unsigned key)
{
    /* Releasing a key on the PC must not lift the same on-screen button. */
    const bool held = ((mouseKeys_ | textHeld_) & (1u << key)) || keyboardKeys_.key(key, 0) != 0;
    machine_key(&machine_, key, held);
    buttons_[key]->setDown(held);
}

void PhoneWindow::releaseKeys()
{
    const uint32_t held = machine_.keys;
    mouseKeys_ = textHeld_ = 0;
    key_text_reset(&textKeys_);
    keyboardKeys_.clear();
    for (unsigned key = 0; key < SGH_MAX_KEYS; ++key)
        if (held & (1u << key)) updateKey(key);
}

void PhoneWindow::updateTextKeys()
{
    unsigned key;
    int down;
    if (!key_text_tick(&textKeys_, machine_.clock.nanoseconds, &key, &down)) return;
    if (down) textHeld_ |= 1u << key;
    else textHeld_ &= ~(1u << key);
    updateKey(key);
}

bool PhoneWindow::event(QEvent *event)
{
    if (event->type() == QEvent::WindowDeactivate) releaseKeys();
    return QMainWindow::event(event);
}

void PhoneWindow::keyPressEvent(QKeyEvent *event)
{
    if (machine_.loaded && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        int hostKey = event->key();
        if (hostKey == Qt::Key_Enter || hostKey == Qt::Key_Space) hostKey = Qt::Key_Return;
        const bool typing = textInput_ && machine_.clock.attached && runTimer_.isActive();
        if (typing && event->key() == Qt::Key_Escape) {
            releaseKeys();
            event->accept();
            return;
        }
        if (typing && event->text().size() == 1) {
            const QChar character = event->text().front();
            if (character.isLetter() || character == ' ' || character == '.') {
                if (!key_text_char(&textKeys_, character.unicode())) QApplication::beep();
                event->accept();
                return;
            }
        }
        for (unsigned key = 0; key < SGH_MAX_KEYS; ++key) {
            if (phoneKeys[key].hostKey != hostKey) continue;
            if (typing && (textKeys_.count || textKeys_.pressed || hostKey == Qt::Key_Backspace)) {
                if (!key_text_key(&textKeys_, key)) QApplication::beep();
            } else if (!event->isAutoRepeat()) {
                keyboardKeys_.insert(keyIdentity(event), key);
                updateKey(key);
            }
            event->accept();
            return;
        }
    }
    QMainWindow::keyPressEvent(event);
}

void PhoneWindow::keyReleaseEvent(QKeyEvent *event)
{
    auto held = keyboardKeys_.find(keyIdentity(event));
    if (held == keyboardKeys_.end()) {
        QMainWindow::keyReleaseEvent(event);
        return;
    }
    if (!event->isAutoRepeat()) {
        const unsigned key = held.value();
        keyboardKeys_.erase(held);
        updateKey(key);
    }
    event->accept();
}

void PhoneWindow::closeEvent(QCloseEvent *event)
{
    if (runTimer_.isActive()) setState(State::Paused);
    if (!saveStorage()) {
        event->ignore();
        return;
    }
    for (auto *dialog : findChildren<QDialog *>(QString(), Qt::FindDirectChildrenOnly)) dialog->hide();
    settings_.sync();
    audioSettings_.sync();
    event->accept();
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const QString models = modelsDirectory();
    if (!models.isEmpty()) QDir::setCurrent(QFileInfo(models).absolutePath());
    PhoneWindow window;
    window.show();
    return app.exec();
}
