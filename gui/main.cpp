// nocry GUI, first slice: live board state, "read device", log and progress from nocry-core.
#include <QApplication>
#include <QCheckBox>
#include <QButtonGroup>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QTabWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QRadioButton>
#include <QDate>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStorageInfo>
#include <functional>
#include <QDir>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QRegularExpression>
#include <QSettings>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QProcess>
#include <QGridLayout>
#include <QFile>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include "nocry.h"

static QJsonObject takeJson(char *s)
{
    QJsonObject o = QJsonDocument::fromJson(QByteArray(s)).object();
    nocry_string_free(s);
    return o;
}

// A push button with a bold title and a wrapping description underneath. Unlike
// QCommandLinkButton it reports the height its text really needs at the current width.
class ActionButton : public QPushButton
{
public:
    ActionButton(const QString &title, const QString &description)
    {
        auto *l = new QVBoxLayout(this);
        l->setContentsMargins(14, 10, 14, 10);
        l->setSpacing(2);
        auto *t = new QLabel(title);
        QFont f = t->font();
        f.setBold(true);
        t->setFont(f);
        auto *d = new QLabel(description);
        d->setWordWrap(true);
        for (QLabel *x : {t, d}) {
            x->setAttribute(Qt::WA_TransparentForMouseEvents);
            l->addWidget(x);
        }
        setAccessibleName(title);
        setAccessibleDescription(description);
        QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Minimum);
        sp.setHeightForWidth(true);
        setSizePolicy(sp);
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return layout()->heightForWidth(w); }
    QSize sizeHint() const override { return layout()->sizeHint(); }
    QSize minimumSizeHint() const override { return layout()->minimumSize(); }
};

class Window : public QMainWindow
{
public:
    Window()
    {
        setWindowTitle(tr("nocry"));
        auto *central = new QWidget;
        auto *v = new QVBoxLayout(central);

        stateTitle = new QLabel;
        QFont f = stateTitle->font();
        f.setPointSizeF(f.pointSizeF() * 1.5);
        f.setBold(true);
        stateTitle->setFont(f);
        stateHint = new QLabel;
        stateHint->setWordWrap(true);
#ifdef Q_OS_LINUX
        // first run on Linux: a normal user cannot open the board's USB devices without a udev rule
        udevBanner = new QWidget;
        {
            auto *h = new QHBoxLayout(udevBanner);
            h->setContentsMargins(0, 0, 0, 0);
            auto *l = new QLabel(tr("<b>One-time setup needed.</b> Linux does not let programs talk to the board until a small system rule is installed."));
            l->setWordWrap(true);
            auto *b = new QPushButton(tr("Install rule…"));
            h->addWidget(l, 1);
            h->addWidget(b);
            connect(b, &QPushButton::clicked, this, [this] { installUdevRule(); });
        }
        v->addWidget(udevBanner);
        udevBanner->setVisible(!udevRuleInstalled());
#endif
#ifdef Q_OS_WIN
        // first run on Windows: the board's USB devices need Windows' WinUSB driver assigned to them
        udevBanner = new QWidget;
        {
            auto *h = new QHBoxLayout(udevBanner);
            h->setContentsMargins(0, 0, 0, 0);
            auto *l = new QLabel(tr("<b>One-time setup needed.</b> Windows cannot talk to the board until a USB driver is set up for it."));
            l->setWordWrap(true);
            auto *b = new QPushButton(tr("Set up driver…"));
            h->addWidget(l, 1);
            h->addWidget(b);
            connect(b, &QPushButton::clicked, this, [this] { installWindowsDriver(); });
        }
        v->addWidget(udevBanner);
        udevBanner->setVisible(false);
        checkWindowsDriver();
#endif
        v->addWidget(stateTitle);
        auto *hintRow = new QHBoxLayout;
        hintRow->addWidget(stateHint, 1);
        bootHelpButton = new QPushButton(tr("How to enter USB boot mode…"));
        hintRow->addWidget(bootHelpButton, 0, Qt::AlignTop);
        netButton = new QPushButton(tr("Connect over the network…"));
        netButton->setToolTip(tr("For a board that shows the nocry rescue screen: work on it over Wi-Fi, without a USB cable."));
        hintRow->addWidget(netButton, 0, Qt::AlignTop);
        wifiButton = new QPushButton(tr("Board's Wi-Fi…"));
        wifiButton->setToolTip(tr("For a board running the nocry rescue system: see which Wi-Fi network it is on, or make it join another."));
        wifiButton->setVisible(false);
        hintRow->addWidget(wifiButton, 0, Qt::AlignTop);
        connect(wifiButton, &QPushButton::clicked, this, [this] { startWifi(); });
        connect(netButton, &QPushButton::clicked, this, [this] {
            if (networkAddress.isEmpty())
                chooseNetwork();
            else
                disconnectNetwork();
        });
        v->addLayout(hintRow);
        connect(bootHelpButton, &QPushButton::clicked, this, [this] { showBootHelp(); });
        // shell access to the running recovery: what works right now, and how to use it
        shellLine = new QLabel;
        shellLine->setTextInteractionFlags(Qt::TextSelectableByMouse);
        shellButton = new QPushButton(tr("How to get a shell…"));
        auto *shellRow = new QHBoxLayout;
        shellRow->addWidget(shellLine, 1);
        shellRow->addWidget(shellButton);
        v->addLayout(shellRow);
        shellLine->setVisible(false);
        shellButton->setVisible(false);
        connect(shellButton, &QPushButton::clicked, this, [this] { showShellHelp(); });

        auto *tabs = new QTabWidget;
        v->addWidget(tabs, 1);

        // --- Home: what is connected, and what can be done with it
        auto *home = new QWidget;
        auto *hv = new QVBoxLayout(home);
        auto *box = new QGroupBox;      // untitled: the frame alone groups the details
        auto *boxRow = new QHBoxLayout(box);
        auto *form = new QFormLayout;
        boxRow->addLayout(form, 1);
        for (auto *l : {&model, &serial, &mac, &board, &slot, &otherSlot, &storage}) {
            *l = new QLabel(QStringLiteral("—"));
            (*l)->setTextInteractionFlags(Qt::TextSelectableByMouse);
        }
        form->addRow(tr("Model:"), model);
        form->addRow(tr("Firmware:"), slot);
        form->addRow(tr("Other slot:"), otherSlot);
        form->addRow(tr("Serial number:"), serial);
        form->addRow(tr("MAC address:"), mac);
        form->addRow(tr("Mainboard:"), board);
        form->addRow(tr("Storage:"), storage);
        readButton = new QPushButton(tr("Refresh"));
        readButton->setToolTip(tr("Read the board's details again"));
        boxRow->addWidget(readButton, 0, Qt::AlignTop | Qt::AlignRight);
        hv->addWidget(box);

        backupButton = new ActionButton(tr("Back up…"), tr("Save everything on the board to a file."));
        restoreButton = new ActionButton(tr("Restore backup…"), tr("Put a saved backup back onto the board."));
        installButton = new ActionButton(tr("Install firmware…"), tr("Repair, update or reinstall the printer's software."));
        switchButton = new ActionButton(tr("Switch Active Slot…"), tr("Choose which installed firmware starts, or copy one slot to the other."));
        resetButton = new ActionButton(tr("Factory reset…"), tr("Erase settings and files; keep the firmware."));
        auto *grid = new QGridLayout;
        grid->addWidget(backupButton, 0, 0);
        grid->addWidget(restoreButton, 0, 1);
        grid->addWidget(installButton, 1, 0);
        grid->addWidget(resetButton, 1, 1);
        grid->addWidget(switchButton, 2, 0);
        hv->addLayout(grid);
        hv->addStretch(1);
        tabs->addTab(home, tr("Home"));
        tabWidget = tabs;

        // --- Partitions (reference)
        parts = new QTableWidget(0, 3);
        parts->setHorizontalHeaderLabels({tr("Partition"), tr("Offset"), tr("Size")});
        parts->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        parts->verticalHeader()->hide();
        parts->setEditTriggers(QAbstractItemView::NoEditTriggers);
        parts->setSelectionBehavior(QAbstractItemView::SelectRows);
        parts->setSelectionMode(QAbstractItemView::SingleSelection);
        auto *adv = new QWidget;
        auto *av = new QVBoxLayout(adv);
        auto *advNote = new QLabel(tr("For people who know what these do. Writing the wrong thing here can stop the printer from starting; "
                                      "nocry can always rebuild the board from USB boot mode, but settings and files would be lost."));
        advNote->setWordWrap(true);
        av->addWidget(advNote);
        av->addWidget(parts, 1);
        partBackupButton = new QPushButton(tr("Back up partition…"));
        partRestoreButton = new QPushButton(tr("Restore partition…"));
        partEraseButton = new QPushButton(tr("Erase partition…"));
        identityButton = new QPushButton(tr("Edit serial number and MAC…"));
        eraseAllButton = new QPushButton(tr("Erase entire storage…"));
        loaderButton = new QPushButton(tr("Boot loader…"));
        loaderButton->setToolTip(tr("See which first-stage boot loader the board has; install the open nocryboot loader or go back to the factory one"));
        auto *prow = new QHBoxLayout;
        prow->addWidget(partBackupButton);
        prow->addWidget(partRestoreButton);
        prow->addWidget(partEraseButton);
        prow->addStretch(1);
        av->addLayout(prow);
        auto *irow = new QHBoxLayout;
        irow->addWidget(identityButton);
        irow->addWidget(eraseAllButton);
        irow->addWidget(loaderButton);
        pictureButton = new QPushButton(tr("Boot picture…"));
        pictureButton->setToolTip(tr("Choose the picture shown for the first seconds after power-on: nocry's or the firmware's own"));
        irow->addWidget(pictureButton);
        irow->addStretch(1);
        av->addLayout(irow);
        tabs->addTab(adv, tr("Advanced"));
        connect(parts, &QTableWidget::itemSelectionChanged, this, [this] { updateButtons(); });
        connect(partBackupButton, &QPushButton::clicked, this, [this] { choosePartBackup(); });
        connect(partRestoreButton, &QPushButton::clicked, this, [this] { choosePartRestore(); });
        connect(partEraseButton, &QPushButton::clicked, this, [this] { choosePartErase(); });
        connect(identityButton, &QPushButton::clicked, this, [this] { chooseIdentity(); });
        connect(eraseAllButton, &QPushButton::clicked, this, [this] { chooseEraseAll(); });
        connect(loaderButton, &QPushButton::clicked, this, [this] { startLoader(0, {}); });
        connect(pictureButton, &QPushButton::clicked, this, [this] { startPicture(-1); });

        // --- Activity log
        log = new QPlainTextEdit;
        log->setReadOnly(true);
        log->setMaximumBlockCount(2000);
        auto *logTab = new QWidget;
        auto *lv = new QVBoxLayout(logTab);
        lv->addWidget(log, 1);
        boardLogButton = new QPushButton(tr("Show the board's own log…"));
        boardLogButton->setToolTip(tr("The kernel log of the recovery system running on the board, for diagnosing problems"));
        lv->addWidget(boardLogButton, 0, Qt::AlignLeft);
        tabs->addTab(logTab, tr("Activity log"));
        connect(boardLogButton, &QPushButton::clicked, this, [this] { showBoardLog(); });

        // --- progress, always visible below the tabs
        progressLabel = new QLabel;
        progress = new QProgressBar;
        progress->setVisible(false);
        cancelButton = new QPushButton(tr("Cancel"));
        cancelButton->setVisible(false);
        v->addWidget(progressLabel);
        auto *pr = new QHBoxLayout;
        pr->addWidget(progress, 1);
        pr->addWidget(cancelButton);
        v->addLayout(pr);
        setCentralWidget(central);
        // no taller than the content needs: extra height would only be empty space on Home
        resize(720, sizeHint().height());

        nocry_set_event_callback(&Window::onEvent, this);
        connect(readButton, &QPushButton::clicked, this, [this] { readDevice(); });
        connect(backupButton, &QPushButton::clicked, this, [this] { chooseBackup(); });
        connect(installButton, &QPushButton::clicked, this, [this] { chooseInstall(); });
        connect(restoreButton, &QPushButton::clicked, this, [this] {
            const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
            const QString f = QFileDialog::getOpenFileName(this, tr("Choose a backup"), dir, tr("Disk images (*.img);;All files (*)"));
            if (!f.isEmpty())
                startRestore(f, false, false);
        });
        connect(resetButton, &QPushButton::clicked, this, [this] { chooseReset(); });
        connect(switchButton, &QPushButton::clicked, this, [this] { startSlots(QString()); });
        connect(cancelButton, &QPushButton::clicked, this, [this] { requestCancel(); });
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { pollState(); });
        timer->start(1000);
        pollState();
    }

    void readDevice()
    {
        if (busy)
            return;
        setBusy(true);
        auto *w = new QFutureWatcher<QJsonObject>(this);
        connect(w, &QFutureWatcher<QJsonObject>::finished, this, [this, w] {
            showInfo(w->result());
            w->deleteLater();
            setBusy(false);
            pollState();
            if (onIdle)
                onIdle();
        });
        w->setFuture(QtConcurrent::run([] { return takeJson(nocry_device_info_json()); }));
    }

    // Full backup of the board to `path`; the board is only read.
    void startBackup(const QString &path)
    {
        if (busy)
            return;
        setBusy(true);
        cancelButton->setEnabled(true);
        cancelButton->setVisible(true);
        log->appendPlainText(tr("Backing up to %1").arg(QDir::toNativeSeparators(path)));
        auto *w = new QFutureWatcher<QJsonObject>(this);
        connect(w, &QFutureWatcher<QJsonObject>::finished, this, [this, w, path] {
            const QJsonObject o = w->result();
            w->deleteLater();
            cancelButton->setVisible(false);
            handleEvent(NOCRY_EVENT_PROGRESS_END, {}, 0, 0, {});
            setBusy(false);
            pollState();
            lastResult = o;
            if (o.value("ok").toBool()) {
                const QString msg = tr("The backup is complete and verified.\n\nFile: %1\nSize: %2\nTime: %3 min %4 s\nSHA-256: %5")
                    .arg(QDir::toNativeSeparators(path), QLocale().formattedDataSize(qint64(o.value("bytes").toDouble())))
                    .arg(int(o.value("seconds").toDouble()) / 60).arg(int(o.value("seconds").toDouble()) % 60)
                    .arg(o.value("sha256").toString());
                log->appendPlainText(msg);
                if (!quiet)
                    QMessageBox::information(this, tr("Backup complete"), msg);
            } else if (o.value("cancelled").toBool()) {
                log->appendPlainText(tr("Backup cancelled: %1").arg(o.value("error").toString()));
            } else {
                log->appendPlainText(tr("Backup failed: %1").arg(o.value("error").toString()));
                if (!quiet)
                    QMessageBox::critical(this, tr("Backup failed"), o.value("error").toString());
            }
            if (onIdle)
                onIdle();
        });
        const QByteArray p = path.toUtf8();
        w->setFuture(QtConcurrent::run([p] { return takeJson(nocry_backup_json(p.constData(), 1, 1)); }));
    }

    // Download a firmware from the list (verified, reused if already there), then go on to
    // the install check.
    void startDownload(const QJsonObject &fw, int keepData)   // keepData: 0 clean, 1 keep data, 2 other slot
    {
        if (busy)
            return;
        setBusy(true);
        stateHint->setText(tr("Downloading the firmware."));
        cancelButton->setEnabled(true);
        cancelButton->setVisible(true);
        const QByteArray url = fw.value("url").toString().toUtf8(), file = fw.value("file").toString().toUtf8(), dir = dataDir();
        const quint64 size = quint64(fw.value("size").toDouble());
        auto *w = new QFutureWatcher<QJsonObject>(this);
        connect(w, &QFutureWatcher<QJsonObject>::finished, this, [this, w, keepData] {
            const QJsonObject o = w->result();
            w->deleteLater();
            cancelButton->setVisible(false);
            handleEvent(NOCRY_EVENT_PROGRESS_END, {}, 0, 0, {});
            setBusy(false);
            pollState();
            lastResult = o;
            if (o.value("ok").toBool()) {
                startInstall(o.value("path").toString(), keepData, false);
                return;
            }
            if (o.value("cancelled").toBool()) {
                log->appendPlainText(tr("Download cancelled."));
            } else {
                log->appendPlainText(tr("Download failed: %1").arg(o.value("error").toString()));
                if (!quiet)
                    QMessageBox::critical(this, tr("Download failed"), o.value("error").toString());
            }
            if (onIdle)
                onIdle();
        });
        w->setFuture(QtConcurrent::run([url, file, size, dir] {
            return takeJson(nocry_download_firmware_json(url.constData(), file.constData(), size, dir.constData()));
        }));
    }

    // Step 1: work out what installing `os` would change. Step 2 (after the user confirms, or
    // autoConfirm in self-tests): write it.
    void startInstall(const QString &os, int keepData, bool execute)
    {
        if (busy)
            return;
        setBusy(true);
        if (execute)
            stateHint->setText(tr("Installing. Do not unplug the board."));
        else
            log->appendPlainText(tr("Checking %1").arg(QDir::toNativeSeparators(os)));
        if (!execute)
            allowCancel();      // only reading and comparing so far
        else if (keepData == 2)
            allowCancel(tr("The slot being installed is left incomplete and cannot be started until the installation is run again. "
                           "The board keeps starting the firmware it starts now."));
        else
            allowCancel(tr("The firmware on the board is being replaced and is incomplete right now. If you stop, the board will not start "
                           "until the installation is run again to the end.\n\nnocry can always do that: USB boot mode does not depend on what is installed."));
        const QByteArray p = os.toUtf8();
        const QByteArray safety = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("safety").toUtf8();
        auto *w = new QFutureWatcher<QJsonObject>(this);
        connect(w, &QFutureWatcher<QJsonObject>::finished, this, [this, w, os, keepData, execute] {
            const QJsonObject o = w->result();
            w->deleteLater();
            handleEvent(NOCRY_EVENT_PROGRESS_END, {}, 0, 0, {});
            setBusy(false);
            pollState();
            lastResult = o;
            // firmware for several printer types on a board that does not say which it is:
            // ask, then plan again with the answer
            const QString need = o.value("need_choice").toString();
            if (o.value("ok").toBool() && !need.isEmpty()) {
                const QString pick = askBoardChoice(need, o.value("choices").toArray());
                if (!pick.isEmpty()) {
                    (need == "model" ? pendingModel : pendingBoard) = pick;
                    startInstall(os, keepData, false);
                    return;
                }
                if (onIdle)
                    onIdle();
                return;
            }
            QString fwName = o.value("firmware_name").toString();
            if (fwName.isEmpty())
                fwName = o.value("firmware_board").toString();
            const QString fw = tr("%1 firmware %2").arg(fwName, o.value("firmware_version").toString()).simplified();
            if (!o.value("ok").toBool()) {
                log->appendPlainText(wasCancelled(o) ? (execute ? tr("Installation stopped before it was finished.") : tr("Stopped.")) : tr("Install failed: %1").arg(o.value("error").toString()));
                if (!quiet && !wasCancelled(o))
                    QMessageBox::critical(this, execute ? tr("Installation failed") : tr("Cannot install this firmware"), o.value("error").toString());
            } else if (o.value("bytes_to_write").toDouble() == 0) {
                const QString msg = keepData == 2
                    ? tr("Slot %1 already holds %2, intact. Nothing needs to be written.").arg(o.value("target_slot").toString().toUpper(), fw)
                    : tr("The board already has %1 installed and intact. Nothing needs to be written.").arg(fw);
                log->appendPlainText(msg);
                if (!quiet)
                    QMessageBox::information(this, tr("Nothing to do"), msg);
            } else if (execute && keepData == 2) {
                infoStale = true;
                const QString msg = tr("%1 is installed in slot %2 and verified.\n\nThe board still starts the firmware it started before. To start the new one, use Switch Active Slot.")
                    .arg(fw, o.value("target_slot").toString().toUpper());
                log->appendPlainText(msg);
                if (!quiet)
                    QMessageBox::information(this, tr("Installation complete"), msg);
            } else if (execute) {
                infoStale = true;
                QString msg = tr("%1 is installed and verified.\n\nUnplug the board, then power it on normally. The first start can take a few minutes.").arg(fw);
                if (!o.value("safety_copy").toString().isEmpty())
                    msg += tr("\n\nA safety copy of the board's partition table and identity from before the installation was saved to:\n%1").arg(QDir::toNativeSeparators(o.value("safety_copy").toString()));
                log->appendPlainText(msg);
                if (!quiet)
                    QMessageBox::information(this, tr("Installation complete"), msg);
            } else if (confirmInstall(o, fw, keepData)) {
                startInstall(os, keepData, true);
                return;
            }
            if (onIdle)
                onIdle();
        });
        const QByteArray mdl = pendingModel.toUtf8(), brd = pendingBoard.toUtf8();
        const int picture = pendingPicture;
        w->setFuture(QtConcurrent::run([p, keepData, execute, safety, mdl, brd, picture] {
            if (keepData == 2)
                return takeJson(nocry_install_slot2_json(p.constData(), execute, safety.constData(), picture));
            return takeJson(nocry_install3_json(p.constData(), keepData, execute, safety.constData(), mdl.constData(), brd.constData(), picture));
        }));
    }

    // Read both system slots (switchTo empty), then let the user pick; or switch to "a"/"b".
    void startSlots(const QString &switchTo)
    {
        if (switchTo.startsWith("copy:")) {
            const QByteArray from = switchTo.mid(5).toUtf8(), sd = safetyDir();
            const QString F = switchTo.mid(5).toUpper(), T = F == "A" ? "B" : "A";
            runAdvanced(tr("Copying slot %1 to slot %2").arg(F, T), [from, sd] { return takeJson(nocry_copy_slot_json(from.constData(), sd.constData())); },
                [F, T](const QJsonObject &o) {
                    const QJsonObject t = o.value(T.toLower()).toObject();
                    return tr("Slot %1 now holds a verified copy of slot %2 (%3 %4).\n\nThe board still starts slot %5.")
                        .arg(T, F, t.value("firmware_name").toString(), t.value("firmware_version").toString(), o.value("active").toString().toUpper());
                }, true,
                tr("Slot %1 is left with an incomplete copy and cannot be started until the copy is run again. "
                   "If slot %1 is the one the board starts, the board will not start until then.").arg(T));
            return;
        }
        if (busy)
            return;
        setBusy(true);
        log->appendPlainText(switchTo.isEmpty() ? tr("Reading the firmware slots…") : tr("Switching to slot %1…").arg(switchTo.toUpper()));
        const QByteArray slotArg = switchTo.toUtf8();
        const QByteArray safety = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("safety").toUtf8();
        auto *w = new QFutureWatcher<QJsonObject>(this);
        connect(w, &QFutureWatcher<QJsonObject>::finished, this, [this, w, switchTo] {
            const QJsonObject o = w->result();
            w->deleteLater();
            setBusy(false);
            pollState();
            lastResult = o;
            if (!o.value("ok").toBool()) {
                log->appendPlainText(tr("Error: %1").arg(o.value("error").toString()));
                if (!quiet && !wasCancelled(o))
                    QMessageBox::critical(this, tr("Switch Active Slot"), o.value("error").toString());
            } else if (!switchTo.isEmpty()) {
                infoStale = true;
                const QString msg = tr("Slot %1 will start the next time the board is powered on.").arg(o.value("active").toString().toUpper());
                log->appendPlainText(msg);
                if (!quiet)
                    QMessageBox::information(this, tr("Switch Active Slot"), msg);
            } else {
                const QString pick = chooseSlot(o);
                if (!pick.isEmpty()) {
                    startSlots(pick);
                    return;
                }
            }
            if (onIdle)
                onIdle();
        });
        w->setFuture(QtConcurrent::run([slotArg, safety] {
            return takeJson(slotArg.isEmpty() ? nocry_slots_json() : nocry_set_active_slot_json(slotArg.constData(), safety.constData()));
        }));
    }

    // Step 1 (execute=false): quick check of the backup against this board, then confirm.
    // Step 2: make the board identical to the backup.
    void startRestore(const QString &image, bool execute, bool allowIdentity)
    {
        if (busy)
            return;
        setBusy(true);
        if (execute)
            stateHint->setText(tr("Restoring. Do not unplug the board."));
        allowCancel(execute ? tr("The board then holds a mix of what it had and what the backup has. It will probably not start until the restore "
                                 "is run again to the end.\n\nnocry can always do that: USB boot mode does not depend on what is on the board.")
                            : QString());
        log->appendPlainText(execute ? tr("Restoring %1").arg(QDir::toNativeSeparators(image)) : tr("Checking %1").arg(QDir::toNativeSeparators(image)));
        const QByteArray p = image.toUtf8();
        const QByteArray safety = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("safety").toUtf8();
        auto *w = new QFutureWatcher<QJsonObject>(this);
        connect(w, &QFutureWatcher<QJsonObject>::finished, this, [this, w, image, execute] {
            const QJsonObject o = w->result();
            w->deleteLater();
            handleEvent(NOCRY_EVENT_PROGRESS_END, {}, 0, 0, {});
            setBusy(false);
            pollState();
            lastResult = o;
            if (!o.value("ok").toBool()) {
                log->appendPlainText(wasCancelled(o) ? (execute ? tr("Restore stopped before it was finished.") : tr("Stopped.")) : tr("Restore failed: %1").arg(o.value("error").toString()));
                if (!quiet && !wasCancelled(o))
                    QMessageBox::critical(this, execute ? tr("Restore failed") : tr("Cannot restore this file"), o.value("error").toString());
            } else if (!execute) {
                const int c = confirmRestore(o, image);
                if (c) {
                    startRestore(image, true, c == 2);
                    return;
                }
            } else {
                infoStale = true;
                const QString msg = o.value("bytes_written").toDouble() == 0
                    ? tr("The board already matches this backup. Nothing needed to be written.")
                    : tr("The backup is restored and verified (%1 written).\n\nUnplug the board, then power it on normally.").arg(QLocale().formattedDataSize(qint64(o.value("bytes_written").toDouble())));
                log->appendPlainText(msg);
                if (!quiet)
                    QMessageBox::information(this, tr("Restore backup"), msg);
            }
            if (onIdle)
                onIdle();
        });
        w->setFuture(QtConcurrent::run([p, execute, allowIdentity, safety] {
            return takeJson(nocry_restore_backup_json(p.constData(), execute, allowIdentity, safety.constData()));
        }));
    }

    int autoRestore = 0;            // self-tests only: 1 = confirm, 2 = confirm and allow another board's identity

    // Runs one advanced operation off the UI thread; `done` gets the result when it succeeded.
    // cancellable: offer Cancel; risk: what stopping half-way leaves behind (asked before stopping; "" = nothing)
    void runAdvanced(const QString &what, std::function<QJsonObject()> job, std::function<QString(const QJsonObject &)> done, bool cancellable = false,
                     const QString &risk = QString())
    {
        if (busy)
            return;
        setBusy(true);
        stateHint->setText(tr("%1. Do not unplug the board.").arg(what));
        log->appendPlainText(what + QStringLiteral("…"));
        if (cancellable)
            allowCancel(risk);
        auto *w = new QFutureWatcher<QJsonObject>(this);
        connect(w, &QFutureWatcher<QJsonObject>::finished, this, [this, w, what, done] {
            const QJsonObject o = w->result();
            w->deleteLater();
            cancelButton->setVisible(false);
            handleEvent(NOCRY_EVENT_PROGRESS_END, {}, 0, 0, {});
            setBusy(false);
            infoStale = true;
            pollState();
            lastResult = o;
            if (o.value("ok").toBool()) {
                const QString msg = done(o);
                log->appendPlainText(msg);
                if (!quiet)
                    QMessageBox::information(this, what, msg);
            } else if (wasCancelled(o)) {
                log->appendPlainText(tr("%1: stopped before it was finished.").arg(what));
            } else {
                log->appendPlainText(tr("%1 failed: %2").arg(what, o.value("error").toString()));
                if (!quiet)
                    QMessageBox::critical(this, what, o.value("error").toString());
            }
            if (onIdle)
                onIdle();
        });
        w->setFuture(QtConcurrent::run(job));
    }

    // The boot picture of the installed slots. want -1: read and offer the choice; 1: nocry's; 2: the firmware's own.
    void startPicture(int want)
    {
        if (want >= 0) {
            runAdvanced(want == 1 ? tr("Setting the nocry boot picture") : tr("Bringing the firmware's boot picture back"),
                [want] { return takeJson(nocry_picture_json(want, 1)); },
                [want](const QJsonObject &o) {
                    if (!o.value("changed").toBool())
                        return tr("Nothing had to be changed.");
                    return want == 1 ? tr("The printer now shows the nocry picture when it powers on. The firmware's own picture is kept on the board.")
                                     : tr("The printer now shows the firmware's own picture (the Creality logo) when it powers on.");
                });
            return;
        }
        if (busy)
            return;
        setBusy(true);
        log->appendPlainText(tr("Reading the boot picture…"));
        auto *w = new QFutureWatcher<QJsonObject>(this);
        connect(w, &QFutureWatcher<QJsonObject>::finished, this, [this, w] {
            const QJsonObject o = w->result();
            w->deleteLater();
            setBusy(false);
            lastResult = o;
            if (!o.value("ok").toBool()) {
                log->appendPlainText(tr("Boot picture: %1").arg(o.value("error").toString()));
                if (!quiet)
                    QMessageBox::critical(this, tr("Boot picture"), o.value("error").toString());
            } else if (!quiet) {
                const int act = choosePicture(o);
                if (act > 0) {
                    startPicture(act);
                    return;
                }
            }
            if (onIdle)
                onIdle();
        });
        w->setFuture(QtConcurrent::run([] { return takeJson(nocry_picture_json(-1, 0)); }));
    }

    // 0 = nothing, 1 = use nocry's, 2 = use the firmware's own
    int choosePicture(const QJsonObject &o)
    {
        auto name = [&](const QString &slot) {
            const QString k = o.value(slot).toString();
            QString t = k == "nocry" ? tr("nocry") : k == "firmware" ? tr("the firmware's own (Creality logo)") : tr("no firmware in this slot");
            if (k == "nocry" && o.value(slot + "_kept").toBool())
                t += tr(" — the firmware's own is kept and can be brought back");
            return t;
        };
        const QString a = o.value("a").toString(), b = o.value("b").toString();
        const bool anyFirmware = a == "firmware" || b == "firmware";
        const bool canBack = (a == "nocry" && o.value("a_kept").toBool()) || (b == "nocry" && o.value("b_kept").toBool());
        QDialog d(this);
        d.setWindowTitle(tr("Boot Picture"));
        auto *v = new QVBoxLayout(&d);
        auto *intro = new QLabel(tr("For the first seconds after power-on, before the printer's software is up, the board shows a picture. "
                                   "It comes from a small program stored with each firmware slot, which also switches the screen on.\n\n"
                                   "nocry has an open one that shows the nocry picture with the printer model. Creality's own is kept on the board when it is replaced, "
                                   "so you can go back at any time. A firmware update from the printer itself brings Creality's back for the slot it updates."));
        intro->setWordWrap(true);
        intro->setMinimumWidth(560);
        v->setSizeConstraint(QLayout::SetMinimumSize);
        v->addWidget(intro);
        auto *f = new QFormLayout;
        f->addRow(tr("Slot A:"), new QLabel(name("a")));
        f->addRow(tr("Slot B:"), new QLabel(name("b")));
        v->addLayout(f);
        if (!o.value("available").toBool())
            v->addWidget(new QLabel(tr("This copy of nocry has no boot picture for this board family.")));
        else if (!o.value("proven").toBool()) {
            auto *l = new QLabel(tr("Warning: nocry's boot picture has not been tested on this board family's own screens. If it does not set the screen up correctly, "
                                    "the screen can stay dark even after the printer has started. You can then bring the firmware's picture back from USB boot mode."));
            l->setWordWrap(true);
            v->addWidget(l);
        }
        int choice = 0;
        auto *row = new QHBoxLayout;
        auto *useNocry = new QPushButton(tr("Use the nocry picture"));
        useNocry->setEnabled(o.value("available").toBool() && anyFirmware);
        auto *useFw = new QPushButton(tr("Use the firmware's picture"));
        useFw->setEnabled(canBack);
        row->addWidget(useNocry);
        row->addWidget(useFw);
        row->addStretch(1);
        v->addLayout(row);
        connect(useNocry, &QPushButton::clicked, &d, [&] { choice = 1; d.accept(); });
        connect(useFw, &QPushButton::clicked, &d, [&] { choice = 2; d.accept(); });
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
        v->addWidget(bb);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        d.exec();
        return choice;
    }

    // The board's first-stage boot loader. action 0: read it and offer what can be done;
    // 1: install nocryboot; 2: put the factory loader back (file: where to take it from, or empty).
    void startLoader(int action, const QString &file)
    {
        const QByteArray f = file.toUtf8(), sd = safetyDir();
        if (action != 0) {
            runAdvanced(action == 1 ? tr("Installing the nocryboot boot loader") : tr("Putting the factory boot loader back"),
                [action, f, sd] { return takeJson(nocry_loader_json(action, f.isEmpty() ? nullptr : f.constData(), 1, sd.constData())); },
                [](const QJsonObject &o) {
                    return tr("The board now has: %1\n\nIt was written and read back correctly. Unplug the board and power it on normally.\n\n"
                              "If the screen stays dark for more than a minute, put the board in USB boot mode with the BOOT button; "
                              "nocry can always put the previous loader back from there.").arg(o.value("loader").toString());
                });
            return;
        }
        if (busy)
            return;
        setBusy(true);
        log->appendPlainText(tr("Reading the boot loader…"));
        auto *w = new QFutureWatcher<QJsonObject>(this);
        connect(w, &QFutureWatcher<QJsonObject>::finished, this, [this, w] {
            const QJsonObject o = w->result();
            w->deleteLater();
            setBusy(false);
            lastResult = o;
            if (!o.value("ok").toBool()) {
                log->appendPlainText(tr("Boot loader: %1").arg(o.value("error").toString()));
                if (!quiet)
                    QMessageBox::critical(this, tr("Boot loader"), o.value("error").toString());
            } else {
                log->appendPlainText(tr("Boot loader on the board: %1").arg(o.value("loader").toString()));
                if (!quiet) {
                    const auto [act, file] = chooseLoader(o);
                    if (act != 0) {
                        startLoader(act, file);
                        return;
                    }
                }
            }
            if (onIdle)
                onIdle();
        });
        w->setFuture(QtConcurrent::run([] { return takeJson(nocry_loader_json(0, nullptr, 0, nullptr)); }));
    }

    // Shows what the board has and returns what to do: {0,""} nothing, {1,file} install, {2,file} factory.
    std::pair<int, QString> chooseLoader(const QJsonObject &o)
    {
        const QString kind = o.value("kind").toString(), blocked = o.value("blocked").toString();
        const bool kept = o.value("factory_kept").toBool(), upToDate = o.value("up_to_date").toBool();
        QDialog d(this);
        d.setWindowTitle(tr("Boot Loader"));
        auto *v = new QVBoxLayout(&d);
        auto *intro = new QLabel(tr("The boot loader is the first program the board runs from its storage. It starts the boot picture and then the firmware.\n\n"
                                   "nocryboot is an open replacement for Creality's. Unlike the factory loader it does not hang when a firmware slot is damaged: "
                                   "it starts the other slot, and if neither can be started it puts the board in USB boot mode by itself, so nocry can repair it "
                                   "without the BOOT button. It can also try newly installed firmware for a few starts and go back if it never comes up."));
        intro->setWordWrap(true);
        v->addWidget(intro);
        auto *f = new QFormLayout;
        f->addRow(tr("On the board:"), new QLabel(o.value("loader").toString()));
        f->addRow(tr("Factory loader kept on the board:"), new QLabel(kept ? tr("yes") : kind == "factory" ? tr("not needed (it is the one in use)") : tr("no")));
        if (!o.value("available").toString().isEmpty())
            f->addRow(tr("This nocry can install:"), new QLabel(o.value("available").toString()));
        v->addLayout(f);
        if (!blocked.isEmpty()) {
            auto *l = new QLabel(tr("nocryboot cannot be installed on this board: %1.").arg(blocked));
            l->setWordWrap(true);
            v->addWidget(l);
        }
        std::pair<int, QString> choice{0, {}};
        auto *row = new QHBoxLayout;
        auto *inst = new QPushButton(kind == "nocryboot" ? tr("Update nocryboot…") : tr("Install nocryboot…"));
        inst->setEnabled(blocked.isEmpty() && !upToDate);
        auto *fac = new QPushButton(tr("Put the factory loader back…"));
        fac->setEnabled(kind != "factory");
        row->addWidget(inst);
        row->addWidget(fac);
        row->addStretch(1);
        v->addLayout(row);
        if (upToDate)
            v->addWidget(new QLabel(tr("The board already has this nocryboot.")));
        connect(inst, &QPushButton::clicked, &d, [&] {
            QString file;
            QString info = tr("The loader is replaced by: %1\n\n").arg(o.value("available").toString());
            if (kind == "factory")
                info += tr("The factory loader is kept on the board itself and on this computer, so it can be put back at any time.");
            else if (kept)
                info += tr("The factory loader kept on the board stays there.");
            else
                info += tr("This board has no copy of its factory loader. If you have a backup of this board made before nocryboot was installed, "
                           "choose it in the next step and nocry keeps the factory loader from it on the board.");
            info += tr("\n\nFirmware, settings, files and the identity are not touched. If the new loader should not start, the BOOT button and nocry bring the old one back.");
            QMessageBox box(QMessageBox::Warning, tr("Boot loader"), tr("Install nocryboot on this board?"), QMessageBox::Cancel, &d);
            box.setInformativeText(info);
            auto *go = box.addButton(tr("Install"), QMessageBox::DestructiveRole);
            box.setDefaultButton(QMessageBox::Cancel);
            box.exec();
            if (box.clickedButton() != go)
                return;
            if (kind != "factory" && !kept)
                file = QFileDialog::getOpenFileName(&d, tr("Backup with the factory loader (Cancel to go on without)"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), tr("Backups and loaders (*.img *.bin);;All files (*)"));
            choice = {1, file};
            d.accept();
        });
        connect(fac, &QPushButton::clicked, &d, [&] {
            QString file;
            if (!kept) {
                QMessageBox::information(&d, tr("Boot loader"), tr("No copy of the factory loader is kept on this board. Choose a backup of this board made before nocryboot was installed; nocry takes the loader from it."));
                file = QFileDialog::getOpenFileName(&d, tr("Backup with the factory loader"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), tr("Backups and loaders (*.img *.bin);;All files (*)"));
                if (file.isEmpty())
                    return;
            }
            QMessageBox box(QMessageBox::Warning, tr("Boot loader"), tr("Put Creality's factory loader back?"), QMessageBox::Cancel, &d);
            box.setInformativeText(tr("The board then behaves as it did from the factory: a damaged firmware slot leaves it dark, and only the BOOT button gets it into USB boot mode. "
                                      "A firmware trial that is under way is ended.\n\nFirmware, settings, files and the identity are not touched."));
            auto *go = box.addButton(tr("Put it back"), QMessageBox::DestructiveRole);
            box.setDefaultButton(QMessageBox::Cancel);
            box.exec();
            if (box.clickedButton() != go)
                return;
            choice = {2, file};
            d.accept();
        });
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
        v->addWidget(bb);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        d.exec();
        return choice;
    }

    QByteArray safetyDir() const { return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("safety").toUtf8(); }

    void startPartBackup(const QString &name, const QString &path)
    {
        const QByteArray n = name.toUtf8(), p = path.toUtf8();
        runAdvanced(tr("Backing up partition %1").arg(name), [n, p] { return takeJson(nocry_partition_backup_json(n.constData(), p.constData())); },
            [path](const QJsonObject &o) {
                return tr("Partition saved and checksummed.\n\nFile: %1\nSize: %2\nSHA-256: %3")
                    .arg(QDir::toNativeSeparators(path), QLocale().formattedDataSize(qint64(o.value("bytes").toDouble())), o.value("sha256").toString());
            }, true);
    }
    void startPartRestore(const QString &name, const QString &file)
    {
        const QByteArray n = name.toUtf8(), f = file.toUtf8(), sd = safetyDir();
        runAdvanced(tr("Restoring partition %1").arg(name), [n, f, sd] { return takeJson(nocry_partition_restore_json(n.constData(), f.constData(), sd.constData())); },
            [name](const QJsonObject &o) {
                return o.value("bytes_written").toDouble() == 0 ? tr("Partition %1 already held exactly this data. Nothing was written.").arg(name)
                                                                : tr("Partition %1 was written and verified (%2 changed).").arg(name, QLocale().formattedDataSize(qint64(o.value("bytes_written").toDouble())));
            }, true,
            tr("Partition %1 then holds a mix of its old contents and the file's. Whatever uses it will probably not work until the restore is run again to the end.").arg(name));
    }
    void startPartErase(const QString &name)
    {
        const QByteArray n = name.toUtf8(), sd = safetyDir();
        runAdvanced(tr("Erasing partition %1").arg(name), [n, sd] { return takeJson(nocry_partition_erase_json(n.constData(), sd.constData())); },
            [name](const QJsonObject &) { return tr("Partition %1 is erased (all zeros) and verified.").arg(name); }, true,
            tr("Partition %1 is then partly erased: neither its old contents nor empty. Whatever uses it will probably not work until it is erased completely or written again.").arg(name));
    }
    void startEraseAll()
    {
        const QByteArray sd = safetyDir();
        runAdvanced(tr("Erasing the entire storage"), [sd] { return takeJson(nocry_erase_disk_json(sd.constData())); },
            [](const QJsonObject &) {
                return tr("The board's storage is completely erased. The board is blank and will only start in USB boot mode.\n\nUse Install firmware to rebuild it.");
            }, true,
            tr("The storage is then partly erased. What has been erased so far is gone and is not brought back by stopping; the board will most likely "
               "only start in USB boot mode, as after a complete erase."));
    }
    void startSecureWipe()
    {
        const QByteArray sd = safetyDir();
        runAdvanced(tr("Secure erase"), [sd] { return takeJson(nocry_secure_wipe_json(sd.constData())); },
            [](const QJsonObject &o) {
                QStringList how;
                for (const auto &p : o.value("partitions").toArray()) {
                    const QString m = p.toObject().value("method").toString();
                    how << (m == "secure-discard" ? tr("the storage chip's secure erase") : m == "discard" ? tr("the storage chip's erase command") : tr("overwriting with zeros"));
                }
                how.removeDuplicates();
                return tr("Settings and files are erased (%1, using %2) and every part was checked to be empty.\n\nUnplug the board, then power it on normally. The first start takes longer while the printer sets itself up again.")
                    .arg(QLocale().formattedDataSize(qint64(o.value("bytes").toDouble())), how.join(tr(" and ")));
            }, true,
            tr("Settings and files are then partly erased: what is gone stays gone, and the rest may still be readable. "
               "The printer may not start properly until the erase is run to the end."));
    }
    void startIdentity(const QString &sn, const QString &mac, const QString &model, const QString &board)
    {
        const QByteArray a = sn.toUtf8(), b = mac.toUtf8(), c = model.toUtf8(), e = board.toUtf8(), sd = safetyDir();
        runAdvanced(tr("Writing the identity"), [a, b, c, e, sd] { return takeJson(nocry_set_identity_json(a.constData(), b.constData(), c.constData(), e.constData(), sd.constData())); },
            [](const QJsonObject &o) {
                return tr("Identity written and verified.\n\nSerial number: %1\nMAC address: %2\nModel: %3").arg(o.value("sn").toString(), o.value("mac").toString(), o.value("model").toString());
            });
    }

    // Factory reset; the user has already confirmed in chooseReset().
    void startReset(bool keepUserdata)
    {
        if (busy)
            return;
        setBusy(true);
        stateHint->setText(tr("Resetting. Do not unplug the board."));
        allowCancel(tr("Settings and files are then partly erased. The printer may not start properly until a factory reset is run to the end."));
        log->appendPlainText(tr("Factory reset…"));
        const QByteArray safety = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("safety").toUtf8();
        auto *w = new QFutureWatcher<QJsonObject>(this);
        connect(w, &QFutureWatcher<QJsonObject>::finished, this, [this, w] {
            const QJsonObject o = w->result();
            w->deleteLater();
            handleEvent(NOCRY_EVENT_PROGRESS_END, {}, 0, 0, {});
            setBusy(false);
            pollState();
            lastResult = o;
            if (!o.value("ok").toBool()) {
                log->appendPlainText(wasCancelled(o) ? tr("Factory reset stopped before it was finished.") : tr("Factory reset failed: %1").arg(o.value("error").toString()));
                if (!quiet && !wasCancelled(o))
                    QMessageBox::critical(this, tr("Factory reset failed"), o.value("error").toString());
            } else {
                infoStale = true;
                const QString msg = o.value("executed").toBool()
                    ? tr("Factory reset complete. Unplug the board, then power it on normally. The first start takes longer while the printer sets itself up again.")
                    : tr("The board's settings and files are already blank. Nothing needed to be erased.");
                log->appendPlainText(msg);
                if (!quiet)
                    QMessageBox::information(this, tr("Factory reset"), msg);
            }
            if (onIdle)
                onIdle();
        });
        w->setFuture(QtConcurrent::run([keepUserdata, safety] {
            return takeJson(nocry_factory_reset_json(keepUserdata, 1, safety.constData()));
        }));
    }

    QString autoSlot;               // self-tests only: slot to pick without a dialog
    bool autoConfirm = false;       // self-tests only

    QTabWidget *tabWidget = nullptr;
    void openInstallDialog() { chooseInstall(); }
    void setPictureChoice(int p) { pendingPicture = p; }   // self-tests: what the install dialog's tick box would set
    QString autoModel, autoBoard;   // self-tests: answers for the printer type / mainboard questions
    void openShellHelp() { showShellHelp(); }
    void openNetworkDialog() { chooseNetwork(); }
    void openWifiDialog() { startWifi(); }
    // self-tests: press Cancel if it is on offer; show the question an install's Cancel asks
    bool cancelOffered() const { return busy && cancelButton->isVisible() && cancelButton->isEnabled() && !cancelRisk.isEmpty(); }
    bool testCancel()
    {
        if (!busy || !cancelButton->isVisible() || !cancelButton->isEnabled())
            return false;
        requestCancel();
        return true;
    }
    void testCancelQuestion()
    {
        setBusy(true);
        progressLabel->setText(tr("Writing — 96 MB of 143 MB"));
        allowCancel(tr("The firmware on the board is being replaced and is incomplete right now. If you stop, the board will not start "
                       "until the installation is run again to the end.\n\nnocry can always do that: USB boot mode does not depend on what is installed."));
        requestCancel();
        setBusy(false);
    }
    // self-tests: pair without the dialog; true when the board accepted the code
    bool testConnect(const QString &address, const QString &code)
    {
        const QJsonObject o = takeJson(nocry_net_connect_json(address.toUtf8().constData(), code.toUtf8().constData()));
        log->appendPlainText(o.value("ok").toBool() ? tr("Connected to %1 over the network.").arg(address) : o.value("error").toString());
        if (o.value("ok").toBool())
            networkConnected(address);
        return o.value("ok").toBool();
    }
    void openBootHelp() { showBootHelp(); }
    void fetchBoardLog() { showBoardLog(); }
    void testSecureWipe() { startSecureWipe(); }
    // self-test entry points (no dialogs)
    void testAdvanced(const QStringList &a)
    {
        const QString op = a.value(0);
        if (op == "pbackup") startPartBackup(a.value(1), a.value(2));
        else if (op == "prestore") startPartRestore(a.value(1), a.value(2));
        else if (op == "perase") startPartErase(a.value(1));
        else if (op == "erase") startEraseAll();
        else if (op == "identity") startIdentity(a.value(1), a.value(2), a.value(3), a.value(4));
        else if (op == "securewipe") startSecureWipe();
        else if (op == "boardlog") showBoardLog();
        else QApplication::exit(3);
    }
    std::function<void()> onIdle;   // used by the self-tests
    QJsonObject lastResult;
    bool quiet = false;             // self-tests: no modal dialogs

protected:
    void closeEvent(QCloseEvent *e) override
    {
        if (busy) {
            QMessageBox::information(this, tr("nocry"), tr("An operation is running. Cancel it or wait for it to finish before closing."));
            e->ignore();
        } else {
            e->accept();
        }
    }

private:
    bool confirmInstall(const QJsonObject &o, const QString &fw, int keepData)
    {
        const QString before = o.value("model_before").toString(), after = o.value("model_after").toString();
        const QString beforeName = o.value("model_before_name").toString(), afterName = o.value("model_after_name").toString();
        const QString curVer = o.value("device_firmware").toString().section(' ', 1);
        QString curName = o.value("device_firmware_name").toString();
        const bool converting = !before.isEmpty() && before != after;
        QString info;
        if (converting) {
            // the most important thing on this screen: say it first, in plain words
            if (after == "Nebula Pad")
                info = tr("You are changing this board from %1 firmware to that of a retail Nebula Pad. It will no longer be set up for the %1 and will work as a generic Nebula Pad instead.").arg(beforeName);
            else if (before == "Nebula Pad")
                info = tr("You are changing this Nebula Pad from retail Nebula Pad firmware to that of the %1. It will no longer function on generic printers and will only be suitable for use on the %1.").arg(afterName);
            else
                info = tr("You are changing this board from %1 firmware to that of the %2. It will no longer work on the %1 and will only be suitable for use on the %2.").arg(beforeName, afterName);
            info += tr("\n\nIf you aren't absolutely sure this is what you want to do, click Cancel.\n\n");
        }
        info += tr("Currently on the board: %1\nTo be written: %2")
            .arg(curName.isEmpty() ? tr("no readable firmware") : tr("%1 firmware %2").arg(curName, curVer).simplified(),
                 QLocale().formattedDataSize(qint64(o.value("bytes_to_write").toDouble())));
        if (o.value("blank_board").toBool())
            info = tr("This board is blank. nocry will rebuild it from scratch: partition table, bootloader, firmware and identity.\n\n") + info;
        if (keepData == 2)
            info += tr("\n\nIt goes into slot %1, the one that is not in use. The firmware that runs now stays where it is and keeps starting until you choose the other slot with Switch Active Slot. Settings and files are shared by both slots and are not changed.")
                        .arg(o.value("target_slot").toString().toUpper());
        else if (keepData && !o.value("blank_board").toBool())
            info += tr("\n\nYour settings, files, serial number and the other firmware slot are kept.");
        else
            info += tr("\n\nClean install: all settings and files on the printer will be erased. The serial number and MAC address are kept.");
        if (o.value("identity_generated").toBool())
            info += tr("\n\nThis board has no valid serial number, so a new serial number and MAC address will be generated.");
        QStringList det;
        for (const auto &n : o.value("notes").toArray())
            det << n.toString();
        det << QString() << o.value("details").toString();
        log->appendPlainText(info);
        if (autoConfirm)
            return true;
        if (quiet)
            return false;
        QMessageBox box(QMessageBox::Warning, tr("Install firmware"), tr("Install %1 on this board?").arg(fw), QMessageBox::Cancel, this);
        box.setInformativeText(info);
        box.setDetailedText(det.join('\n'));
        auto *go = box.addButton(keepData == 2 ? tr("Install to slot %1").arg(o.value("target_slot").toString().toUpper())
                                 : keepData ? tr("Install") : tr("Erase and install"), QMessageBox::DestructiveRole);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        return box.clickedButton() == go;
    }

    static QString slotText(const QString &letter, const QJsonObject &s, bool active)
    {
        QString t = s.value("bootable").toBool()
            ? tr("Slot %1 — %2 firmware %3").arg(letter, s.value("firmware_name").toString(), s.value("firmware_version").toString())
            : tr("Slot %1 — empty or incomplete").arg(letter);
        if (active)
            t += tr("  (starts now)");
        return t;
    }

    // Returns "a"/"b" if the user chose a different, usable system; "" otherwise.
    QString chooseSlot(const QJsonObject &o)
    {
        const QString active = o.value("active").toString();
        const QJsonObject a = o.value("a").toObject(), b = o.value("b").toObject();
        log->appendPlainText(slotText("A", a, active == "a"));
        log->appendPlainText(slotText("B", b, active == "b"));
        if (!autoSlot.isEmpty())
            return autoSlot == active ? QString() : autoSlot;
        if (quiet)
            return QString();
        QDialog d(this);
        d.setWindowTitle(tr("Switch Active Slot"));
        auto *v = new QVBoxLayout(&d);
        v->addWidget(new QLabel(tr("The board holds two firmware slots. Choose the one that starts when it is powered on:")));
        auto *ra = new QRadioButton(slotText("A", a, active == "a"));
        auto *rb = new QRadioButton(slotText("B", b, active == "b"));
        ra->setEnabled(a.value("bootable").toBool());
        rb->setEnabled(b.value("bootable").toBool());
        (active == "b" ? rb : ra)->setChecked(true);
        v->addWidget(ra);
        v->addWidget(rb);
        if (!a.value("bootable").toBool() || !b.value("bootable").toBool())
            v->addWidget(new QLabel(tr("A slot without complete firmware cannot be selected.")));
        v->addWidget(new QLabel(tr("Settings and files are shared by both slots and are not changed.")));
        // copying one slot's firmware over the other
        QString copyFrom;
        auto *copyRow = new QHBoxLayout;
        for (const QString &f : {QStringLiteral("a"), QStringLiteral("b")}) {
            const QString F = f.toUpper(), T = f == "a" ? "B" : "A";
            const QJsonObject src = f == "a" ? a : b, dst = f == "a" ? b : a;
            auto *cb = new QPushButton(tr("Copy slot %1 to slot %2…").arg(F, T));
            cb->setEnabled(src.value("bootable").toBool());
            copyRow->addWidget(cb);
            connect(cb, &QPushButton::clicked, &d, [&, f, F, T, src, dst] {
                QString info = tr("Slot %1 will get an exact copy of the firmware in slot %2 (%3 %4).").arg(T, F, src.value("firmware_name").toString(), src.value("firmware_version").toString());
                if (dst.value("bootable").toBool())
                    info += tr("\n\nThe firmware now in slot %1 (%2 %3) will be replaced.").arg(T, dst.value("firmware_name").toString(), dst.value("firmware_version").toString());
                if (active == T.toLower())
                    info += tr("\n\nSlot %1 is the active slot: this replaces the firmware the board starts.").arg(T);
                info += tr("\n\nWhich slot starts, and your settings and files, are not changed.");
                QMessageBox box(QMessageBox::Warning, tr("Copy slot"), tr("Copy slot %1 to slot %2?").arg(F, T), QMessageBox::Cancel, &d);
                box.setInformativeText(info);
                auto *go = box.addButton(tr("Copy"), QMessageBox::DestructiveRole);
                box.setDefaultButton(QMessageBox::Cancel);
                box.exec();
                if (box.clickedButton() == go) {
                    copyFrom = f;
                    d.accept();
                }
            });
        }
        copyRow->addStretch(1);
        v->addLayout(copyRow);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        bb->button(QDialogButtonBox::Ok)->setText(tr("Switch"));
        v->addWidget(bb);
        auto update = [=] { bb->button(QDialogButtonBox::Ok)->setEnabled((ra->isChecked() ? "a" : "b") != active); };
        connect(ra, &QRadioButton::toggled, &d, update);
        update();
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        if (d.exec() != QDialog::Accepted)
            return QString();
        if (!copyFrom.isEmpty())
            return QStringLiteral("copy:") + copyFrom;
        return ra->isChecked() ? "a" : "b";
    }

    // 0 = cancelled, 1 = restore, 2 = restore a different board's backup (copies its identity)
    int confirmRestore(const QJsonObject &o, const QString &image)
    {
        const bool other = o.value("different_board").toBool();
        const QString im = o.value("image_model").toString(), isn = o.value("image_sn_tail").toString();
        QString info = tr("Everything on the board — firmware, settings, files and identity — will be replaced with the contents of this backup. Only the parts that differ are written.\n\nBackup: %1")
            .arg(QFileInfo(image).fileName());
        if (!im.isEmpty())
            info += tr("\nMade from: %1, serial number ending in %2").arg(im, isn);
        if (other)
            info += tr("\n\nWarning: this backup was made from a different board (this board's serial number ends in %1). Restoring it copies that board's serial number and MAC address onto this one. Two boards with the same identity can conflict on your network and with Creality Cloud.\n\nIf you aren't absolutely sure this is what you want to do, click Cancel.").arg(o.value("device_sn_tail").toString());
        if (!o.value("has_checksum").toBool())
            info += tr("\n\nNote: there is no checksum file next to this backup, so the file itself cannot be checked for damage first.");
        log->appendPlainText(info);
        if (autoRestore)
            return (other && autoRestore != 2) ? 0 : (other ? 2 : 1);
        if (quiet)
            return 0;
        QMessageBox box(QMessageBox::Warning, tr("Restore backup"), tr("Restore this backup to the board?"), QMessageBox::Cancel, this);
        box.setInformativeText(info);
        auto *go = box.addButton(other ? tr("Restore and copy identity") : tr("Erase and restore"), QMessageBox::DestructiveRole);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        return box.clickedButton() == go ? (other ? 2 : 1) : 0;
    }

    QString selectedPartition() const
    {
        const int r = parts->currentRow();
        return r >= 0 && parts->item(r, 0) ? parts->item(r, 0)->text() : QString();
    }

    void networkConnected(const QString &address)
    {
        networkAddress = address;
        infoStale = true;      // whatever is shown belongs to another board
        autoBlocked = false;
        pollState();
    }

    void disconnectNetwork()
    {
        if (busy)
            return;
        nocry_string_free(nocry_net_connect_json(nullptr, nullptr));
        log->appendPlainText(tr("Disconnected from %1. The board keeps running its rescue system.").arg(networkAddress));
        networkAddress.clear();
        pollState();
    }

    // Find a board running nocry-rescue on the network and pair with the code on its screen.
    void chooseNetwork()
    {
        struct NetDialog : QDialog {
            using QDialog::QDialog;
            bool working = false;      // a pairing attempt is under way: stay open until it ends
            void reject() override
            {
                if (!working)
                    QDialog::reject();
            }
        };
        NetDialog d(this);
        d.setWindowTitle(tr("Connect over the network"));
        auto *v = new QVBoxLayout(&d);
        auto *intro = new QLabel(tr("A board that shows the nocry rescue screen can be worked on over Wi-Fi, without a USB cable.\n\n"
                                    "This computer has to be on the same network as the board. If the board is not on your network, "
                                    "join the Wi-Fi network shown on its screen first."));
        intro->setWordWrap(true);
        intro->setMinimumWidth(500);
        v->addWidget(intro);
        v->setSizeConstraint(QLayout::SetFixedSize);   // follows the wrapped text as the status line changes
        auto *form = new QFormLayout;
        auto *addr = new QComboBox;
        addr->setEditable(true);
        addr->setMinimumWidth(320);
        QSettings settings(QStringLiteral("nocry"), QStringLiteral("nocry"));
        const QString last = settings.value("network/address").toString();
        if (!last.isEmpty())
            addr->addItem(last, last);
        auto *again = new QPushButton(tr("Search again"));
        auto *addrRow = new QHBoxLayout;
        addrRow->addWidget(addr, 1);
        addrRow->addWidget(again);
        form->addRow(tr("Board:"), addrRow);
        auto *code = new QLineEdit;
        code->setPlaceholderText(tr("the 6 digits on the board's screen"));
        code->setMaxLength(12);
        form->addRow(tr("Pairing code:"), code);
        v->addLayout(form);
        auto *status = new QLabel;
        status->setWordWrap(true);
        status->setMinimumWidth(500);
        v->addWidget(status);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        QPushButton *go = bb->button(QDialogButtonBox::Ok);
        go->setText(tr("Connect"));
        v->addWidget(bb);

        // what the user typed, or the address behind the entry they picked
        auto address = [addr] {
            const int i = addr->findText(addr->currentText());
            return (i >= 0 && addr->itemData(i).isValid() ? addr->itemData(i).toString() : addr->currentText()).trimmed();
        };
        auto valid = [&d, go, code, address] {
            go->setEnabled(!d.working && !address().isEmpty() && code->text().remove(' ').size() >= 4);
        };
        connect(code, &QLineEdit::textChanged, &d, valid);
        connect(addr, &QComboBox::currentTextChanged, &d, valid);

        auto search = [&d, addr, again, status, valid] {
            again->setEnabled(false);
            status->setText(tr("Looking for boards on the network…"));
            auto *w = new QFutureWatcher<QJsonObject>(&d);
            connect(w, &QFutureWatcher<QJsonObject>::finished, &d, [w, addr, again, status, valid] {
                const QJsonArray boards = w->result().value("boards").toArray();
                w->deleteLater();
                again->setEnabled(true);
                const QString typed = addr->currentText();
                for (const auto &b : boards) {
                    const QJsonObject o = b.toObject();
                    QString a = o.value("address").toString();
                    if (o.value("port").toInt() != 5560)
                        a += QStringLiteral(":%1").arg(o.value("port").toInt());
                    if (addr->findData(a) < 0)
                        addr->insertItem(0, tr("%1 at %2").arg(o.value("name").toString(), a), a);
                }
                if (!boards.isEmpty() && (typed.isEmpty() || addr->findText(typed) >= 0))
                    addr->setCurrentIndex(0);
                else
                    addr->setEditText(typed);
                status->setText(boards.isEmpty()
                    ? tr("No board answered. Check that this computer is on the same network as the board, or type the address shown on the board's screen.")
                    : tr("Type the pairing code shown on the board's screen."));
                valid();
            });
            w->setFuture(QtConcurrent::run([] { return takeJson(nocry_net_discover_json(2500)); }));
        };
        connect(again, &QPushButton::clicked, &d, search);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        connect(bb, &QDialogButtonBox::accepted, &d, [&d, this, addr, again, code, status, bb, address, valid] {
            const QString a = address();
            d.working = true;
            for (QWidget *x : {static_cast<QWidget *>(addr), static_cast<QWidget *>(again), static_cast<QWidget *>(code), static_cast<QWidget *>(bb)})
                x->setEnabled(false);
            status->setText(tr("Pairing with %1…").arg(a));
            auto *w = new QFutureWatcher<QJsonObject>(&d);
            connect(w, &QFutureWatcher<QJsonObject>::finished, &d, [&d, this, w, a, addr, again, code, status, bb, valid] {
                const QJsonObject o = w->result();
                w->deleteLater();
                d.working = false;
                for (QWidget *x : {static_cast<QWidget *>(addr), static_cast<QWidget *>(again), static_cast<QWidget *>(code), static_cast<QWidget *>(bb)})
                    x->setEnabled(true);
                if (o.value("ok").toBool()) {
                    QSettings(QStringLiteral("nocry"), QStringLiteral("nocry")).setValue("network/address", a);
                    log->appendPlainText(tr("Connected to %1 over the network (encrypted, paired with the code on its screen).").arg(a));
                    networkAddress = a;
                    d.accept();
                    return;
                }
                status->setText(tr("<b>Could not connect.</b> %1").arg(o.value("error").toString().toHtmlEscaped()));
                code->selectAll();
                code->setFocus();
                valid();
            });
            const QByteArray ab = a.toUtf8(), cb = code->text().toUtf8();
            w->setFuture(QtConcurrent::run([ab, cb] { return takeJson(nocry_net_connect_json(ab.constData(), cb.constData())); }));
        });
        valid();
        search();
        code->setFocus();
        if (d.exec() == QDialog::Accepted)
            networkConnected(networkAddress);
    }

    // The Wi-Fi of a board running nocry-rescue: what it is on, and joining another network.
    void startWifi()
    {
        if (busy)
            return;
        setBusy(true);
        stateHint->setText(tr("Asking the board about its Wi-Fi."));
        auto *w = new QFutureWatcher<QJsonObject>(this);
        connect(w, &QFutureWatcher<QJsonObject>::finished, this, [this, w] {
            const QJsonObject o = w->result();
            w->deleteLater();
            setBusy(false);
            pollState();
            lastResult = o;
            if (!o.value("ok").toBool()) {
                log->appendPlainText(tr("Wi-Fi: %1").arg(o.value("error").toString()));
                if (!quiet)
                    QMessageBox::warning(this, tr("Board's Wi-Fi"), o.value("error").toString());
            } else {
                chooseWifi(o);
            }
            if (onIdle)
                onIdle();
        });
        w->setFuture(QtConcurrent::run([] { return takeJson(nocry_wifi_json(0, nullptr, nullptr)); }));
    }

    static QString wifiSummary(const QJsonObject &o)
    {
        const QString mode = o.value("mode").toString(), ssid = o.value("ssid").toString(), addr = o.value("address").toString(), src = o.value("source").toString();
        QString t;
        if (mode == "client")
            t = tr("The board is on the Wi-Fi network <b>%1</b>, address %2.").arg(ssid.toHtmlEscaped(), addr.toHtmlEscaped());
        else if (mode == "ap")
            t = tr("The board is running its own Wi-Fi network <b>%1</b> (address %2).").arg(ssid.toHtmlEscaped(), addr.toHtmlEscaped());
        else if (mode == "joining")
            t = tr("The board is joining <b>%1</b> right now.").arg(ssid.toHtmlEscaped());
        else
            t = tr("The board has no working Wi-Fi.");
        if (src == "saved")
            t += tr(" It joined the network the installed firmware had saved.");
        else if (src.startsWith("failed:"))
            t += tr("<br><b>It could not join %1</b>: wrong password, or the network is out of range.").arg(src.mid(7).toHtmlEscaped());
        else if (mode == "ap" && !o.value("saved_from").toString().isEmpty())
            t += tr(" The installed firmware's saved network (%1) could not be joined.").arg(o.value("saved_names").toString().toHtmlEscaped());
        return t;
    }

    void chooseWifi(const QJsonObject &o)
    {
        log->appendPlainText(QTextDocumentFragment::fromHtml(wifiSummary(o)).toPlainText());
        if (quiet)
            return;
        const bool overNet = o.value("over_network").toBool();
        QDialog d(this);
        d.setWindowTitle(tr("Board's Wi-Fi"));
        auto *v = new QVBoxLayout(&d);
        v->setSizeConstraint(QLayout::SetFixedSize);
        auto *sum = new QLabel(wifiSummary(o));
        sum->setWordWrap(true);
        sum->setMinimumWidth(480);
        v->addWidget(sum);
        v->addWidget(new QLabel(tr("Make it join this network (2.4 GHz only):")));
        auto *list = new QListWidget;
        list->setMinimumHeight(140);
        for (const auto &nv : o.value("networks").toArray()) {
            const QJsonObject n = nv.toObject();
            const int sig = n.value("signal").toInt();
            auto *it = new QListWidgetItem(tr("%1 — %2%3").arg(n.value("ssid").toString(), sig >= -60 ? tr("strong") : sig >= -72 ? tr("medium") : tr("weak"),
                                                                n.value("secured").toBool() ? QString() : tr(", open")));
            it->setData(Qt::UserRole, n.value("ssid").toString());
            list->addItem(it);
        }
        v->addWidget(list);
        if (list->count() == 0)
            list->addItem(tr("(the board found no networks when it started; type the name below)"));
        auto *form = new QFormLayout;
        auto *name = new QLineEdit;
        name->setMaxLength(32);
        auto *pass = new QLineEdit;
        pass->setEchoMode(QLineEdit::Password);
        pass->setMaxLength(63);
        pass->setPlaceholderText(tr("leave empty for an open network"));
        auto *showPass = new QCheckBox(tr("Show"));
        auto *passRow = new QHBoxLayout;
        passRow->addWidget(pass, 1);
        passRow->addWidget(showPass);
        form->addRow(tr("Network name:"), name);
        form->addRow(tr("Password:"), passRow);
        v->addLayout(form);
        auto *note = new QLabel(overNet
            ? tr("The board then leaves the network it is on, so this connection ends. Put this computer on the chosen network too: nocry looks for the board there and connects again by itself. "
                 "If the board cannot join, it goes back to its own network within a minute.")
            : tr("If the board cannot join, it goes back to its own network within a minute. Its screen shows where it is."));
        note->setWordWrap(true);
        note->setMinimumWidth(480);
        v->addWidget(note);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
        QPushButton *join = bb->addButton(tr("Join"), QDialogButtonBox::AcceptRole);
        QPushButton *own = nullptr;
        if (o.value("mode").toString() == "client")
            own = bb->addButton(tr("Back to its own network"), QDialogButtonBox::ActionRole);
        v->addWidget(bb);
        connect(list, &QListWidget::currentItemChanged, &d, [name](QListWidgetItem *it) {
            if (it && it->data(Qt::UserRole).isValid())
                name->setText(it->data(Qt::UserRole).toString());
        });
        connect(showPass, &QCheckBox::toggled, &d, [pass](bool on) { pass->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
        auto valid = [join, name, pass] {
            const int n = pass->text().toUtf8().size();
            join->setEnabled(!name->text().isEmpty() && (n == 0 || (n >= 8 && n <= 63)));
        };
        connect(name, &QLineEdit::textChanged, &d, valid);
        connect(pass, &QLineEdit::textChanged, &d, valid);
        valid();
        int action = 0;
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        connect(join, &QPushButton::clicked, &d, [&] { action = 1; d.accept(); });
        if (own)
            connect(own, &QPushButton::clicked, &d, [&] { action = 2; d.accept(); });
        if (d.exec() != QDialog::Accepted || !action)
            return;
        changeWifi(action, name->text(), pass->text());
    }

    // action 1: join `ssid`; 2: the board's own network. Over the network, then find the board again.
    void changeWifi(int action, const QString &ssid, const QString &password)
    {
        if (busy)
            return;
        setBusy(true);
        const QString where = action == 1 ? ssid : QStringLiteral("nocry-rescue");
        stateHint->setText(action == 1 ? tr("Telling the board to join %1.").arg(ssid) : tr("Telling the board to open its own network."));
        log->appendPlainText(stateHint->text());
        const QByteArray s = ssid.toUtf8(), p = password.toUtf8();
        auto *w = new QFutureWatcher<QJsonObject>(this);
        connect(w, &QFutureWatcher<QJsonObject>::finished, this, [this, w, where] {
            const QJsonObject o = w->result();
            w->deleteLater();
            lastResult = o;
            if (!o.value("ok").toBool()) {
                setBusy(false);
                pollState();
                log->appendPlainText(tr("Wi-Fi: %1").arg(o.value("error").toString()));
                if (!quiet)
                    QMessageBox::warning(this, tr("Board's Wi-Fi"), o.value("error").toString());
                return;
            }
            if (!o.value("over_network").toBool()) {       // by USB: the cable stays, nothing to find again
                setBusy(false);
                pollState();
                const QString msg = tr("The board is changing its Wi-Fi to %1. That takes up to a minute; its screen shows the result, and \"Board's Wi-Fi…\" shows it here.").arg(where);
                log->appendPlainText(msg);
                if (!quiet)
                    QMessageBox::information(this, tr("Board's Wi-Fi"), msg);
                return;
            }
            // over the network: the connection is gone; look for the board where it comes up
            networkAddress.clear();
            stateTitle->setText(tr("Looking for the board…"));
            stateHint->setText(tr("Put this computer on the Wi-Fi network %1. nocry connects again by itself when it sees the board (up to 3 minutes).").arg(where));
            progressLabel->setText(tr("Looking for the board on the network"));
            allowCancel();
            auto *f = new QFutureWatcher<QJsonObject>(this);
            connect(f, &QFutureWatcher<QJsonObject>::finished, this, [this, f, where] {
                const QJsonObject r = f->result();
                f->deleteLater();
                setBusy(false);
                lastResult = r;
                if (r.value("ok").toBool()) {
                    const QString a = r.value("address").toString();
                    const bool fellBack = a.startsWith("192.168.77.") && where != "nocry-rescue";
                    const QString msg = fellBack
                        ? tr("The board is back on its own network (%1): it could not join %2. Check the password and that the network is a 2.4 GHz one in range, then try again.").arg(a, where)
                        : tr("Connected again: the board is at %1.").arg(a);
                    log->appendPlainText(msg);
                    networkConnected(a);
                    if (!quiet)
                        fellBack ? QMessageBox::warning(this, tr("Board's Wi-Fi"), msg) : QMessageBox::information(this, tr("Board's Wi-Fi"), msg);
                } else {
                    pollState();
                    const QString msg = wasCancelled(r)
                        ? tr("Stopped looking. The board keeps running; use \"Connect over the network…\" when this computer is on its network. The pairing code on its screen is unchanged.")
                        : tr("%1\n\nThe board keeps running. Its screen shows which network it is on and its address; use \"Connect over the network…\" from a computer on that network.").arg(r.value("error").toString());
                    log->appendPlainText(msg);
                    if (!quiet && !wasCancelled(r))
                        QMessageBox::warning(this, tr("Board's Wi-Fi"), msg);
                }
            });
            f->setFuture(QtConcurrent::run([] { return takeJson(nocry_wifi_json(3, nullptr, nullptr)); }));
        });
        w->setFuture(QtConcurrent::run([action, s, p] { return takeJson(nocry_wifi_json(action, s.constData(), p.constData())); }));
    }

    void updateButtons()
    {
        const bool ok = !busy && canRead;
        const bool sel = ok && !selectedPartition().isEmpty();
        partBackupButton->setEnabled(sel);
        partRestoreButton->setEnabled(sel);
        partEraseButton->setEnabled(sel);
        identityButton->setEnabled(ok && parts->rowCount() > 0);
        loaderButton->setEnabled(ok && parts->rowCount() > 0);
        pictureButton->setEnabled(ok && parts->rowCount() > 0);
        eraseAllButton->setEnabled(ok);
    }

    bool confirmDanger(const QString &title, const QString &question, const QString &info, const QString &action)
    {
        QMessageBox box(QMessageBox::Warning, title, question, QMessageBox::Cancel, this);
        box.setInformativeText(info);
        auto *go = box.addButton(action, QMessageBox::DestructiveRole);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        return box.clickedButton() == go;
    }

    static QString partitionRole(const QString &n)
    {
        if (n == "sn_mac") return tr("It holds the serial number, MAC address and model.");
        if (n == "ota") return tr("It records which firmware slot starts.");
        if (n.startsWith("rtos")) return tr("It holds the real-time firmware that starts before Linux.");
        if (n.startsWith("kernel")) return tr("It holds the Linux kernel of a firmware slot.");
        if (n == "rootfs" || n == "rootfs2") return tr("It holds the system of a firmware slot.");
        if (n == "rootfs_data") return tr("It holds settings and system changes.");
        if (n == "userdata") return tr("It holds the printer's files and configuration.");
        return QString();
    }

    void choosePartBackup()
    {
        const QString name = selectedPartition();
        if (name.isEmpty())
            return;
        QString fn = QStringLiteral("nocry-%1").arg(name);
        if (serial->text().size() >= 4 && serial->text() != QStringLiteral("—"))
            fn += QStringLiteral("-%1").arg(serial->text().right(4));
        fn += QStringLiteral("-%1.img").arg(QDate::currentDate().toString("yyyyMMdd"));
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        const QString path = QFileDialog::getSaveFileName(this, tr("Save partition %1 as").arg(name), QDir(dir).filePath(fn), tr("Disk images (*.img);;All files (*)"));
        if (!path.isEmpty())
            startPartBackup(name, path);
    }

    void choosePartRestore()
    {
        const QString name = selectedPartition();
        if (name.isEmpty())
            return;
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        const QString file = QFileDialog::getOpenFileName(this, tr("Choose the data for partition %1").arg(name), dir, tr("Disk images (*.img *.bin *.squashfs);;All files (*)"));
        if (file.isEmpty())
            return;
        if (confirmDanger(tr("Restore partition"), tr("Replace the contents of partition \"%1\"?").arg(name),
                tr("%1\n\nIt will be overwritten with:\n%2 (%3)\n\nnocry does not check that this file belongs in this partition.")
                    .arg(partitionRole(name), QDir::toNativeSeparators(file), QLocale().formattedDataSize(QFileInfo(file).size())).trimmed(),
                tr("Overwrite partition")))
            startPartRestore(name, file);
    }

    void choosePartErase()
    {
        const QString name = selectedPartition();
        if (name.isEmpty())
            return;
        if (confirmDanger(tr("Erase partition"), tr("Erase partition \"%1\"?").arg(name),
                tr("%1\n\nThe whole partition is filled with zeros. This cannot be undone except from a backup.").arg(partitionRole(name)).trimmed(),
                tr("Erase partition")))
            startPartErase(name);
    }

    void chooseEraseAll()
    {
        if (!confirmDanger(tr("Erase entire storage"), tr("Erase everything on this board?"),
                tr("This wipes the partition table, the bootloader, both firmware slots, all settings and files, and the board's serial number and MAC address.\n\n"
                   "Afterwards the board is blank. It will not start by itself; it can only be rebuilt with nocry from USB boot mode, and it gets a new, generated "
                   "serial number unless you enter the old one again.\n\nMake a backup first if there is anything you may want back."),
                tr("Continue…")))
            return;
        bool ok = false;
        const QString t = QInputDialog::getText(this, tr("Erase entire storage"), tr("Type ERASE to confirm:"), QLineEdit::Normal, QString(), &ok);
        if (ok && t.trimmed() == QStringLiteral("ERASE"))
            startEraseAll();
        else if (ok)
            QMessageBox::information(this, tr("Erase entire storage"), tr("Nothing was erased."));
    }

    void chooseIdentity()
    {
        QDialog d(this);
        d.setWindowTitle(tr("Serial number and MAC address"));
        auto *v = new QVBoxLayout(&d);
        auto *note = new QLabel(tr("These identify the board to the printer's software, your network and Creality Cloud. Use the values printed on the "
                                   "board's label where you can. Two devices with the same MAC address conflict on a network."));
        note->setWordWrap(true);
        note->setMinimumWidth(480);
        v->addWidget(note);
        auto *form = new QFormLayout;
        const QString dash = QStringLiteral("—");
        auto *sn = new QLineEdit(serial->text() == dash ? QString() : serial->text());
        auto *mc = new QLineEdit(mac->text() == dash ? QString() : mac->text());
        sn->setMaxLength(14);
        mc->setMaxLength(17);
        auto *mdl = new QComboBox;
        mdl->setEditable(true);
        const QList<QPair<QString, QString>> known = {
            {"Nebula Pad", tr("Retail Nebula Pad")}, {"F005", tr("Ender-3 V3 KE")}, {"F003", tr("CR-10 SE")}, {"F004", tr("Ender-5 Max")},
            {"CR-K1", tr("K1")}, {"CR-K1 Max", tr("K1 Max")}, {"K1C", tr("K1C")}, {"K1 SE", tr("K1 SE")},
            {"F001", tr("Ender-3 V3")}, {"F002", tr("Ender-3 V3 Plus")}};
        for (const auto &k : known)
            mdl->addItem(k.first == k.second ? k.first : QStringLiteral("%1 — %2").arg(k.first, k.second), k.first);
        int idx = mdl->findData(modelId);
        if (idx >= 0)
            mdl->setCurrentIndex(idx);
        else if (!modelId.isEmpty())
            mdl->setEditText(modelId);
        auto *brd = new QLineEdit(board->text() == dash ? QStringLiteral("NEBULA V1.0.0.1") : board->text());
        form->addRow(tr("Serial number (14 hex digits):"), sn);
        form->addRow(tr("MAC address (12 hex digits):"), mc);
        form->addRow(tr("Model:"), mdl);
        form->addRow(tr("Mainboard:"), brd);
        v->addLayout(form);
        auto *gen = new QPushButton(tr("Generate new random values"));
        auto *copy = new QPushButton(tr("Copy to clipboard"));
        copy->setToolTip(tr("Copy the four values as shown here, for your notes"));
        auto *brow = new QHBoxLayout;
        brow->addWidget(gen);
        brow->addWidget(copy);
        brow->addStretch(1);
        v->addLayout(brow);
        auto *modelNote = new QLabel(tr("The model must match the installed firmware, or the printer gets no configuration. To change printer type, use Install firmware instead."));
        modelNote->setWordWrap(true);
        v->addWidget(modelNote);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        bb->button(QDialogButtonBox::Ok)->setText(tr("Write"));
        v->addWidget(bb);
        auto modelValue = [mdl] {
            const int i = mdl->findText(mdl->currentText());
            return i >= 0 ? mdl->itemData(i).toString() : mdl->currentText().trimmed();
        };
        auto valid = [=] {
            static const QRegularExpression hs(QStringLiteral("^[0-9A-Fa-f]{14}$")), hm(QStringLiteral("^[0-9A-Fa-f]{12}$"));
            const QString m = QString(mc->text()).remove(':').remove('-');
            bb->button(QDialogButtonBox::Ok)->setEnabled(hs.match(sn->text().trimmed()).hasMatch() && hm.match(m.trimmed()).hasMatch()
                                                         && !modelValue().isEmpty() && !brd->text().trimmed().isEmpty());
        };
        for (QLineEdit *e : {sn, mc, brd})
            connect(e, &QLineEdit::textChanged, &d, valid);
        connect(mdl, &QComboBox::currentTextChanged, &d, valid);
        connect(copy, &QPushButton::clicked, &d, [=] {
            QApplication::clipboard()->setText(tr("Serial number: %1\nMAC address: %2\nModel: %3\nMainboard: %4")
                                                   .arg(sn->text().trimmed(), mc->text().trimmed(), modelValue(), brd->text().trimmed()));
            copy->setText(tr("Copied"));
            QTimer::singleShot(1500, copy, [copy] { copy->setText(tr("Copy to clipboard")); });
        });
        connect(gen, &QPushButton::clicked, &d, [=] {
            const QJsonObject o = takeJson(nocry_generate_identity_json());
            if (o.value("ok").toBool()) {
                sn->setText(o.value("sn").toString());
                mc->setText(o.value("mac").toString());
            }
        });
        valid();
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        if (d.exec() != QDialog::Accepted)
            return;
        const QString nsn = sn->text().trimmed().toUpper(), nmac = QString(mc->text()).remove(':').remove('-').trimmed().toUpper();
        QString changes;
        if (serial->text() != nsn) changes += tr("Serial number: %1 → %2\n").arg(serial->text(), nsn);
        if (mac->text() != nmac) changes += tr("MAC address: %1 → %2\n").arg(mac->text(), nmac);
        if (modelId != modelValue()) changes += tr("Model: %1 → %2\n").arg(modelId.isEmpty() ? dash : modelId, modelValue());
        if (board->text() != brd->text().trimmed()) changes += tr("Mainboard: %1 → %2\n").arg(board->text(), brd->text().trimmed());
        if (changes.isEmpty()) {
            QMessageBox::information(this, tr("Serial number and MAC address"), tr("Nothing was changed."));
            return;
        }
        if (confirmDanger(tr("Serial number and MAC address"), tr("Write this identity to the board?"), changes.trimmed(), tr("Write identity")))
            startIdentity(nsn, nmac, modelValue(), brd->text().trimmed());
    }

    void chooseReset()
    {
        QMessageBox box(QMessageBox::Warning, tr("Factory reset"), tr("Reset this board to factory settings?"), QMessageBox::Cancel, this);
        box.setInformativeText(tr("All settings, calibration, Wi-Fi details and files on the printer will be erased. The printer sets itself up again the next time it starts.\n\n"
                                  "The installed firmware, both firmware slots, the serial number and the MAC address are kept."));
        // two options in one message box: a small widget holding both check boxes
        auto *keep = new QCheckBox(tr("Keep the user storage partition (advanced)"));
        auto *secure = new QCheckBox(tr("Secure erase: also destroy the old data so it cannot be recovered (takes longer)"));
        connect(secure, &QCheckBox::toggled, keep, [keep](bool on) { keep->setEnabled(!on); if (on) keep->setChecked(false); });
        box.setCheckBox(secure);
        if (auto *grid = qobject_cast<QGridLayout *>(box.layout()))
            grid->addWidget(keep, grid->rowCount(), 0, 1, grid->columnCount());
        auto *go = box.addButton(tr("Erase and reset"), QMessageBox::DestructiveRole);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() != go)
            return;
        if (secure->isChecked())
            startSecureWipe();
        else
            startReset(keep->isChecked());
    }

    QByteArray dataDir() const { return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation).toUtf8(); }

    // The firmware list this board's current model belongs to ("NEBULA", "F005", "CR4CU220812S11"...).
    QString deviceModelKey() const
    {
        if (modelId == "Nebula Pad")
            return QStringLiteral("NEBULA");
        if (modelId == "F001" || modelId == "F002")
            return QStringLiteral("F001");
        if (modelId.startsWith("CR-K1") || modelId.startsWith("K1") || modelId.startsWith("K2 SE") || modelId.startsWith("GS-"))
            return QStringLiteral("CR4CU220812S11");      // one firmware line for the K1 family
        return modelId;
    }

    // "Which printer is this board in?" / "Which mainboard is it?" Returns the chosen value, "" if cancelled.
    QString askBoardChoice(const QString &what, const QJsonArray &choices)
    {
        const QString preset = what == "model" ? autoModel : autoBoard;
        if (!preset.isEmpty())
            return preset;
        if (quiet || choices.isEmpty())
            return QString();
        QDialog d(this);
        d.setWindowTitle(what == "model" ? tr("Printer type") : tr("Mainboard"));
        auto *v = new QVBoxLayout(&d);
        auto *l = new QLabel(what == "model"
            ? tr("This firmware is for several printers, and the board does not say which one it is in.\n\nWhich printer is this board for? The firmware loads that printer's settings; with the wrong choice the printer will not work properly.")
            : tr("The board does not say which mainboard version it is.\n\nThe version is printed on the board (for example CR4CU220812S11). Which one is it?"));
        l->setWordWrap(true);
        l->setMinimumWidth(420);
        v->addWidget(l);
        auto *box = new QComboBox;
        for (const auto &c : choices) {
            const QJsonObject o = c.toObject();
            const QString val = o.value("value").toString(), name = o.value("name").toString();
            box->addItem(val == name ? val : QStringLiteral("%1  (%2)").arg(name, val), val);
        }
        box->setCurrentIndex(-1);
        v->addWidget(box);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        bb->button(QDialogButtonBox::Ok)->setEnabled(false);
        connect(box, &QComboBox::currentIndexChanged, &d, [bb](int i) { bb->button(QDialogButtonBox::Ok)->setEnabled(i >= 0); });
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        v->addWidget(bb);
        return d.exec() == QDialog::Accepted ? box->currentData().toString() : QString();
    }

    void chooseInstall()
    {
        QDialog d(this);
        d.setWindowTitle(tr("Install firmware"));
        auto *v = new QVBoxLayout(&d);

        // --- where the firmware comes from
        auto *fromList = new QRadioButton(tr("Download"));
        auto *fromFile = new QRadioButton(tr("Use a file on this computer"));
        auto *srcGroup = new QButtonGroup(&d);
        srcGroup->addButton(fromList);
        srcGroup->addButton(fromFile);
        fromList->setChecked(true);

        auto *listBox = new QWidget;
        auto *lf = new QFormLayout(listBox);
        lf->setContentsMargins(24, 0, 0, 0);
        auto *modelBox = new QComboBox;
        auto *versionBox = new QComboBox;
        auto *notes = new QLabel;
        notes->setWordWrap(true);
        notes->setTextFormat(Qt::PlainText);
        notes->setMinimumWidth(460);
        auto *hint = new QLabel;
        hint->setWordWrap(true);
        auto *listInfo = new QLabel;
        auto *refresh = new QPushButton(tr("Check for new versions"));
        auto *rrow = new QHBoxLayout;
        rrow->addWidget(listInfo, 1);
        rrow->addWidget(refresh);
        lf->addRow(tr("Printer:"), modelBox);
        lf->addRow(tr("Version:"), versionBox);
        lf->addRow(QString(), notes);
        lf->addRow(QString(), hint);
        lf->addRow(QString(), rrow);

        auto *fileBox = new QWidget;
        auto *fh = new QHBoxLayout(fileBox);
        fh->setContentsMargins(24, 0, 0, 0);
        auto *path = new QLineEdit;
        path->setPlaceholderText(tr("Creality update file (.img), .ingenic package, or unpacked update folder"));
        auto *file = new QPushButton(tr("Choose file…"));
        auto *folder = new QPushButton(tr("Choose folder…"));
        fh->addWidget(path, 1);
        fh->addWidget(file);
        fh->addWidget(folder);

        v->addWidget(fromList);
        v->addWidget(listBox);
        v->addWidget(fromFile);
        v->addWidget(fileBox);

        // --- how to install
        auto *clean = new QRadioButton(tr("Clean install (Recommended) — erase settings and files, like a new printer"));
        auto *keep = new QRadioButton(tr("Keep my data — repair or update the system; settings and files stay"));
        auto *modeGroup = new QButtonGroup(&d);
        modeGroup->addButton(clean);
        auto *other = new QRadioButton(tr("Install to the other slot — keep the current firmware as it is, add this one beside it"));
        modeGroup->addButton(keep);
        modeGroup->addButton(other);
        clean->setChecked(true);
        v->addSpacing(8);
        v->addWidget(clean);
        v->addWidget(keep);
        v->addWidget(other);
        v->addSpacing(8);
        auto *pic = new QCheckBox(tr("Show the nocry boot picture when the printer powers on (untick to keep the Creality logo)"));
        pic->setToolTip(tr("The picture of the first seconds after power-on comes from a small program stored with the firmware. nocry can put its own open one there.\n"
                           "Creality's is kept on the board and can be brought back at any time (Advanced, Boot picture).\n"
                           "On K1-family boards Creality's stays for now: nocry's has not been tested on their screens yet."));
        pic->setChecked(true);
        v->addWidget(pic);
        v->addWidget(new QLabel(tr("Nothing is written until you confirm on the next screen.")));
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        bb->button(QDialogButtonBox::Ok)->setText(tr("Continue"));
        v->addWidget(bb);

        QJsonArray models;
        const QString mine = deviceModelKey();
        auto fillVersions = [&] {
            versionBox->clear();
            const QJsonObject m = models.at(modelBox->currentIndex()).toObject();
            for (const auto &fv : m.value("firmwares").toArray()) {
                const QJsonObject f = fv.toObject();
                QString t = tr("%1 — released %2, %3").arg(f.value("version").toString(), f.value("released").toString(),
                                                           QLocale().formattedDataSize(qint64(f.value("size").toDouble())));
                if (f.value("downloaded").toBool())
                    t += tr(" (already downloaded)");
                versionBox->addItem(t);
            }
            const QString key = m.value("key").toString();
            if (key == "NEBULAOS")
                // one image for both printers; the board's printer type is left as it is
                hint->setText(mine.isEmpty() || mine == "F005" || mine == "NEBULA"
                    ? tr("NebulaOS is an independent open-source firmware, not made by Creality, for the Ender-3 V3 KE and the retail Nebula Pad. "
                         "It uses its own firmware on the printer's mainboard: read its release notes before switching between it and Creality's firmware. "
                         "The board's printer type is not changed.")
                    : tr("NebulaOS is made for the Ender-3 V3 KE and the retail Nebula Pad only. nocry will not install it on this board."));
            else
                hint->setText(!mine.isEmpty() && key != mine
                    ? tr("This is firmware for a different printer than this board is set up for. Installing it converts the board.")
                    : QString());
            hint->setVisible(!hint->text().isEmpty());
        };
        auto showNotes = [&] {
            const QJsonObject f = models.at(modelBox->currentIndex()).toObject().value("firmwares").toArray().at(versionBox->currentIndex()).toObject();
            QStringList n;
            for (const auto &x : f.value("notes").toArray())
                n << x.toString().simplified();
            QString t = n.join(' ');
            if (t.size() > 320)
                t = t.left(320) + QStringLiteral("…");
            notes->setText(t);
            notes->setVisible(!t.isEmpty());
        };
        auto fillModels = [&](const QJsonObject &cat) {
            models = cat.value("models").toArray();
            modelBox->clear();
            int sel = 0;
            for (int i = 0; i < models.size(); i++) {
                const QJsonObject m = models[i].toObject();
                const bool isMine = m.value("key").toString() == mine;
                modelBox->addItem(isMine ? tr("%1 (this board)").arg(m.value("name").toString()) : m.value("name").toString());
                if (isMine)
                    sel = i;
            }
            // self-tests: show another list than the board's own
            const QByteArray wantKey = qgetenv("NOCRY_TEST_LIST_KEY");
            for (int i = 0; !wantKey.isEmpty() && i < models.size(); i++)
                if (models[i].toObject().value("key").toString() == QString::fromUtf8(wantKey))
                    sel = i;
            modelBox->setCurrentIndex(sel);
            listInfo->setText(cat.value("updated").toBool() ? tr("List updated %1").arg(cat.value("built").toString())
                                                            : tr("List from %1").arg(cat.value("built").toString()));
        };
        auto update = [&] {
            const bool lst = fromList->isChecked();
            // NebulaOS starts through its own program; nocry's boot picture is not used with it
            const bool nebulaos = lst && modelBox->currentIndex() >= 0 && models.at(modelBox->currentIndex()).toObject().value("key").toString() == "NEBULAOS";
            pic->setEnabled(!nebulaos);
            pic->setToolTip(nebulaos ? tr("NebulaOS shows its own boot picture.") : QString());
            listBox->setEnabled(lst);
            fileBox->setEnabled(!lst);
            bb->button(QDialogButtonBox::Ok)->setEnabled(lst ? versionBox->count() > 0 : QFileInfo::exists(path->text().trimmed()));
        };
        connect(modelBox, &QComboBox::currentIndexChanged, &d, [&] { fillVersions(); showNotes(); update(); });
        connect(versionBox, &QComboBox::currentIndexChanged, &d, [&] { showNotes(); });
        connect(fromList, &QRadioButton::toggled, &d, [&] { update(); });
        connect(path, &QLineEdit::textChanged, &d, [&] { update(); });
        const QByteArray dir = dataDir();
        fillModels(takeJson(nocry_catalog_json(dir.constData(), 0)));
        update();
        connect(refresh, &QPushButton::clicked, &d, [&] {
            refresh->setEnabled(false);
            listInfo->setText(tr("Checking for new versions…"));
            auto *w = new QFutureWatcher<QJsonObject>(&d);
            connect(w, &QFutureWatcher<QJsonObject>::finished, &d, [&, w] {
                const QJsonObject o = w->result();
                w->deleteLater();
                refresh->setEnabled(true);
                if (o.value("ok").toBool()) {
                    fillModels(o);
                } else {
                    fillModels(takeJson(nocry_catalog_json(dir.constData(), 0)));
                    QMessageBox::warning(&d, tr("Check for new versions"), tr("Could not update the list: %1\n\nThe list already in the program is still usable.").arg(o.value("error").toString()));
                }
                update();
            });
            w->setFuture(QtConcurrent::run([dir] { return takeJson(nocry_catalog_json(dir.constData(), 1)); }));
        });
        connect(file, &QPushButton::clicked, &d, [&] {
            const QString f = QFileDialog::getOpenFileName(&d, tr("Choose firmware"), QString(), tr("Firmware (*.img *.ingenic);;All files (*)"));
            if (!f.isEmpty())
                path->setText(f);
        });
        connect(folder, &QPushButton::clicked, &d, [&] {
            const QString f = QFileDialog::getExistingDirectory(&d, tr("Choose unpacked update folder"));
            if (!f.isEmpty())
                path->setText(f);
        });
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        if (d.exec() != QDialog::Accepted)
            return;
        pendingPicture = pic->isChecked() ? 0 : 2;     // 0: nocry's where proven on the board family; 2: the firmware's own
        if (fromFile->isChecked()) {
            pendingModel.clear();
            pendingBoard.clear();
            startInstall(path->text().trimmed(), other->isChecked() ? 2 : keep->isChecked() ? 1 : 0, false);
        } else {
            const QJsonObject f = models.at(modelBox->currentIndex()).toObject().value("firmwares").toArray().at(versionBox->currentIndex()).toObject();
            pendingModel.clear();
            pendingBoard.clear();
            startDownload(f, other->isChecked() ? 2 : keep->isChecked() ? 1 : 0);
        }
    }

    void chooseBackup()
    {
        QString name = QStringLiteral("nocry-backup");
        if (model->text() != QStringLiteral("—") && serial->text().size() >= 4)
            name += QStringLiteral("-%1-%2").arg(modelId.simplified().replace(' ', '_'), serial->text().right(4));
        name += QStringLiteral("-%1.img").arg(QDate::currentDate().toString("yyyyMMdd"));
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        const QString path = QFileDialog::getSaveFileName(this, tr("Save backup as"), QDir(dir).filePath(name), tr("Disk images (*.img);;All files (*)"));
        if (path.isEmpty())
            return;
        const qint64 need = emmcBytes > 0 ? emmcBytes : (8LL << 30);
        const QStorageInfo st(QFileInfo(path).absolutePath());
        if (st.isValid() && st.bytesAvailable() < need) {
            QMessageBox::warning(this, tr("Not enough space"),
                tr("A full backup needs about %1, but only %2 is free in that location.")
                    .arg(QLocale().formattedDataSize(need), QLocale().formattedDataSize(st.bytesAvailable())));
            return;
        }
        startBackup(path);
    }

    static void onEvent(int kind, const char *text, uint64_t done, uint64_t total, const char *note, void *user)
    {
        auto *self = static_cast<Window *>(user);
        QString t = QString::fromUtf8(text), n = QString::fromUtf8(note);
        QMetaObject::invokeMethod(self, [=] { self->handleEvent(kind, t, done, total, n); }, Qt::QueuedConnection);
    }

    void handleEvent(int kind, const QString &text, quint64 done, quint64 total, const QString &note)
    {
        if (kind == NOCRY_EVENT_LOG) {
            log->appendPlainText(text);
        } else if (kind == NOCRY_EVENT_PROGRESS) {
            if (!busy)
                return;     // a late report from an operation that has ended
            progress->setVisible(true);
            progress->setRange(0, 1000);
            progress->setValue(total ? int(done * 1000 / total) : 0);
            QString what = text;
            if (text == "checking")
                what = tr("Checking");
            else if (text == "erasing")
                what = tr("Erasing");
            else if (text == "verifying")
                what = tr("Verifying");
            const bool counted = text == "checking" || text == "erasing" || text == "verifying";
            if (counted) {
                progressLabel->setText(what);
                return;
            }
            if (text == "downloading")
                what = tr("Downloading");
            if (text == "reimage")
                what = tr("Restoring");
            else if (text == "image")
                what = tr("Writing");
            else if (text == "checking the backup file")
                what = tr("Checking the backup file");
            if (text == "backup")
                what = tr("Reading the board");
            else if (text.startsWith("verify"))
                what = tr("Verifying");
            progressLabel->setText(QStringLiteral("%1 — %2 of %3   %4").arg(what, QLocale().formattedDataSize(qint64(done)), QLocale().formattedDataSize(qint64(total)), note.simplified()));
        } else if (busy) {
            // between two steps: back to the moving bar
            progress->setRange(0, 0);
            progressLabel->setText(tr("Working…"));
        } else {
            progress->setVisible(false);
            progressLabel->clear();
        }
    }

    // Offer Cancel for the operation under way. `risk` says what stopping half-way leaves behind;
    // when it is not empty the user has to confirm it first. Operations that take seconds and
    // must not be cut short (boot loader, boot picture, identity) offer no Cancel at all.
    void allowCancel(const QString &risk = QString())
    {
        cancelRisk = risk;
        cancelButton->setEnabled(true);
        cancelButton->setVisible(true);
    }

    void requestCancel()
    {
        if (!busy)
            return;
        if (!cancelRisk.isEmpty() && !quiet) {
            QMessageBox box(QMessageBox::Warning, tr("Stop now?"), tr("Stop before this is finished?"), QMessageBox::NoButton, this);
            box.setInformativeText(cancelRisk);
            auto *stop = box.addButton(tr("Stop anyway"), QMessageBox::DestructiveRole);
            auto *go = box.addButton(tr("Keep going"), QMessageBox::RejectRole);
            box.setDefaultButton(go);
            box.setEscapeButton(go);
            box.exec();
            // the work went on while the question was open and may have finished
            if (box.clickedButton() != stop || !busy)
                return;
        }
        cancelButton->setEnabled(false);
        stateHint->setText(tr("Stopping at the next safe point…"));
        log->appendPlainText(tr("Stop requested."));
        nocry_cancel();
    }

    static bool wasCancelled(const QJsonObject &o)
    {
        return o.value("cancelled").toBool() || o.value("error").toString().contains(QLatin1String("cancelled"));
    }

    void setBusy(bool b)
    {
        busy = b;
        // something is always shown while work is under way: a moving bar until the operation
        // reports how far it is, and again between its steps
        progress->setVisible(b);
        if (b) {
            progress->setRange(0, 0);
            progressLabel->setText(tr("Working…"));
        } else {
            progressLabel->clear();
        }
        cancelButton->setVisible(false);
        cancelRisk.clear();
        readButton->setEnabled(!b && canRead);
        backupButton->setEnabled(!b && canRead);
        installButton->setEnabled(!b && canRead);
        switchButton->setEnabled(!b && canRead);
        resetButton->setEnabled(!b && canRead);
        restoreButton->setEnabled(!b && canRead);
        updateButtons();
        if (b) {
            stateTitle->setText(tr("Working…"));
            stateHint->setText(tr("Leave the board connected."));
        }
    }

    void pollState()
    {
        if (busy || polling)
            return;
        polling = true;
        auto *w = new QFutureWatcher<QJsonObject>(this);
        connect(w, &QFutureWatcher<QJsonObject>::finished, this, [this, w] {
            polling = false;
            if (!busy)
                showState(w->result());
            w->deleteLater();
        });
        w->setFuture(QtConcurrent::run([] { return takeJson(nocry_board_state_json()); }));
    }

    void showState(const QJsonObject &o)
    {
        const QString s = o.value("state").toString();
        const bool net = s == "network", netUp = net && o.value("reachable").toBool();
        canRead = s != "none" && s != "error" && s != "uboot-disk" && (!net || netUp);
        if (net)
            networkAddress = o.value("address").toString();
        netButton->setVisible(s == "none" || net);
        wifiButton->setVisible(netUp || s == "recovery");
        wifiButton->setEnabled(!busy);
        netButton->setText(net ? tr("Disconnect") : tr("Connect over the network…"));
        if (s == "none" && infoLoaded) {
            for (auto *l : {model, serial, mac, board, slot, otherSlot, storage})
                l->setText(QStringLiteral("—"));
            parts->setRowCount(0);
            infoLoaded = false;
            modelId.clear();
        }
        if (s == "none")
            autoBlocked = false;
        bootHelpButton->setVisible(s == "none");
        recoveryRunning = s == "recovery";
        if (!recoveryRunning && !busy) {
            shellLine->setVisible(false);
            shellButton->setVisible(false);
        }
        // fill in the details by itself when a board shows up or after something changed it
        if (canRead && (!infoLoaded || infoStale) && !autoBlocked && !busy && !quiet && !QApplication::activeModalWidget()) {
            readDevice();
            return;
        }
        if (s == "none") {
            stateTitle->setText(tr("No board connected"));
            stateHint->setText(tr("Connect the board by USB, then hold BOOT, tap RESET and release BOOT."));
        } else if (net && netUp) {
            stateTitle->setText(tr("Board connected over the network"));
            stateHint->setText(tr("Rescue system at %1. Nothing on the board is changed until you choose an action.").arg(networkAddress));
        } else if (net) {
            stateTitle->setText(tr("The board is not answering"));
            stateHint->setText(tr("Nothing at %1 answers. Check that the board still shows its rescue screen and that this computer is on the same network; nocry keeps trying. "
                                  "A board that was restarted shows a new pairing code: disconnect and connect again.").arg(networkAddress));
        } else if (s == "usb-boot") {
            stateTitle->setText(tr("Board in USB boot mode"));
            stateHint->setText(tr("Ready. Nothing on the board is changed until you choose an action."));
        } else if (s == "fastboot") {
            stateTitle->setText(tr("Board connected (loader running)"));
            stateHint->setText(tr("Ready."));
        } else if (s == "recovery") {
            stateTitle->setText(tr("Board connected"));
            stateHint->setText(tr("The recovery system is running from memory."));
        } else if (s == "unsupported") {
            stateTitle->setText(tr("This board is not supported"));
            stateHint->setText(tr("An Ingenic board in USB boot mode is connected, but it uses a different processor than the X2000 family nocry works with. "
                                  "Newer boards such as the 2025 K1C (X2600E) also use secure boot and only run software signed by Creality, so nocry cannot load onto them. Nothing was sent to the board."));
        } else if (s == "uboot-disk") {
            stateTitle->setText(tr("Board in USB disk mode"));
            stateHint->setText(tr("Started by the command-line tool. To use this window, hold BOOT, tap RESET and release BOOT."));
        } else {
            stateTitle->setText(tr("USB problem"));
            stateHint->setText(o.value("error").toString());
        }
        readButton->setEnabled(!busy && canRead);
        backupButton->setEnabled(!busy && canRead);
        installButton->setEnabled(!busy && canRead);
        switchButton->setEnabled(!busy && canRead);
        resetButton->setEnabled(!busy && canRead);
        restoreButton->setEnabled(!busy && canRead);
        updateButtons();
    }

    // Ask (off the UI thread) whether adb sees the board and where its serial console is.
    void refreshShell(const QString &sn)
    {
        const QByteArray serialNo = sn.toUtf8();
        auto *w = new QFutureWatcher<QJsonObject>(this);
        connect(w, &QFutureWatcher<QJsonObject>::finished, this, [this, w] {
            shell = w->result();
            w->deleteLater();
            const QString port = shell.value("console_port").toString();
            QString adb;
            if (shell.value("adb_ready").toBool())
                adb = tr("ADB: ready");
            else if (!shell.value("adb_installed").toBool())
                adb = tr("ADB: the adb tool is not installed on this computer");
            else if (!shell.value("adb_state").toString().isEmpty())
                adb = tr("ADB: board seen, but %1").arg(shell.value("adb_state").toString());
            else
                adb = tr("ADB: not connected");
            const QString con = port.isEmpty() ? tr("Serial console: not found") : tr("Serial console: %1").arg(port);
            shellLine->setText(adb + QStringLiteral("   ·   ") + con);
            shellLine->setVisible(recoveryRunning);
            shellButton->setVisible(recoveryRunning);
        });
        w->setFuture(QtConcurrent::run([serialNo] { return takeJson(nocry_shell_status_json(serialNo.constData())); }));
    }

    void showBoardLog()
    {
        if (busy)
            return;
        setBusy(true);
        auto *w = new QFutureWatcher<QJsonObject>(this);
        connect(w, &QFutureWatcher<QJsonObject>::finished, this, [this, w] {
            const QJsonObject o = w->result();
            w->deleteLater();
            setBusy(false);
            pollState();
            lastResult = o;
            if (!o.value("ok").toBool()) {
                log->appendPlainText(tr("Board log: %1").arg(o.value("error").toString()));
                if (!quiet)
                    QMessageBox::information(this, tr("Board log"), o.value("error").toString());
            } else if (!quiet) {
                QDialog d(this);
                d.setWindowTitle(tr("Board log"));
                auto *v = new QVBoxLayout(&d);
                v->addWidget(new QLabel(o.value("kind").toString() == "previous"
                    ? tr("Log left in memory by the previous start of the recovery system (the board was reset since):")
                    : tr("Kernel log of the recovery system running on the board now:")));
                auto *t = new QPlainTextEdit(o.value("text").toString());
                t->setReadOnly(true);
                t->setLineWrapMode(QPlainTextEdit::NoWrap);
                QFont f = t->font();
                f.setFamily(QStringLiteral("monospace"));
                f.setStyleHint(QFont::Monospace);
                t->setFont(f);
                v->addWidget(t, 1);
                auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
                auto *save = bb->addButton(tr("Save…"), QDialogButtonBox::ActionRole);
                auto *copy = bb->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
                connect(copy, &QPushButton::clicked, &d, [t] { QApplication::clipboard()->setText(t->toPlainText()); });
                connect(save, &QPushButton::clicked, &d, [&d, t, this] {
                    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
                    const QString p = QFileDialog::getSaveFileName(&d, tr("Save board log"), QDir(dir).filePath(QStringLiteral("nocry-board-log.txt")), tr("Text files (*.txt)"));
                    if (p.isEmpty())
                        return;
                    QFile f(p);
                    if (f.open(QIODevice::WriteOnly | QIODevice::Text))
                        f.write(t->toPlainText().toUtf8());
                    else
                        QMessageBox::warning(&d, tr("Board log"), tr("Could not write %1").arg(p));
                    Q_UNUSED(this);
                });
                connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
                v->addWidget(bb);
                d.resize(820, 560);
                d.exec();
            }
            if (onIdle)
                onIdle();
        });
        w->setFuture(QtConcurrent::run([] { return takeJson(nocry_boot_log_json()); }));
    }

    void showShellHelp()
    {
        const QString sn = shell.value("adb_serial").toString().isEmpty() ? serial->text() : shell.value("adb_serial").toString();
        const QString port = shell.value("console_port").toString();
        const QString adbCmd = QStringLiteral("adb -s %1 shell").arg(sn);
#if defined(Q_OS_WIN)
        const QString conCmd = port.isEmpty() ? QString() : tr("PuTTY: connection type Serial, line %1, speed 115200").arg(port);
        const QString adbInstall = tr("Download \"SDK Platform-Tools for Windows\" from developer.android.com, unzip it, and run adb.exe from that folder.");
        const QString conTool = tr("Use a serial terminal such as PuTTY.");
#elif defined(Q_OS_MACOS)
        const QString conCmd = port.isEmpty() ? QString() : QStringLiteral("screen %1 115200").arg(port);
        const QString adbInstall = tr("Install it with Homebrew: brew install android-platform-tools");
        const QString conTool = tr("screen is part of macOS. To leave it: Ctrl-A, then K, then Y.");
#else
        const QString conCmd = port.isEmpty() ? QString() : QStringLiteral("picocom %1").arg(port);
        const QString adbInstall = tr("Install your distribution's package: android-tools (Arch, Fedora) or adb (Debian, Ubuntu).");
        const QString conTool = tr("picocom is in every distribution's packages (screen %1 115200 also works). To leave picocom: Ctrl-A, then Ctrl-X.").arg(port.isEmpty() ? QStringLiteral("<port>") : port);
#endif
        QDialog d(this);
        d.setWindowTitle(tr("Getting a shell on the board"));
        auto *v = new QVBoxLayout(&d);
        auto para = [&](const QString &t) {
            auto *l = new QLabel(t);
            l->setWordWrap(true);
            l->setMinimumWidth(520);
            v->addWidget(l);
        };
        auto command = [&](const QString &c) {
            auto *row = new QHBoxLayout;
            auto *e = new QLineEdit(c);
            e->setReadOnly(true);
            QFont f = e->font();
            f.setFamily(QStringLiteral("monospace"));
            f.setStyleHint(QFont::Monospace);
            e->setFont(f);
            auto *b = new QPushButton(tr("Copy"));
            connect(b, &QPushButton::clicked, &d, [c] { QApplication::clipboard()->setText(c); });
            row->addWidget(e, 1);
            row->addWidget(b);
            v->addLayout(row);
        };
        para(tr("While the recovery system runs, the board offers a root shell in two ways. It runs from memory: "
                "nothing you do in it changes the printer unless you write to its storage (/dev/mmcblk0)."));
        para(tr("<b>ADB</b> — the easier one, with file copy (adb push, adb pull)."));
        if (shell.value("adb_ready").toBool()) {
            para(tr("Ready. In a terminal, run:"));
            command(adbCmd);
        } else if (!shell.value("adb_installed").toBool()) {
            para(tr("The adb tool is not installed on this computer. %1 Then press Refresh in the main window and run:").arg(adbInstall));
            command(adbCmd);
        } else {
            para(tr("adb is installed but does not list the board as ready%1. Unplug and reconnect the board, or press Refresh. The command will be:")
                     .arg(shell.value("adb_state").toString().isEmpty() ? QString() : tr(" (it says \"%1\")").arg(shell.value("adb_state").toString())));
            command(adbCmd);
        }
        para(tr("<b>USB serial console</b> — works without adb."));
        if (port.isEmpty()) {
            para(tr("No serial port from the board was found on this computer."));
        } else {
            para(tr("The board's console is on %1. %2").arg(port, conTool));
            command(conCmd);
            para(tr("Press Enter once to get the prompt."));
        }
        para(tr("Only one program can use the board at a time for nocry's own actions, but a shell can stay open while you use this window."));
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        v->addWidget(bb);
        d.exec();
    }

#ifdef Q_OS_WIN
    // Shows the setup banner unless nocry's driver package is in Windows' driver store.
    void checkWindowsDriver()
    {
        auto *p = new QProcess(this);
        connect(p, &QProcess::finished, this, [this, p](int, QProcess::ExitStatus) {
            const bool have = p->readAllStandardOutput().contains("nocry.inf");
            p->deleteLater();
            udevBanner->setVisible(!have);
        });
        connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) {
                p->deleteLater();
                udevBanner->setVisible(true);
            }
        });
        p->start(QStringLiteral("pnputil.exe"), {QStringLiteral("/enum-drivers")});
    }

    // Runs the driver setup script with administrator rights (one Windows prompt).
    void installWindowsDriver()
    {
        QMessageBox ask(QMessageBox::Question, tr("Set up driver"), tr("Set up the USB driver for the board?"), QMessageBox::Ok | QMessageBox::Cancel, this);
        ask.setInformativeText(tr("Windows will ask for administrator permission once.\n\n"
                                  "No driver software is added: the board is assigned to WinUSB, a driver that is part of Windows. "
                                  "Windows only accepts that from a trusted publisher, so the setup creates a certificate on this computer, "
                                  "uses it once, marks it as trusted and then destroys its signing key so it cannot be used for anything else.\n\n"
                                  "This also lets the adb and fastboot tools reach the board."));
        if (ask.exec() != QMessageBox::Ok)
            return;
        const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("nocry-driver-setup"));
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        for (const char *n : {"nocry.inf", "install-driver.ps1"}) {
            QFile in(QStringLiteral(":/windows/%1").arg(n)), out(QDir(dir).filePath(n));
            if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly) || out.write(in.readAll()) <= 0) {
                QMessageBox::warning(this, tr("Set up driver"), tr("Could not write a temporary file."));
                return;
            }
        }
        QString script = QDir::toNativeSeparators(QDir(dir).filePath(QStringLiteral("install-driver.ps1")));
        const QString logFile = QDir(dir).filePath(QStringLiteral("install-driver.log"));
        script.replace(QLatin1Char('\''), QStringLiteral("''"));
        // an elevated PowerShell runs the script; this one waits for it and passes on its result
        const QString cmd = QStringLiteral("try { $p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -WindowStyle Hidden "
                                           "-ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%1\"'; exit $p.ExitCode } catch { exit 1223 }").arg(script);
        auto *proc = new QProcess(this);
        setBusy(true);
        connect(proc, &QProcess::finished, this, [this, proc, logFile](int code, QProcess::ExitStatus) {
            proc->deleteLater();
            setBusy(false);
            checkWindowsDriver();
            QString text;
            QFile f(logFile);
            if (f.open(QIODevice::ReadOnly))
                text = QString::fromLocal8Bit(f.readAll()).trimmed();
            log->appendPlainText(tr("Driver setup:\n%1").arg(text));
            if (code == 0) {
                QMessageBox::information(this, tr("Set up driver"), tr("The driver is set up. If the board is connected, unplug it and plug it in again."));
            } else if (code == 1223) {
                QMessageBox::information(this, tr("Set up driver"), tr("The administrator prompt was declined; nothing was changed."));
            } else {
                QMessageBox box(QMessageBox::Warning, tr("Set up driver"), tr("The driver could not be set up."), QMessageBox::Close, this);
                box.setInformativeText(tr("Details are in the Activity log tab."));
                box.setDetailedText(text);
                box.exec();
            }
        });
        proc->start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-Command"), cmd});
    }
#endif

    static bool udevRuleInstalled()
    {
        for (const char *d : {"/etc/udev/rules.d", "/usr/lib/udev/rules.d", "/lib/udev/rules.d", "/usr/local/lib/udev/rules.d"})
            if (QFile::exists(QStringLiteral("%1/60-nocry.rules").arg(d)))
                return true;
        return false;
    }

    // Installs the udev rule with one password prompt (polkit). Falls back to showing the commands.
    void installUdevRule()
    {
        QFile res(QStringLiteral(":/60-nocry.rules"));
        if (!res.open(QIODevice::ReadOnly))
            return;
        const QString tmp = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("60-nocry.rules"));
        QFile out(tmp);
        if (!out.open(QIODevice::WriteOnly) || out.write(res.readAll()) <= 0) {
            QMessageBox::warning(this, tr("Install rule"), tr("Could not write a temporary file."));
            return;
        }
        out.close();
        const QString script = QStringLiteral("install -m 644 '%1' /etc/udev/rules.d/60-nocry.rules && udevadm control --reload && udevadm trigger --action=add --subsystem-match=usb").arg(tmp);
        const QString manual = QStringLiteral("sudo sh -c \"%1\"").arg(script);
        const QString pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
        auto showManual = [this, manual](const QString &why) {
            QMessageBox box(QMessageBox::Information, tr("Install rule"), why, QMessageBox::Close, this);
            box.setInformativeText(tr("Run this in a terminal, then unplug and reconnect the board:\n\n%1").arg(manual));
            auto *copy = box.addButton(tr("Copy command"), QMessageBox::ActionRole);
            box.exec();
            if (box.clickedButton() == copy)
                QApplication::clipboard()->setText(manual);
            if (udevBanner)
                udevBanner->setVisible(!udevRuleInstalled());
        };
        if (pkexec.isEmpty()) {
            showManual(tr("This system has no graphical password prompt (pkexec)."));
            return;
        }
        if (QMessageBox::question(this, tr("Install rule"),
                tr("nocry will install the file /etc/udev/rules.d/60-nocry.rules. It lets the logged-in user open the board's USB devices and stops the desktop from mounting the board's storage.\n\nYou will be asked for your password once."),
                QMessageBox::Ok | QMessageBox::Cancel) != QMessageBox::Ok)
            return;
        auto *proc = new QProcess(this);
        connect(proc, &QProcess::finished, this, [this, proc, showManual](int code, QProcess::ExitStatus) {
            proc->deleteLater();
            if (code == 0 && udevRuleInstalled()) {
                udevBanner->setVisible(false);
                QMessageBox::information(this, tr("Install rule"), tr("The rule is installed. If the board is connected, unplug it and plug it in again."));
            } else {
                showManual(tr("The rule was not installed (the password prompt was cancelled or failed)."));
            }
        });
        proc->start(pkexec, {QStringLiteral("sh"), QStringLiteral("-c"), script});
    }

    // Step-by-step help for getting a board into USB boot mode, with a picture where we have one.
    void showBootHelp()
    {
        QDialog d(this);
        d.setWindowTitle(tr("Entering USB boot mode"));
        auto *v = new QVBoxLayout(&d);
        auto *which = new QComboBox;
        which->addItem(tr("Nebula Pad (retail, Ender-3 V3 KE, CR-10 SE, Ender-5 Max)"), QStringLiteral("nebula"));
        which->addItem(tr("K1 family mainboard (K1, K1 Max, K1C, K1 SE, Ender-3 V3)"), QStringLiteral("k1"));
        v->addWidget(which);
        auto *pic = new QLabel;
        pic->setAlignment(Qt::AlignCenter);
        v->addWidget(pic);
        auto *steps = new QLabel;
        steps->setWordWrap(true);
        steps->setMinimumWidth(520);
        steps->setTextFormat(Qt::RichText);
        v->addWidget(steps);
        auto fill = [=] {
            const QString k = which->currentData().toString();
            const QPixmap p(QStringLiteral(":/boards/%1.png").arg(k));
            pic->setPixmap(p);
            pic->setVisible(!p.isNull());
            const QString common = tr("<ol><li>Connect the board to this computer with a USB cable that carries data, not a charge-only one.</li>"
                                      "<li>Press and hold <b>BOOT</b>.</li><li>While holding it, press and release <b>RESET</b>.</li>"
                                      "<li>Release <b>BOOT</b>.</li></ol>"
                                      "This window then shows \"Board in USB boot mode\". Nothing on the board changes by doing this; "
                                      "unplugging the board or pressing RESET alone returns it to normal.");
            if (k == "k1")
                steps->setText(tr("<p>The two buttons sit together near one corner of the mainboard: with the Micro-USB port facing down, they are at the top left. "
                                  "The upper one is BOOT (marked \"mode\" on some boards), the lower one is RESET.</p>") + common);
            else
                steps->setText(tr("<p>Open the pad's back cover. BOOT and RESET are two small push buttons on the circuit board.</p>") + common);
        };
        connect(which, &QComboBox::currentIndexChanged, &d, fill);
        fill();
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        v->addWidget(bb);
        d.exec();
    }

    void showInfo(const QJsonObject &o)
    {
        if (!o.value("ok").toBool()) {
            log->appendPlainText(tr("Error: %1").arg(o.value("error").toString()));
            autoBlocked = true;     // don't retry by itself every second; Refresh still works
            stateHint->setText(tr("Could not read the board: %1").arg(o.value("error").toString()));
            return;
        }
        autoBlocked = false;
        refreshShell(o.value("sn").toString());
        QLocale loc;
        const QString desc = o.value("model_description").toString();
        const QString m = o.value("model").toString();
        modelId = m;
        emmcBytes = qint64(o.value("emmc_bytes").toDouble());
        infoLoaded = true;
        infoStale = false;
        model->setText(m.isEmpty() ? tr("Unknown (the board has no identity record)") : (desc.isEmpty() ? m : desc.left(1).toUpper() + desc.mid(1)));
        serial->setText(o.value("sn").toString().isEmpty() ? QStringLiteral("—") : o.value("sn").toString());
        mac->setText(o.value("mac").toString().isEmpty() ? QStringLiteral("—") : o.value("mac").toString());
        board->setText(o.value("board").toString().isEmpty() ? QStringLiteral("—") : o.value("board").toString());
        const QString sl = o.value("active_slot").toString();
        auto describe = [this](const QJsonValue &v) {
            const QJsonObject s = v.toObject();
            return s.value("bootable").toBool() ? QStringLiteral("%1 %2").arg(s.value("firmware_name").toString(), s.value("firmware_version").toString())
                                                : tr("empty");
        };
        if (sl.isEmpty()) {
            slot->setText(tr("None — this board is blank. Use Install firmware."));
            otherSlot->setText(QStringLiteral("—"));
        } else {
            const bool aActive = sl == "a";
            slot->setText(tr("%1 — slot %2, active").arg(describe(o.value(aActive ? "slot_a" : "slot_b")), aActive ? "A" : "B"));
            otherSlot->setText(tr("%1 — slot %2").arg(describe(o.value(aActive ? "slot_b" : "slot_a")), aActive ? "B" : "A"));
        }
        storage->setText(loc.formattedDataSize(qint64(o.value("emmc_bytes").toDouble())));
        const QJsonArray ps = o.value("partitions").toArray();
        parts->setRowCount(ps.size());
        for (int i = 0; i < ps.size(); i++) {
            const QJsonObject p = ps[i].toObject();
            parts->setItem(i, 0, new QTableWidgetItem(p.value("name").toString()));
            parts->setItem(i, 1, new QTableWidgetItem(QStringLiteral("0x%1").arg(qulonglong(p.value("offset").toDouble()), 0, 16)));
            parts->setItem(i, 2, new QTableWidgetItem(loc.formattedDataSize(qint64(p.value("size").toDouble()))));
        }
    }

    QLabel *stateTitle, *stateHint, *model, *serial, *mac, *board, *slot, *otherSlot, *storage, *progressLabel;
    bool infoLoaded = false, infoStale = false, autoBlocked = false, recoveryRunning = false;
    QLabel *shellLine;
    QPushButton *shellButton;
    QPushButton *boardLogButton, *bootHelpButton, *netButton, *wifiButton;
    QString networkAddress;     // the board paired over the network ("" = USB)
    // printer type / mainboard the user chose for the install in progress ("" = let nocry decide)
    QString pendingModel, pendingBoard;
    int pendingPicture = 2;     // boot picture choice for the install under way (see nocry_install3_json)
    QWidget *udevBanner = nullptr;
    QPushButton *partBackupButton, *partRestoreButton, *partEraseButton, *identityButton, *eraseAllButton, *loaderButton, *pictureButton;
    QJsonObject shell;
    QPushButton *readButton, *backupButton, *installButton, *switchButton, *resetButton, *restoreButton, *cancelButton;
    QString modelId;
    qint64 emmcBytes = 0;
    QTableWidget *parts;
    QProgressBar *progress;
    QPlainTextEdit *log;
    bool busy = false, polling = false, canRead = false;
    QString cancelRisk;     // what stopping the current operation half-way would leave behind ("" = nothing)
};

// Development default: find the built images in the source tree (packaged builds ship them
// next to the executable, which nocry-core finds by itself).
static void setDevPaths()
{
    QDir d(QCoreApplication::applicationDirPath());
    // far enough up for a macOS bundle too: gui/build/nocry-gui.app/Contents/MacOS
    for (int i = 0; i < 8; i++, d.cdUp()) {
        if (d.exists("tools/burner-build/out/spl-x2000e/spl-x2000e.bin")) {        // built here
            const QByteArray img = d.filePath("tools/burner-build/out").toUtf8();
            const QByteArray rec = d.filePath("tools/recovery-build/out/nocry-recovery.uImage").toUtf8();
            nocry_set_paths(img.constData(), QFile::exists(rec) ? rec.constData() : nullptr);
            return;
        }
        if (d.exists("host/Cargo.toml") && d.exists("images/spl-x2000e/spl-x2000e.bin")) {   // fetched by tools/get-images.sh
            const QByteArray img = d.filePath("images").toUtf8();
            const QByteArray rec = d.filePath("images/nocry-recovery.uImage").toUtf8();
            nocry_set_paths(img.constData(), rec.constData());
            return;
        }
    }
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("nocry");
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/nocry.png")));
    QApplication::setDesktopFileName(QStringLiteral("nocry"));
    setDevPaths();
    Window w;
    w.show();
    const QStringList args = app.arguments();
    const int st = args.indexOf("--selftest");   // --selftest out.png: read the device, save a screenshot, quit
    if (st > 0 && st + 1 < args.size()) {
        const QString out = args[st + 1];
        const int tab = st + 2 < args.size() ? args[st + 2].toInt() : 0;   // optional: tab to show
        w.onIdle = [&w, out, tab] {
            w.tabWidget->setCurrentIndex(tab);
            QTimer::singleShot(500, [&w, out] { w.grab().save(out); QApplication::quit(); });
        };
        QTimer::singleShot(1500, [&w] { w.readDevice(); });
        QTimer::singleShot(120000, [] { QApplication::exit(2); });
    }
    // --selftest-backup out.img shot.png [cancel-after-seconds]: back up, screenshot at the end
    // (and one mid-way as shot.png.mid.png), exit 0 on the expected outcome
    const int sb = args.indexOf("--selftest-backup");
    if (sb > 0 && sb + 2 < args.size()) {
        const QString img = args[sb + 1], shot = args[sb + 2];
        const int cancelAfter = sb + 3 < args.size() ? args[sb + 3].toInt() : 0;
        w.quiet = true;
        w.onIdle = [&w, shot, cancelAfter] {
            QTimer::singleShot(300, [&w, shot, cancelAfter] {
                w.grab().save(shot);
                const bool ok = w.lastResult.value("ok").toBool(), c = w.lastResult.value("cancelled").toBool();
                QApplication::exit(cancelAfter ? (c ? 0 : 1) : (ok ? 0 : 1));
            });
        };
        QTimer::singleShot(1500, [&w, img] { w.startBackup(img); });
        QTimer::singleShot(20000, [&w, shot] { w.grab().save(shot + ".mid.png"); });
        if (cancelAfter)
            QTimer::singleShot(1500 + cancelAfter * 1000, [] { nocry_cancel(); });
    }
    // --selftest-restore backup.img shot.png [allow]: writes only with NOCRY_TEST_TARGET
    const int sx = args.indexOf("--selftest-restore");
    if (sx > 0 && sx + 2 < args.size()) {
        const QString img = args[sx + 1], shot = args[sx + 2];
        w.quiet = true;
        if (qEnvironmentVariableIsSet("NOCRY_TEST_TARGET"))
            w.autoRestore = (sx + 3 < args.size() && args[sx + 3] == "allow") ? 2 : 1;
        w.onIdle = [&w, shot] {
            QTimer::singleShot(300, [&w, shot] {
                w.grab().save(shot);
                fprintf(stderr, "%s\n", QJsonDocument(w.lastResult).toJson(QJsonDocument::Compact).constData());
                QApplication::exit(w.lastResult.value("ok").toBool() ? 0 : 1);
            });
        };
        QTimer::singleShot(1500, [&w, img] { w.startRestore(img, false, false); });
    }
    // --selftest-adv <pbackup NAME OUT | prestore NAME FILE | perase NAME | erase | identity SN MAC MODEL BOARD>
    // Writing operations require NOCRY_TEST_TARGET (an image file).
    const int sa = args.indexOf("--selftest-adv");
    if (sa > 0 && sa + 1 < args.size()) {
        const QStringList a = args.mid(sa + 1);
        if (a.value(0) != "pbackup" && a.value(0) != "boardlog" && !qEnvironmentVariableIsSet("NOCRY_TEST_TARGET"))
            return 4;
        w.quiet = true;
        w.onIdle = [&w] {
            QTimer::singleShot(200, [&w] {
                fprintf(stderr, "%s\n", QJsonDocument(w.lastResult).toJson(QJsonDocument::Compact).constData());
                QApplication::exit(w.lastResult.value("ok").toBool() ? 0 : 1);
            });
        };
        QTimer::singleShot(1200, [&w, a] { w.testAdvanced(a); });
    }
    // --selftest-shell shot.png: read the board, save the main window and the shell help dialog
    const int sh = args.indexOf("--selftest-shell");
    if (sh > 0 && sh + 1 < args.size()) {
        const QString shot = args[sh + 1];
        w.onIdle = [&w, shot] {
            QTimer::singleShot(2500, [&w, shot] {       // let the adb query finish
                w.grab().save(shot);
                QTimer::singleShot(700, [shot] {
                    if (QWidget *m = QApplication::activeModalWidget()) {
                        m->grab().save(shot + ".help.png");
                        static_cast<QDialog *>(m)->reject();
                    }
                    QTimer::singleShot(200, [] { QApplication::quit(); });
                });
                w.openShellHelp();
            });
            w.onIdle = nullptr;
        };
        QTimer::singleShot(1500, [&w] { w.readDevice(); });
        QTimer::singleShot(60000, [] { QApplication::exit(2); });
    }
    // --selftest-install-dialog shot.png: open the Install firmware dialog, save a picture of it
    const int sd = args.indexOf("--selftest-install-dialog");
    if (sd > 0 && sd + 1 < args.size()) {
        const QString shot = args[sd + 1];
        w.quiet = true;
        QTimer::singleShot(1200, [shot] {
            QTimer::singleShot(600, [shot] {
                if (QWidget *m = QApplication::activeModalWidget()) {
                    m->grab().save(shot);
                    static_cast<QDialog *>(m)->reject();
                }
                QTimer::singleShot(200, [] { QApplication::quit(); });
            });
        });
        QTimer::singleShot(1200, [&w] { w.openInstallDialog(); });
    }
    // --selftest-identity-dialog shot.png: read the board, open the identity dialog, press
    // "Copy to clipboard", save a picture and print what was copied
    const int sid = args.indexOf("--selftest-identity-dialog");
    if (sid > 0 && sid + 1 < args.size()) {
        const QString shot = args[sid + 1];
        w.onIdle = [&w, shot] {
            static bool once = false;      // readDevice and the dialog closing both end here
            if (once)
                return;
            once = true;
            QTimer::singleShot(600, [shot] {
                if (QWidget *m = QApplication::activeModalWidget()) {
                    for (QPushButton *b : m->findChildren<QPushButton *>())
                        if (b->text() == QObject::tr("Copy to clipboard"))
                            b->click();
                    m->grab().save(shot);
                    fprintf(stderr, "clipboard:\n%s\n", qPrintable(QApplication::clipboard()->text()));
                    static_cast<QDialog *>(m)->reject();
                }
                QTimer::singleShot(200, [] { QApplication::quit(); });
            });
            for (QPushButton *b : w.findChildren<QPushButton *>())
                if (b->text() == QObject::tr("Edit serial number and MAC…")) {
                    b->setEnabled(true);
                    b->click();
                }
        };
        QTimer::singleShot(300, [&w] { w.readDevice(); });
    }
    if (qEnvironmentVariableIsSet("NOCRY_TEST_PICTURE"))
        w.setPictureChoice(qEnvironmentVariableIntValue("NOCRY_TEST_PICTURE"));
    // NOCRY_TEST_CANCEL_MS=n with any self-test: press Cancel n ms after it first becomes available
    if (const int ms = qEnvironmentVariableIntValue("NOCRY_TEST_CANCEL_MS")) {
        auto *t = new QTimer(&w);
        auto *seen = new qint64(0);
        QObject::connect(t, &QTimer::timeout, &w, [&w, t, seen, ms] {
            if (!w.cancelOffered())
                return;
            if (!*seen)
                *seen = QDateTime::currentMSecsSinceEpoch();
            if (QDateTime::currentMSecsSinceEpoch() - *seen >= ms && w.testCancel())
                t->stop();
        });
        t->start(20);
    }
    // --selftest-cancel-question shot.png: the question Cancel asks during an install, and the window behind it
    const int scq = args.indexOf("--selftest-cancel-question");
    if (scq >= 0 && scq + 1 < args.size()) {
        const QString shot = args[scq + 1];
        QTimer::singleShot(600, [&w] { w.testCancelQuestion(); });
        QTimer::singleShot(1500, [&w, shot] {
            w.grab().save(shot + ".window.png");
            if (QWidget *d = QApplication::activeModalWidget()) {
                d->grab().save(shot);
                d->close();
            }
            QTimer::singleShot(200, [] { QApplication::quit(); });
        });
    }
    // --selftest-wifi ADDRESS CODE shot.png: pair, open the Board's Wi-Fi dialog, save a picture of it
    const int swf = args.indexOf("--selftest-wifi");
    if (swf >= 0 && swf + 3 < args.size()) {
        const QString a = args[swf + 1], c = args[swf + 2], shot = args[swf + 3];
        QTimer::singleShot(400, [&w, a, c] {
            if (!w.testConnect(a, c))
                QApplication::exit(3);
        });
        QTimer::singleShot(4000, [&w] { w.openWifiDialog(); });
        QTimer::singleShot(7000, [shot] {
            if (QWidget *d = QApplication::activeModalWidget()) {
                d->grab().save(shot);
                d->close();
            }
            QTimer::singleShot(300, [] { QApplication::quit(); });
        });
        QTimer::singleShot(30000, [] { QApplication::exit(2); });
    }
    // --selftest-net-dialog shot.png: open the network dialog, save a picture of it after the search
    const int snd = args.indexOf("--selftest-net-dialog");
    if (snd >= 0 && snd + 1 < args.size()) {
        const QString shot = args[snd + 1];
        QTimer::singleShot(800, [&w] { w.openNetworkDialog(); });
        QTimer::singleShot(4500, [shot] {
            if (QWidget *d = QApplication::activeModalWidget()) {
                d->grab().save(shot);
                d->close();
            }
            QTimer::singleShot(200, [] { QApplication::quit(); });
        });
    }
    // --selftest-net ADDRESS CODE shot.png: pair, read the board over the network, save the window
    const int sn = args.indexOf("--selftest-net");
    if (sn >= 0 && sn + 3 < args.size()) {
        const QString a = args[sn + 1], c = args[sn + 2], shot = args[sn + 3];
        w.onIdle = [&w, shot] {
            QTimer::singleShot(1500, [&w, shot] { w.grab().save(shot); QApplication::quit(); });
        };
        QTimer::singleShot(500, [&w, a, c, shot] {
            if (!w.testConnect(a, c)) {
                w.grab().save(shot);
                QApplication::exit(3);
            }
        });
        QTimer::singleShot(90000, [] { QApplication::exit(2); });
    }
    // --selftest-picture shot.png: open the Boot Picture dialog (needs NOCRY_TEST_TARGET, an image file)
    const int spi = args.indexOf("--selftest-picture");
    if (spi > 0 && spi + 1 < args.size() && qEnvironmentVariableIsSet("NOCRY_TEST_TARGET")) {
        const QString shot = args[spi + 1];
        QTimer::singleShot(800, [&w] { w.startPicture(-1); });
        QTimer::singleShot(4000, [shot] {
            if (QWidget *m = QApplication::activeModalWidget()) {
                m->grab().save(shot);
                static_cast<QDialog *>(m)->reject();
            }
            QTimer::singleShot(200, [] { QApplication::quit(); });
        });
    }
    // --selftest-loader shot.png: open the Boot Loader dialog (needs NOCRY_TEST_TARGET, an image file)
    const int slo = args.indexOf("--selftest-loader");
    if (slo > 0 && slo + 1 < args.size() && qEnvironmentVariableIsSet("NOCRY_TEST_TARGET")) {
        const QString shot = args[slo + 1];
        QTimer::singleShot(800, [&w] { w.startLoader(0, {}); });
        QTimer::singleShot(4000, [shot] {
            if (QWidget *m = QApplication::activeModalWidget()) {
                m->grab().save(shot);
                static_cast<QDialog *>(m)->reject();
            }
            QTimer::singleShot(200, [] { QApplication::quit(); });
        });
    }
    // --selftest-install-list <MODEL-KEY> <version|latest> <keep:0|1> shot.png: download from the
    // list, then plan; writes only with NOCRY_TEST_TARGET
    const int sl = args.indexOf("--selftest-install-list");
    if (sl > 0 && sl + 4 < args.size()) {
        const QString key = args[sl + 1], ver = args[sl + 2], shot = args[sl + 4];
        const int keep = args[sl + 3].toInt();
        w.quiet = true;
        w.autoConfirm = qEnvironmentVariableIsSet("NOCRY_TEST_TARGET");
        w.onIdle = [&w, shot] {
            QTimer::singleShot(300, [&w, shot] {
                w.grab().save(shot);
                fprintf(stderr, "%s\n", QJsonDocument(w.lastResult).toJson(QJsonDocument::Compact).constData());
                QApplication::exit(w.lastResult.value("ok").toBool() ? 0 : 1);
            });
        };
        QTimer::singleShot(1500, [&w, key, ver, keep] {
            const QByteArray dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation).toUtf8();
            const QJsonObject cat = takeJson(nocry_catalog_json(dir.constData(), 0));
            for (const auto &mv : cat.value("models").toArray()) {
                if (mv.toObject().value("key").toString() != key)
                    continue;
                const QJsonArray fws = mv.toObject().value("firmwares").toArray();
                for (const auto &fv : fws) {
                    if (ver == "latest" || fv.toObject().value("version").toString() == ver) {
                        w.startDownload(fv.toObject(), keep);
                        return;
                    }
                }
            }
            QApplication::exit(3);
        });
    }
    // --selftest-reset shot.png <keep-userdata:0|1>: only with NOCRY_TEST_TARGET (an image file)
    const int sr = args.indexOf("--selftest-reset");
    if (sr > 0 && sr + 2 < args.size() && qEnvironmentVariableIsSet("NOCRY_TEST_TARGET")) {
        const QString shot = args[sr + 1];
        const bool keep = args[sr + 2] == "1";
        w.quiet = true;
        w.onIdle = [&w, shot] {
            QTimer::singleShot(300, [&w, shot] {
                w.grab().save(shot);
                fprintf(stderr, "%s\n", QJsonDocument(w.lastResult).toJson(QJsonDocument::Compact).constData());
                QApplication::exit(w.lastResult.value("ok").toBool() ? 0 : 1);
            });
        };
        QTimer::singleShot(1500, [&w, keep] { w.startReset(keep); });
    }
    // --selftest-slots shot.png [a|b]: read the slots; with a slot, switch to it (only when
    // NOCRY_TEST_TARGET names an image file)
    const int ss = args.indexOf("--selftest-slots");
    if (ss > 0 && ss + 1 < args.size()) {
        const QString shot = args[ss + 1];
        w.quiet = true;
        if (ss + 2 < args.size() && qEnvironmentVariableIsSet("NOCRY_TEST_TARGET"))
            w.autoSlot = args[ss + 2];
        w.onIdle = [&w, shot] {
            QTimer::singleShot(300, [&w, shot] {
                w.grab().save(shot);
                fprintf(stderr, "%s\n", QJsonDocument(w.lastResult).toJson(QJsonDocument::Compact).constData());
                QApplication::exit(w.lastResult.value("ok").toBool() ? 0 : 1);
            });
        };
        QTimer::singleShot(1500, [&w] { w.startSlots(QString()); });
    }
    // --selftest-install <firmware> <keep:0|1> shot.png: plan and print the result. It only
    // proceeds to write when NOCRY_TEST_TARGET names an image file (never a real board).
    const int si = args.indexOf("--selftest-install");
    if (si > 0 && si + 3 < args.size()) {
        const QString os = args[si + 1], shot = args[si + 3];
        const int keep = args[si + 2].toInt();
        w.quiet = true;
        w.autoConfirm = qEnvironmentVariableIsSet("NOCRY_TEST_TARGET");
        // optional: answers for the printer type and mainboard questions
        w.autoModel = qEnvironmentVariable("NOCRY_TEST_MODEL");
        w.autoBoard = qEnvironmentVariable("NOCRY_TEST_BOARD");
        w.onIdle = [&w, shot] {
            QTimer::singleShot(300, [&w, shot] {
                w.grab().save(shot);
                fprintf(stderr, "%s\n", QJsonDocument(w.lastResult).toJson(QJsonDocument::Compact).constData());
                QApplication::exit(w.lastResult.value("ok").toBool() ? 0 : 1);
            });
        };
        QTimer::singleShot(1500, [&w, os, keep] { w.startInstall(os, keep, false); });
    }
    return app.exec();
}
