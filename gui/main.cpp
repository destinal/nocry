// nocry GUI, first slice: live board state, "read device", log and progress from nocry-core.
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QTabWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QRadioButton>
#include <QDate>
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
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
        v->addWidget(stateTitle);
        v->addWidget(stateHint);

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
        switchButton = new ActionButton(tr("Switch Active Slot…"), tr("Choose which installed firmware starts."));
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

        // --- Partitions (reference)
        parts = new QTableWidget(0, 3);
        parts->setHorizontalHeaderLabels({tr("Partition"), tr("Offset"), tr("Size")});
        parts->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        parts->verticalHeader()->hide();
        parts->setEditTriggers(QAbstractItemView::NoEditTriggers);
        parts->setSelectionBehavior(QAbstractItemView::SelectRows);
        tabs->addTab(parts, tr("Partitions"));

        // --- Activity log
        log = new QPlainTextEdit;
        log->setReadOnly(true);
        log->setMaximumBlockCount(2000);
        tabs->addTab(log, tr("Activity log"));

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
        connect(cancelButton, &QPushButton::clicked, this, [this] {
            cancelButton->setEnabled(false);
            stateHint->setText(tr("Stopping…"));
            nocry_cancel();
        });
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

    // Step 1: work out what installing `os` would change. Step 2 (after the user confirms, or
    // autoConfirm in self-tests): write it.
    void startInstall(const QString &os, bool keepData, bool execute)
    {
        if (busy)
            return;
        setBusy(true);
        if (execute)
            stateHint->setText(tr("Installing. Do not unplug the board."));
        else
            log->appendPlainText(tr("Checking %1").arg(QDir::toNativeSeparators(os)));
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
            QString fwName = o.value("firmware_name").toString();
            if (fwName.isEmpty())
                fwName = o.value("firmware_board").toString();
            const QString fw = tr("%1 firmware %2").arg(fwName, o.value("firmware_version").toString()).simplified();
            if (!o.value("ok").toBool()) {
                log->appendPlainText(tr("Install failed: %1").arg(o.value("error").toString()));
                if (!quiet)
                    QMessageBox::critical(this, execute ? tr("Installation failed") : tr("Cannot install this firmware"), o.value("error").toString());
            } else if (o.value("bytes_to_write").toDouble() == 0) {
                const QString msg = tr("The board already has %1 installed and intact. Nothing needs to be written.").arg(fw);
                log->appendPlainText(msg);
                if (!quiet)
                    QMessageBox::information(this, tr("Nothing to do"), msg);
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
        w->setFuture(QtConcurrent::run([p, keepData, execute, safety] {
            return takeJson(nocry_install_json(p.constData(), keepData, execute, safety.constData()));
        }));
    }

    // Read both system slots (switchTo empty), then let the user pick; or switch to "a"/"b".
    void startSlots(const QString &switchTo)
    {
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
                if (!quiet)
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
                log->appendPlainText(tr("Restore failed: %1").arg(o.value("error").toString()));
                if (!quiet)
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

    // Factory reset; the user has already confirmed in chooseReset().
    void startReset(bool keepUserdata)
    {
        if (busy)
            return;
        setBusy(true);
        stateHint->setText(tr("Resetting. Do not unplug the board."));
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
                log->appendPlainText(tr("Factory reset failed: %1").arg(o.value("error").toString()));
                if (!quiet)
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
    bool confirmInstall(const QJsonObject &o, const QString &fw, bool keepData)
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
        if (keepData)
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
        auto *go = box.addButton(keepData ? tr("Install") : tr("Erase and install"), QMessageBox::DestructiveRole);
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

    void chooseReset()
    {
        QMessageBox box(QMessageBox::Warning, tr("Factory reset"), tr("Reset this board to factory settings?"), QMessageBox::Cancel, this);
        box.setInformativeText(tr("All settings, calibration, Wi-Fi details and files on the printer will be erased. The printer sets itself up again the next time it starts.\n\n"
                                  "The installed firmware, both firmware slots, the serial number and the MAC address are kept."));
        auto *keep = new QCheckBox(tr("Keep the user storage partition (advanced)"));
        box.setCheckBox(keep);
        auto *go = box.addButton(tr("Erase and reset"), QMessageBox::DestructiveRole);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == go)
            startReset(keep->isChecked());
    }

    void chooseInstall()
    {
        QDialog d(this);
        d.setWindowTitle(tr("Install firmware"));
        auto *v = new QVBoxLayout(&d);
        v->addWidget(new QLabel(tr("Firmware (.ingenic file, or a folder with an unpacked update):")));
        auto *path = new QLineEdit;
        path->setMinimumWidth(420);
        auto *file = new QPushButton(tr("Choose file…"));
        auto *folder = new QPushButton(tr("Choose folder…"));
        auto *h = new QHBoxLayout;
        h->addWidget(path, 1);
        h->addWidget(file);
        h->addWidget(folder);
        v->addLayout(h);
        auto *clean = new QRadioButton(tr("Clean install (Recommended) — erase settings and files, like a new printer"));
        auto *keep = new QRadioButton(tr("Keep my data — repair or update the system; settings and files stay"));
        clean->setChecked(true);
        v->addWidget(clean);
        v->addWidget(keep);
        v->addWidget(new QLabel(tr("Nothing is written until you confirm on the next screen.")));
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        bb->button(QDialogButtonBox::Ok)->setText(tr("Continue"));
        bb->button(QDialogButtonBox::Ok)->setEnabled(false);
        v->addWidget(bb);
        connect(path, &QLineEdit::textChanged, &d, [bb](const QString &t) { bb->button(QDialogButtonBox::Ok)->setEnabled(QFileInfo::exists(t.trimmed())); });
        connect(file, &QPushButton::clicked, &d, [&] {
            const QString f = QFileDialog::getOpenFileName(&d, tr("Choose firmware"), QString(), tr("Ingenic firmware (*.ingenic);;All files (*)"));
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
        if (d.exec() == QDialog::Accepted)
            startInstall(path->text().trimmed(), keep->isChecked(), false);
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
        } else {
            progress->setVisible(false);
            progressLabel->clear();
        }
    }

    void setBusy(bool b)
    {
        busy = b;
        readButton->setEnabled(!b && canRead);
        backupButton->setEnabled(!b && canRead);
        installButton->setEnabled(!b && canRead);
        switchButton->setEnabled(!b && canRead);
        resetButton->setEnabled(!b && canRead);
        restoreButton->setEnabled(!b && canRead);
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
        canRead = s != "none" && s != "error" && s != "uboot-disk";
        if (s == "none" && infoLoaded) {
            for (auto *l : {model, serial, mac, board, slot, otherSlot, storage})
                l->setText(QStringLiteral("—"));
            parts->setRowCount(0);
            infoLoaded = false;
            modelId.clear();
        }
        if (s == "none")
            autoBlocked = false;
        // fill in the details by itself when a board shows up or after something changed it
        if (canRead && (!infoLoaded || infoStale) && !autoBlocked && !busy && !quiet && !QApplication::activeModalWidget()) {
            readDevice();
            return;
        }
        if (s == "none") {
            stateTitle->setText(tr("No board connected"));
            stateHint->setText(tr("Connect the board by USB, then hold BOOT, tap RESET and release BOOT."));
        } else if (s == "usb-boot") {
            stateTitle->setText(tr("Board in USB boot mode"));
            stateHint->setText(tr("Ready. Nothing on the board is changed until you choose an action."));
        } else if (s == "fastboot") {
            stateTitle->setText(tr("Board connected (loader running)"));
            stateHint->setText(tr("Ready."));
        } else if (s == "recovery") {
            stateTitle->setText(tr("Board connected"));
            stateHint->setText(tr("The recovery system is running from memory."));
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
    bool infoLoaded = false, infoStale = false, autoBlocked = false;
    QPushButton *readButton, *backupButton, *installButton, *switchButton, *resetButton, *restoreButton, *cancelButton;
    QString modelId;
    qint64 emmcBytes = 0;
    QTableWidget *parts;
    QProgressBar *progress;
    QPlainTextEdit *log;
    bool busy = false, polling = false, canRead = false;
};

// Development default: find the built images in the source tree (packaged builds ship them
// next to the executable, which nocry-core finds by itself).
static void setDevPaths()
{
    QDir d(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 5; i++, d.cdUp()) {
        if (d.exists("tools/burner-build/out/spl-x2000e/spl-x2000e.bin")) {
            const QByteArray img = d.filePath("tools/burner-build/out").toUtf8();
            const QByteArray rec = d.filePath("tools/recovery-build/out/nocry-recovery.uImage").toUtf8();
            nocry_set_paths(img.constData(), rec.constData());
            return;
        }
    }
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("nocry");
    setDevPaths();
    Window w;
    w.show();
    const QStringList args = app.arguments();
    const int st = args.indexOf("--selftest");   // --selftest out.png: read the device, save a screenshot, quit
    if (st > 0 && st + 1 < args.size()) {
        const QString out = args[st + 1];
        w.onIdle = [&w, out] { QTimer::singleShot(500, [&w, out] { w.grab().save(out); QApplication::quit(); }); };
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
        const bool keep = args[si + 2] == "1";
        w.quiet = true;
        w.autoConfirm = qEnvironmentVariableIsSet("NOCRY_TEST_TARGET");
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
