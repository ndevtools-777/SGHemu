#include "gui_qt.h"

extern "C" {
#include "host.h"
}

#include <QActionGroup>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStatusBar>
#include <QTextStream>
#include <QVBoxLayout>

#include <cerrno>
#include <cstring>

static QAction *checkAction(QMenu *menu, const QString &text, bool checked)
{
    auto *action = menu->addAction(text);
    action->setCheckable(true);
    action->setChecked(checked);
    return action;
}

void PhoneWindow::addToolMenus(QMenu *file, QMenu *emulation)
{
    loggingAction_ = checkAction(file, "&Logging", false);
    connect(loggingAction_, &QAction::triggered, this, [this](bool enabled) {
        if (!applyLogging(enabled)) {
            QMessageBox::warning(this, "Logging", "Could not open the diagnostic logs in build/.");
            return;
        }
        settings_.setValue("diagnostics/logging", enabled);
    });

    continuationAction_ = checkAction(emulation, "Diagnostic: continue &unknown accesses", false);
    connect(continuationAction_, &QAction::triggered, this, [this](bool enabled) { continuation_ = enabled; });
    simAction_ = checkAction(emulation, "Virtual &SIM inserted (reset)", false);
    connect(simAction_, &QAction::triggered, this, [this](bool inserted) {
        const int previous = simOverride_;
        simOverride_ = inserted;
        const State next = runTimer_.isActive() ? State::Running : State::Paused;
        if (!loadFirmware(firmware_, next, storage_.defaults_boot != 0)) simOverride_ = previous;
        updateUi();
    });
    priorityAction_ = checkAction(emulation, "High process priority", highPriority_);
    connect(priorityAction_, &QAction::triggered, this, [this](bool enabled) {
        if (host_high_priority(enabled)) {
            highPriority_ = enabled;
            settings_.setValue("emulation/high_priority", enabled);
        } else {
            QMessageBox::warning(this, "Process priority", QString::fromLocal8Bit(strerror(errno)));
            priorityAction_->setChecked(highPriority_);
        }
    });
    javaAction_ = checkAction(emulation, "Extended Java buffers (next import/reset)", javaExtended_);
    connect(javaAction_, &QAction::triggered, this, [this](bool enabled) {
        javaExtended_ = enabled;
        settings_.setValue("emulation/extended_java_buffers", enabled);
    });
    clockAction_ = checkAction(emulation, "Sync phone date/time to host", hostTime_);
    connect(clockAction_, &QAction::triggered, this, [this](bool enabled) {
        hostTime_ = enabled;
        rtc_set_sync(&machine_.rtc, enabled);
        syncClock();
        settings_.setValue("emulation/sync_time", hostTime_);
        updateUi();
    });
    auto *text = checkAction(emulation, "Keyboard text input (abc/ABC)", textInput_);
    text->setShortcut(QKeySequence("Ctrl+T"));
    connect(text, &QAction::triggered, this, [this](bool enabled) {
        releaseKeys();
        textInput_ = enabled;
        settings_.setValue("emulation/keyboard_text_input", enabled);
    });

    auto *battery = emulation->addMenu("Battery");
    batteryLevel_ = battery->addAction("Set percentage...", this, [this] {
        bool accepted;
        const int value = QInputDialog::getInt(this, "Battery", "Battery charge (0-100%):",
                                               batteryPercent_, 0, 100, 1, &accepted);
        if (!accepted) return;
        batteryPercent_ = value;
        battery_set(&machine_.battery, value, charging_);
        settings_.setValue("battery/percent", value);
    });
    batteryCable_ = checkAction(battery, "Charging cable connected", charging_);
    connect(batteryCable_, &QAction::triggered, this, [this](bool connected) {
        charging_ = connected;
        battery_set(&machine_.battery, batteryPercent_, connected);
        settings_.setValue("battery/charging", connected);
    });

    auto *cell = emulation->addMenu("Cellular");
    cellAction_ = checkAction(cell, "Cellular enabled", cellEnabled_);
    connect(cellAction_, &QAction::triggered, this, [this](bool enabled) { applyCellular(enabled, networkName_); });
    smsAction_ = cell->addAction("Send SMS to phone...", this, &PhoneWindow::sendSms);
    callAction_ = cell->addAction("Call phone...", this, &PhoneWindow::callPhone);
    networkAction_ = cell->addAction("Network name...", this, &PhoneWindow::editNetwork);
    auto *gsm = cell->addMenu("GSM");
    auto *gsmGroup = new QActionGroup(gsm);
    const char *modes[] = {"Off", "Internet", "Internet + built-in WAP gateway"};
    for (unsigned mode = 0; mode < 3; ++mode) {
        gsmActions_[mode] = checkAction(gsm, modes[mode], gsmMode_ == mode);
        gsmGroup->addAction(gsmActions_[mode]);
        connect(gsmActions_[mode], &QAction::triggered, this, [this, mode] { setGsmMode(mode); });
    }

    emulation->addSeparator();
    audioAction_ = checkAction(emulation, "MA-3 &audio (reset)", audioEnabled_);
    connect(audioAction_, &QAction::triggered, this, [this](bool enabled) {
        const bool previous = audioEnabled_;
        audioEnabled_ = enabled;
        if (machine_.loaded && !loadFirmware(firmware_, State::Stopped, storage_.defaults_boot != 0))
            audioEnabled_ = previous;
        audioSettings_.setValue("audio/enabled", audioEnabled_);
        updateUi();
        if (enabled && !audioError_.isEmpty()) QMessageBox::warning(this, "MA-3 audio", audioError_);
    });
}

void PhoneWindow::applyCellular(bool enabled, const QString &network)
{
    if (!cellular_set_network(&machine_.cellular, &machine_.bus, enabled, network.toUtf8().constData())) {
        QMessageBox::warning(this, "Cellular", QString::fromUtf8(machine_.cellular.error));
    } else {
        cellEnabled_ = enabled;
        networkName_ = network;
        phone_net_set_mode(&machine_.network, enabled ? gsmMode_ : 0);
        settings_.setValue("cellular/enabled", enabled);
        settings_.setValue("cellular/network", network);
    }
    updateUi();
}

void PhoneWindow::setGsmMode(unsigned mode)
{
    gsmMode_ = mode;
    machine_.cellular.mode = mode;
    phone_net_set_mode(&machine_.network, cellEnabled_ ? mode : 0);
    settings_.setValue("cellular/gsm_mode", mode);
    if (mode == 2) QMessageBox::information(this, "Set up the built-in WAP gateway",
        "In the phone's WAP browser profile settings, enter:\n\n"
        "Gateway IP address: 10.0.0.1\nSecurity: Non-secure\nPort (if shown): 9201\n\n"
        "On the C100, use # to enter dots in the IP address.\n\n"
        "Test homepage: http://sghemu.local/\n\n"
        "Make sure Cellular is enabled, then open the phone's WAP browser.");
}

void PhoneWindow::editNetwork()
{
    bool accepted;
    const QString name = QInputDialog::getText(this, "Network name", "Network name (1-30 ASCII characters):",
                                               QLineEdit::Normal, networkName_, &accepted);
    if (accepted) applyCellular(cellEnabled_, name);
}

void PhoneWindow::callPhone()
{
    bool accepted;
    const QString number = QInputDialog::getText(this, "Call phone", "Caller number:",
                                                QLineEdit::Normal, "+79990001122", &accepted);
    if (!accepted) return;
    if (cellular_call(&machine_.cellular, &machine_.bus, number.toUtf8().constData()))
        setState(State::Running);
    else QMessageBox::warning(this, "Call phone", QString::fromUtf8(machine_.cellular.error));
}

void PhoneWindow::sendSms()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Send SMS to phone");
    QFormLayout form(&dialog);
    QLineEdit number("+79990001122");
    QPlainTextEdit message;
    QComboBox level;
    level.addItems({"Normal (no class)", "Class 0 - Flash", "Class 1 - Phone", "Class 2 - SIM", "Class 3 - Terminal"});
    form.addRow("Sender number:", &number);
    form.addRow("Message (up to 70 characters):", &message);
    form.addRow("SMS level:", &level);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons.button(QDialogButtonBox::Ok)->setText("Send SMS");
    form.addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const QString text = message.toPlainText();
        const QDateTime now = QDateTime::currentDateTime();
        const int offset = now.offsetFromUtc() / (15 * 60);
        const int parts[] = {now.date().year() % 100, now.date().month(), now.date().day(),
                             now.time().hour(), now.time().minute(), now.time().second(), qAbs(offset)};
        uint8_t timestamp[7];
        for (unsigned i = 0; i < 7; ++i) timestamp[i] = (parts[i] % 10) * 16 + parts[i] / 10;
        if (offset < 0) timestamp[6] |= 8;
        if (text.isEmpty() || text.size() > 70) {
            QMessageBox::information(&dialog, "Send SMS", "Enter a message of 1 to 70 characters.");
            return;
        }
        if (!cellular_sms_class(&machine_.cellular, number.text().toUtf8().constData(), text.utf16(),
                                text.size(), timestamp, level.currentIndex() - 1)) {
            QMessageBox::warning(&dialog, "Send SMS", QString::fromUtf8(machine_.cellular.error));
            return;
        }
        setState(State::Running);
        dialog.accept();
    });
    dialog.exec();
}

void PhoneWindow::updateCall()
{
    const Cellular &cell = machine_.cellular;
    if (callRevision_ == cell.revision) return;
    callRevision_ = cell.revision;
    if (!cell.outgoing || !cell.call_state) {
        if (callDialog_) callDialog_->hide();
        return;
    }
    if (!callDialog_) {
        callDialog_ = new QDialog(this);
        callDialog_->setWindowTitle("Incoming call from phone");
        auto *layout = new QVBoxLayout(callDialog_);
        callLabel_ = new QLabel(callDialog_);
        layout->addWidget(callLabel_);
        auto *buttons = new QDialogButtonBox(callDialog_);
        acceptCall_ = buttons->addButton("Accept", QDialogButtonBox::ActionRole);
        declineCall_ = buttons->addButton("Decline", QDialogButtonBox::ActionRole);
        layout->addWidget(buttons);
        connect(acceptCall_, &QPushButton::clicked, this, [this] { answerCall(true); });
        connect(declineCall_, &QPushButton::clicked, this, [this] { answerCall(false); });
        connect(callDialog_, &QDialog::rejected, this, [this] { answerCall(false); });
    }
    const bool connected = cell.call_state == CELL_CONNECTED;
    callLabel_->setText(QString("%1\nDialed number: %2")
        .arg(connected ? "Call connected" : "Incoming call from the emulated phone", cell.number));
    acceptCall_->setText(connected ? "Hang up" : "Accept");
    declineCall_->setVisible(!connected);
    callDialog_->show();
}

void PhoneWindow::answerCall(bool accept)
{
    if (cellular_answer(&machine_.cellular, accept)) {
        setState(State::Running);
        updateCall();
    } else {
        QMessageBox::warning(this, "Call", QString::fromUtf8(machine_.cellular.error));
        if (machine_.cellular.call_state) callDialog_->show();
    }
}

void PhoneWindow::showDiagnostics()
{
    if (!diagnostics_) {
        diagnostics_ = new QDialog(this);
        diagnostics_->setWindowTitle("SGHemu - diagnostics");
        diagnostics_->resize(740, 460);
        auto *layout = new QVBoxLayout(diagnostics_);
        diagnosticsText_ = new QPlainTextEdit(diagnostics_);
        diagnosticsText_->setReadOnly(true);
        diagnosticsText_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        diagnosticsText_->setLineWrapMode(QPlainTextEdit::NoWrap);
        layout->addWidget(diagnosticsText_);
    }
    diagnostics_->show();
    diagnostics_->raise();
    diagnostics_->activateWindow();
    updateDiagnostics();
}

static QString hex(uint32_t value, int width = 8)
{
    return QString("%1").arg(value, width, 16, QLatin1Char('0')).toUpper();
}

void PhoneWindow::updateDiagnostics()
{
    if (!diagnostics_ || !diagnostics_->isVisible() || diagnosticsText_->textCursor().hasSelection()) return;
    const Machine &m = machine_;
    QString text;
    QTextStream out(&text);
    out << "Model: " << m.name << "\nConfig: " << QFile::decodeName(m.config_path)
        << "\nState: " << statusBar()->currentMessage() << '\n'
        << "Instructions: " << m.cpu.instructions << "   scheduler ticks: " << m.cpu.cycles << '\n'
        << "CPSR: " << hex(m.cpu.cpsr) << "   Unknown / ROM faults: " << m.bus.faults << '\n'
        << "Last access: " << (m.bus.last_write ? 'W' : 'R') << m.bus.last_width * 8
        << " at " << hex(m.bus.last_address) << ", value " << hex(m.bus.last_value)
        << ", instruction PC " << hex(m.bus.pc) << '\n'
        << "Keypad bits: " << hex(m.keys) << " (guest active-low matrix)\n"
        << "ROM images: " << m.rom_count;
    if (m.rom_count > 1) out << " (second mapped at " << hex(m.roms[1].base) << ')';
    out << "\nUnplaced image: " << (*m.unplaced_path ? QFile::decodeName(m.unplaced_path) : "none")
        << " (" << m.unplaced_size << " bytes)\n"
        << "LCD: " << (m.lcd.attached ? "attached" : "not mapped") << ", display " << (m.lcd.enabled ? "on" : "off")
        << ", commands " << m.lcd.commands << ", pixel writes " << m.lcd.writes << '\n'
        << "CPU error: " << (m.cpu.error ? m.cpu.error : "none") << "\n\n";
    for (unsigned i = 0; i < 16; ++i)
        out << 'R' << i << " = " << hex(m.cpu.r[i]) << (i % 4 == 3 ? "\n" : "    ");
    for (unsigned i = 0; i < m.flash_count; ++i) {
        const Flash &flash = m.flashes[i];
        out << "Flash " << hex(flash.base) << ": " << flash.programs << " programs, " << flash.erases
            << " erases; last " << hex(flash.last_program) << " / " << hex(flash.last_erase) << '\n';
    }
    if (m.clock.attached)
        out << "PLL: ARM " << m.clock.arm_hz << " Hz / DSP " << m.clock.dsp_hz
            << " Hz; core cycles " << m.cpu.clock_cycles << "; time " << m.clock.nanoseconds / 1e9 << " s"
            << (m.clock.unknown ? " (unrecognized divider)" : "") << '\n';
    out << "\nMA-3 audio: " << (audio_output_live(m.ma3.output) ? "native PCM" : "muted")
        << "; FIFO bytes " << m.ma3.fifo_bytes << ", IRQs " << m.ma3.irq_events
        << ", FIFO clears " << m.ma3.fifo_clears << ", partial packets dropped " << m.ma3.partial_drops << '\n';
    if (*m.ma3.output_error) out << "Audio error: " << m.ma3.output_error << '\n';
    else if (!audioError_.isEmpty()) out << "Audio error: " << audioError_ << '\n';
    if (*m.ma3.error) out << "MA-3 error: " << m.ma3.error << '\n';
    if (m.ma3.output) {
        const Ma3Stats stats = ma3_stats(&m.ma3);
        out << "PCM frames " << stats.frames << ", nonzero samples " << stats.nonzero
            << ", peak " << stats.peak << ", host underruns " << stats.underruns << '\n';
    }
    out << "CPU tones: " << m.phone_io.frames << " PCM frames, " << m.phone_io.tone_changes << " changes\n"
        << "\nSIM: " << (!m.sim.attached ? "not mapped" : m.sim.present ? "synthetic card inserted" : "removed")
        << "; resets " << m.sim.resets << ", APDUs " << m.sim.card.commands << ", RX " << m.sim.received
        << ", TX " << m.sim.transmitted << ", DMA bytes " << m.sim.dma_bytes << '\n';
    if (m.cellular.ready) {
        out << "Cellular: " << (m.cellular.enabled ? "enabled" : "off") << "; network " << m.cellular.network
            << "; call state " << m.cellular.call_state << "; SMS received " << m.cellular.received_sms
            << " / sent " << m.cellular.sent_sms << "; last recipient " << m.cellular.last_sms_number << '\n';
        out << "Packet data: mode " << m.network.mode << "; UDP TX " << m.network.transmitted << " / RX "
            << m.network.received << "; " << (*m.network.error ? m.network.error : "no transport error") << '\n';
    }
    if (m.network.java.first)
        out << "Java TCP: DNS " << m.network.java.lookups << "; connects " << m.network.java.connects
            << "; TX " << m.network.java.sent << " / RX " << m.network.java.received << " bytes; "
            << (*m.network.java.error ? m.network.java.error : "no DNS error") << '\n';
    out << "\nFile > Logging writes build/gui-unmapped.log, gui-ma3-traffic.log and gui-profile.log.\n"
        << "Diagnostic continuation returns zero on unknown reads and drops writes.\n"
        << "Clock-enabled profiles use core-cycle time; external bus waits remain uncalibrated.";
    out.flush();
    if (text == diagnosticsText_->toPlainText()) return;
    auto *scroll = diagnosticsText_->verticalScrollBar();
    const int top = scroll->value();
    diagnosticsText_->setPlainText(text);
    scroll->setValue(top);
}

bool PhoneWindow::applyLogging(bool enabled)
{
    FILE *bus = nullptr, *traffic = nullptr, *profile = nullptr;
    if (enabled) {
        QDir().mkpath("build");
        bus = fopen("build/gui-unmapped.log", "a");
        traffic = fopen("build/gui-ma3-traffic.log", "a");
        profile = fopen("build/gui-profile.log", "a");
        if (!bus || !traffic || !profile) {
            if (bus) fclose(bus);
            if (traffic) fclose(traffic);
            if (profile) fclose(profile);
            loggingAction_->setChecked(busLog_ != nullptr);
            return false;
        }
        fputs("# GUI logging enabled\n", bus);
        fputs("# GUI logging enabled\n", traffic);
        fprintf(profile, "# SGHemu Qt profile: model=%s, PC sampled once per execution batch\n", machine_.name);
    }
    if (busLog_) fclose(busLog_);
    if (machine_.ma3.trace) fclose(machine_.ma3.trace);
    if (profileLog_) fclose(profileLog_);
    machine_.bus.log = machine_.network.trace = busLog_ = bus;
    machine_.ma3.trace = traffic;
    profileLog_ = profile;
    profileWall_ = wallClock_.elapsed();
    profileInstructions_ = machine_.cpu.instructions;
    profileGuest_ = machine_.clock.nanoseconds;
    profilePcs_.clear();
    loggingAction_->setChecked(enabled);
    return true;
}

void PhoneWindow::logProgress()
{
    if (!profileLog_) return;
    const qint64 now = wallClock_.elapsed();
    if (now - profileWall_ < 1000) return;
    const double elapsed = (now - profileWall_) / 1000.0;
    const uint64_t instructions = machine_.cpu.instructions - profileInstructions_;
    const double guest = (machine_.clock.nanoseconds - profileGuest_) / 1e9;
    fprintf(profileLog_, "wall_s=%.3f running=%d instructions=%llu mips=%.3f emu_s=%.3f speed_pct=%.1f tx=%llu rx=%llu\n",
            elapsed, runTimer_.isActive(), static_cast<unsigned long long>(instructions), instructions / elapsed / 1e6,
            guest, guest * 100.0 / elapsed, static_cast<unsigned long long>(machine_.network.transmitted),
            static_cast<unsigned long long>(machine_.network.received));
    fputs("guest_pc_samples", profileLog_);
    for (int i = 0; i < 8 && !profilePcs_.isEmpty(); ++i) {
        auto best = profilePcs_.begin();
        for (auto it = best; it != profilePcs_.end(); ++it)
            if (it.value() > best.value()) best = it;
        fprintf(profileLog_, " %08x:%u", best.key(), best.value());
        profilePcs_.erase(best);
    }
    fputc('\n', profileLog_);
    profilePcs_.clear();
    profileWall_ = now;
    profileInstructions_ = machine_.cpu.instructions;
    profileGuest_ = machine_.clock.nanoseconds;
    fflush(profileLog_);
    fflush(busLog_);
    fflush(machine_.ma3.trace);
}

void PhoneWindow::logStop(const char *reason)
{
    if (!busLog_) return;
    const Arm7 &cpu = machine_.cpu;
    for (FILE *log : {busLog_, profileLog_}) {
        fprintf(log, "# STOP pc=%08x last_pc=%08x opcode=%08x cpsr=%08x reason=%s\n",
                cpu.r[15], cpu.last_pc, cpu.last_opcode, cpu.cpsr, reason);
        for (unsigned i = 0; i < 16; ++i) fprintf(log, "r%u=%08x%s", i, cpu.r[i], i % 4 == 3 ? "\n" : " ");
        fflush(log);
    }
}
