#ifndef SGH_GUI_QT_H
#define SGH_GUI_QT_H

extern "C" {
#include "content.h"
#include "emu_speed.h"
#include "key_text.h"
#include "nvram.h"
#include "userfs.h"
}

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QSettings>
#include <QTimer>
#include <QVector>

class QAction;
class QDialog;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class LcdView;

struct Firmware {
    QByteArray model, firstRom, secondRom;
};

class PhoneWindow final : public QMainWindow {
  public:
    PhoneWindow();
    ~PhoneWindow() override;

  protected:
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

  private:
    enum class State { Empty, Running, Paused, Stopped, Standby };
    enum class ImportStatus { Queued, Starting, Storing, Imported, Failed };

    struct ImportItem {
        QString path, name, error;
        uint32_t size, type, registryId = 0;
        uint64_t accepted = 0;
        ImportStatus status = ImportStatus::Queued;
    };

    void openFirmwareFolder();
    void openModel();
    void openRom(bool second);
    void openSnapshot();
    void openRecentFirmware();
    bool loadFirmware(Firmware firmware, State next, bool defaults = false, const QByteArray &snapshot = {});
    bool saveStorage();
    void setState(State state, const QString &detail = {});
    void updateUi();
    void runSlice();
    bool emulate(unsigned steps, State afterReset);
    void enterStandby();
    void advanceStandby(bool powerPressed = false);
    void refresh();
    void updateKey(unsigned key);
    void releaseKeys(uint32_t keep = 0);
    void updateTextKeys();
    void fitLcd();
    void openAudio();
    void syncClock();

    void addToolMenus(QMenu *file, QMenu *emulation);
    void showDiagnostics();
    void updateDiagnostics();
    bool applyLogging(bool enabled);
    void logProgress();
    void logStop(const char *reason);
    void applyCellular(bool enabled, const QString &network);
    void setGsmMode(unsigned mode);
    void editNetwork();
    void callPhone();
    void sendSms();
    void updateCall();
    void answerCall(bool accept);

    void showFilesystem();
    void refreshFilesystem();
    void refreshImportQueue();
    void addImportFiles(const QStringList &paths);
    void startNextImport();
    void openJavaGames();
    void importAccepted();
    void importFailed(const QString &reason);
    void checkImport();
    void exportFilesystem(bool backup);

    Machine machine_{};
    NvramStore storage_{};
    Firmware firmware_;
    QByteArray simStorage_;
    QSettings settings_, audioSettings_;
    State state_ = State::Empty;
    QString stateDetail_, audioError_;
    QTimer runTimer_, uiTimer_;
    QElapsedTimer wallClock_, audioClock_;
    uint64_t audioGuestStart_ = 0;
    qint64 lastSave_ = 0, lastClockSync_ = 0, offSinceNs_ = 0;
    EmuSpeed speed_{};
    LcdView *lcd_;
    QLabel *speedLabel_, *ledIndicator_;
    QAction *run_, *pause_, *stop_, *step_, *snapshot_;
    QAction *continuationAction_, *simAction_, *priorityAction_, *clockAction_, *javaAction_;
    QAction *batteryLevel_, *batteryCable_, *cellAction_, *networkAction_, *smsAction_, *callAction_;
    QAction *gsmActions_[3]{}, *audioAction_, *patternAction_;
    QPushButton *buttons_[SGH_MAX_KEYS]{};
    QHash<quint32, unsigned> keyboardKeys_;
    uint32_t mouseKeys_ = 0, textHeld_ = 0;
    KeyText textKeys_{};
    bool continuation_ = false, textInput_ = false, highPriority_ = false;
    bool hostTime_ = true, javaExtended_ = false;
    int simOverride_ = -1, batteryPercent_ = 100, zoom_ = 2;
    bool charging_ = false, ghosting_ = false, detune_ = false, water_ = false;
    bool cellEnabled_ = true;
    QString networkName_;
    unsigned gsmMode_ = 0, callRevision_ = ~0u, led_ = ~0u;
    QDialog *callDialog_ = nullptr;
    QLabel *callLabel_ = nullptr;
    QPushButton *acceptCall_ = nullptr, *declineCall_ = nullptr;
    bool audioEnabled_ = true;
    unsigned audioTicks_ = MA3_DEFAULT_TICKS_HZ;

    QDialog *diagnostics_ = nullptr;
    QPlainTextEdit *diagnosticsText_ = nullptr;
    QAction *loggingAction_;
    FILE *busLog_ = nullptr, *profileLog_ = nullptr;
    qint64 profileWall_ = 0;
    uint64_t profileInstructions_ = 0, profileGuest_ = 0;
    QHash<uint32_t, unsigned> profilePcs_;

    QDialog *filesystem_ = nullptr;
    QTableWidget *phoneFiles_ = nullptr, *importList_ = nullptr;
    QLabel *fsStatus_ = nullptr;
    QPushButton *removeFiles_, *startQueue_, *stopQueue_, *javaGames_, *exportList_, *backupNvm_;
    QVector<ImportItem> imports_;
    ContentImport content_{};
    int importCurrent_ = -1;
    uint64_t importStartNs_ = 0, importStartInstructions_ = 0;
    bool importRunning_ = false, importDiscard_ = false, showSystem_ = false;
    QString importMessage_;
};

#endif
