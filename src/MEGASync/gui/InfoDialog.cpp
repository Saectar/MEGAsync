#include <QDesktopServices>
#include <QDesktopWidget>
#include <QUrl>
#include <QRect>
#include <QTimer>
#include <QHelpEvent>
#include <QToolTip>
#include <QSignalMapper>
#include <QVBoxLayout>
#include "InfoDialog.h"
#include "ActiveTransfer.h"
#include "RecentFile.h"
#include "ui_InfoDialog.h"
#include "control/Utilities.h"
#include "MegaApplication.h"

#if QT_VERSION >= 0x050000
#include <QtConcurrent/QtConcurrent>
#endif

using namespace mega;

InfoDialog::InfoDialog(MegaApplication *app, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::InfoDialog)
{
    ui->setupUi(this);

    //Set window properties
    setWindowFlags(Qt::FramelessWindowHint | Qt::Popup);

#ifdef __APPLE__
    setAttribute(Qt::WA_TranslucentBackground);
#endif

    //Initialize fields
    this->app = app;
    downloadSpeed = 0;
    uploadSpeed = 0;
    currentUpload = 0;
    currentDownload = 0;
    totalUploads = 0;
    totalDownloads = 0;
    totalDownloadedSize = totalUploadedSize = 0;
    totalDownloadSize = totalUploadSize = 0;
    remainingUploads = remainingDownloads = 0;
    uploadStartTime = 0;
    downloadStartTime = 0;
    effectiveDownloadSpeed = 200000;
    effectiveUploadSpeed = 200000;
    ui->lDownloads->setText(QString::fromAscii(""));
    ui->lUploads->setText(QString::fromAscii(""));
    indexing = false;
    waiting = false;
    syncsMenu = NULL;
    activeDownload = NULL;
    activeUpload = NULL;
    transferMenu = NULL;
    gWidget = NULL;

    //Set properties of some widgets
    ui->sActiveTransfers->setCurrentWidget(ui->pUpdated);
    ui->wTransfer1->setType(MegaTransfer::TYPE_DOWNLOAD);
    ui->wTransfer1->hideTransfer();
    ui->wTransfer2->setType(MegaTransfer::TYPE_UPLOAD);
    ui->wTransfer2->hideTransfer();

    megaApi = app->getMegaApi();
    megaApiGuest = app->getMegaApiGuest();
    preferences = Preferences::instance();
    scanningTimer.setSingleShot(false);
    scanningTimer.setInterval(60);
    scanningAnimationIndex = 1;
    connect(&scanningTimer, SIGNAL(timeout()), this, SLOT(scanningAnimationStep()));

    uploadsFinishedTimer.setSingleShot(true);
    uploadsFinishedTimer.setInterval(5000);
    connect(&uploadsFinishedTimer, SIGNAL(timeout()), this, SLOT(onAllUploadsFinished()));

    downloadsFinishedTimer.setSingleShot(true);
    downloadsFinishedTimer.setInterval(5000);
    connect(&downloadsFinishedTimer, SIGNAL(timeout()), this, SLOT(onAllDownloadsFinished()));

    transfersFinishedTimer.setSingleShot(true);
    transfersFinishedTimer.setInterval(5000);
    connect(&transfersFinishedTimer, SIGNAL(timeout()), this, SLOT(onAllTransfersFinished()));

    ui->wDownloadDesc->hide();
    ui->wUploadDesc->hide();

#ifdef __APPLE__
    arrow = new QPushButton(this);
    arrow->setIcon(QIcon(QString::fromAscii("://images/top_arrow.png")));
    arrow->setIconSize(QSize(22,11));
    arrow->setStyleSheet(QString::fromAscii("border: none; padding-bottom: -1px; "));
    arrow->resize(22,11);
    arrow->hide();
#endif

    if (gWidget)
    {
        gWidget->hideDownloads();
    }

    //Create the overlay widget with a semi-transparent background
    //that will be shown over the transfers when they are paused
    overlay = new QPushButton(this);
    overlay->setIcon(QIcon(QString::fromAscii("://images/tray_paused_large_ico.png")));
    overlay->setIconSize(QSize(64, 64));
    overlay->setStyleSheet(QString::fromAscii("background-color: rgba(247, 247, 247, 200); "
                                              "border: none; "));

#ifdef __APPLE__
    minHeightAnimation = new QPropertyAnimation();
    maxHeightAnimation = new QPropertyAnimation();
    animationGroup = new QParallelAnimationGroup();

    minHeightAnimation->setTargetObject(this);
    maxHeightAnimation->setTargetObject(this);
    minHeightAnimation->setPropertyName("minimumHeight");
    maxHeightAnimation->setPropertyName("maximumHeight");
    animationGroup->addAnimation(minHeightAnimation);
    animationGroup->addAnimation(maxHeightAnimation);
    connect(animationGroup, SIGNAL(finished()), this, SLOT(onAnimationFinished()));
#endif

    ui->wTransfer1->hide();
    ui->wTransfer1->hide();
    overlay->resize(ui->wTransfers->minimumSize());
#ifdef __APPLE__
    overlay->move(1, 72);
#else
    overlay->move(2, 60);
    overlay->resize(overlay->width()-4, overlay->height());
#endif
    overlay->hide();
    connect(overlay, SIGNAL(clicked()), this, SLOT(onOverlayClicked()));
    connect(ui->wTransfer1, SIGNAL(cancel(int, int)), this, SLOT(onTransfer1Cancel(int, int)));
    connect(ui->wTransfer2, SIGNAL(cancel(int, int)), this, SLOT(onTransfer2Cancel(int, int)));

#ifdef __APPLE__
    ui->wRecentlyUpdated->hide();
    ui->wRecent1->hide();
    ui->wRecent2->hide();
    ui->wRecent3->hide();
    setMinimumHeight(377);
    setMaximumHeight(377);
#endif

    if (preferences->logged())
    {
        setUsage();
        updateSyncsButton();
    }
    else
    {
        regenerateLayout();
        gWidget->hideDownloads();
    }
}

InfoDialog::~InfoDialog()
{
    delete ui;
    delete gWidget;
}

void InfoDialog::setUsage()
{
    if (!preferences->totalStorage())
    {
        return;
    }

    int percentage = ceil((100 * preferences->usedStorage()) / (double)preferences->totalStorage());
    ui->pUsage->setProgress(preferences->cloudDriveStorage(), preferences->rubbishStorage(),
                            preferences->inShareStorage(), preferences->inboxStorage(),
                            preferences->totalStorage(), preferences->usedStorage());
    QString used = tr("%1 of %2").arg(QString::number(percentage).append(QString::fromAscii("%")))
            .arg(Utilities::getSizeString(preferences->totalStorage()));
    ui->lPercentageUsed->setText(used);
    ui->lTotalUsed->setText(tr("Usage: %1").arg(Utilities::getSizeString(preferences->usedStorage())));
}

void InfoDialog::setTransfer(MegaTransfer *transfer)
{
    if (!transfer)
    {
        return;
    }

    int type = transfer->getType();
    QString fileName = QString::fromUtf8(transfer->getFileName());
    long long completedSize = transfer->getTransferredBytes();
    long long totalSize = transfer->getTotalBytes();

    ActiveTransfer *wTransfer;
    if (type == MegaTransfer::TYPE_DOWNLOAD)
    {
        wTransfer = !preferences->logged() ? gWidget->getTransfer() : ui->wTransfer1;
        if (activeDownload != transfer)
        {
            delete activeDownload;
            activeDownload = transfer->copy();
        }

        if (!downloadStartTime)
        {
            downloadStartTime = QDateTime::currentMSecsSinceEpoch();
            elapsedDownloadTime=0;
            lastUpdate = QDateTime::currentMSecsSinceEpoch();
        }
    }
    else
    {
        wTransfer = ui->wTransfer2;
        if (activeUpload != transfer)
        {
            delete activeUpload;
            activeUpload = transfer->copy();
        }

        if (!uploadStartTime)
        {
            uploadStartTime = QDateTime::currentMSecsSinceEpoch();
            elapsedUploadTime=0;
            lastUpdate = QDateTime::currentMSecsSinceEpoch();
        }
    }

    bool shown = wTransfer->isVisible();
    wTransfer->setFileName(fileName);
    wTransfer->setProgress(completedSize, totalSize, !transfer->isSyncTransfer());
    if (!shown)
    {
        updateState();
    }
}

void InfoDialog::addRecentFile(QString fileName, long long fileHandle, QString localPath, QString nodeKey)
{
    RecentFileInfo info1 = ui->wRecent1->getFileInfo();
    RecentFileInfo info2 = ui->wRecent2->getFileInfo();
    ui->wRecent3->setFileInfo(info2);
    ui->wRecent2->setFileInfo(info1);
    ui->wRecent1->setFile(fileName, fileHandle, localPath, nodeKey, QDateTime::currentDateTime().toMSecsSinceEpoch());

#ifdef __APPLE__
    if (!ui->wRecentlyUpdated->isVisible())
    {
        showRecentList();
    }
#endif
    updateRecentFiles();
}

void InfoDialog::clearRecentFiles()
{
    ui->wRecent1->clear();
    ui->wRecent2->clear();
    ui->wRecent3->clear();
    updateRecentFiles();

#ifdef __APPLE__
    ui->wRecentlyUpdated->hide();
    ui->wRecent1->hide();
    ui->wRecent2->hide();
    ui->wRecent3->hide();
    setMinimumHeight(377);
    setMaximumHeight(377);
#endif
}

void InfoDialog::updateTransfers()
{
    remainingUploads = megaApi->getNumPendingUploads() + megaApiGuest->getNumPendingUploads();
    remainingDownloads = megaApi->getNumPendingDownloads() + megaApiGuest->getNumPendingDownloads();
    totalUploads = megaApi->getTotalUploads() + megaApiGuest->getTotalUploads();
    totalDownloads = megaApi->getTotalDownloads() + megaApiGuest->getTotalDownloads();

    if (totalUploads < remainingUploads)
    {
        totalUploads = remainingUploads;
    }

    if (totalDownloads < remainingDownloads)
    {
        totalDownloads = remainingDownloads;
    }

    currentDownload = totalDownloads - remainingDownloads + 1;
    currentUpload = totalUploads - remainingUploads + 1;

    if (remainingDownloads)
    {
        long long remainingBytes = totalDownloadSize-totalDownloadedSize;
        if (remainingBytes < 0)
        {
            remainingBytes = 0;
        }

        unsigned long long timeIncrement = QDateTime::currentMSecsSinceEpoch()-lastUpdate;
        if (timeIncrement < 1000)
        {
            elapsedDownloadTime += timeIncrement;
        }

        double effectiveSpeed = effectiveDownloadSpeed;
        double elapsedDownloadTimeSecs = elapsedDownloadTime/1000.0;
        if (elapsedDownloadTimeSecs)
        {
            effectiveSpeed = totalDownloadedSize/elapsedDownloadTimeSecs;
        }

        effectiveDownloadSpeed += (effectiveSpeed-effectiveDownloadSpeed)/3; //Smooth the effective speed
        if (isVisible())
        {
            int totalRemainingSeconds = (effectiveDownloadSpeed) ? remainingBytes/effectiveDownloadSpeed : 0;
            int remainingHours = totalRemainingSeconds/3600;
            if ((remainingHours<0) || (remainingHours>99))
            {
                totalRemainingSeconds = 0;
            }

            int remainingMinutes = (totalRemainingSeconds%3600)/60;
            int remainingSeconds =  (totalRemainingSeconds%60);
            QString remainingTime;
            if (totalRemainingSeconds)
            {
                remainingTime = QString::fromAscii("%1:%2:%3").arg(remainingHours, 2, 10, QChar::fromAscii('0'))
                    .arg(remainingMinutes, 2, 10, QChar::fromAscii('0'))
                    .arg(remainingSeconds, 2, 10, QChar::fromAscii('0'));
            }
            else
            {
                remainingTime = QString::fromAscii("--:--:--");
            }

            !preferences->logged() ? gWidget->setRemainingTime(remainingTime)
                      : ui->lRemainingTimeD->setText(remainingTime);
            ui->wDownloadDesc->show();
            QString fullPattern = QString::fromAscii("<span style=\"color: rgb(120, 178, 66); \">%1</span>%2");
            QString operation = tr("Downloading ");
            if (operation.size() && operation[operation.size() - 1] != QChar::fromAscii(' '))
            {
                operation.append(QChar::fromAscii(' '));
            }

            QString pattern(tr("%1 of %2 (%3/s)"));
            QString pausedPattern(tr("%1 of %2 (paused)"));
            QString invalidSpeedPattern(tr("%1 of %2"));
            QString downloadString;

            if (downloadSpeed >= 20000)
            {
                downloadString = pattern.arg(currentDownload)
                        .arg(totalDownloads)
                        .arg(Utilities::getSizeString(downloadSpeed));
            }
            else if (downloadSpeed >= 0)
            {
                downloadString = invalidSpeedPattern.arg(currentDownload).arg(totalDownloads);
            }
            else
            {
                downloadString = pausedPattern.arg(currentDownload).arg(totalDownloads);
            }

            if (preferences->logged())
            {
                ui->lDownloads->setText(fullPattern.arg(operation).arg(downloadString));
                if (!ui->wTransfer1->isActive())
                {
                    ui->wDownloadDesc->hide();
                }
                else
                {
                    ui->wDownloadDesc->show();
                }
            }
            else
            {
                gWidget->setDownloadLabel(fullPattern.arg(operation).arg(downloadString));
                if (!gWidget->getTransfer()->isActive())
                {
                    gWidget->hideDownloads();
                }
                else
                {
                    gWidget->showDownloads();
                }
            }
        }
    }


    if (remainingUploads)
    {
        long long remainingBytes = totalUploadSize-totalUploadedSize;
        if (remainingBytes < 0)
        {
            remainingBytes = 0;
        }

        unsigned long long timeIncrement = QDateTime::currentMSecsSinceEpoch()-lastUpdate;
        if (timeIncrement < 1000)
        {
            elapsedUploadTime += timeIncrement;
        }

        double effectiveSpeed = effectiveUploadSpeed;
        double elapsedUploadTimeSecs = elapsedUploadTime / 1000.0;
        if (elapsedUploadTimeSecs)
        {
            effectiveSpeed = totalUploadedSize / elapsedUploadTimeSecs;
        }

        effectiveUploadSpeed += (effectiveSpeed - effectiveUploadSpeed) / 3; //Smooth the effective speed

        if (isVisible())
        {
            int totalRemainingSeconds = (effectiveUploadSpeed) ? remainingBytes/effectiveUploadSpeed : 0;
            int remainingHours = totalRemainingSeconds/3600;
            if ((remainingHours < 0) || (remainingHours > 99))
            {
                totalRemainingSeconds = 0;
            }

            int remainingMinutes = (totalRemainingSeconds%3600)/60;
            int remainingSeconds =  (totalRemainingSeconds%60);
            QString remainingTime;
            if (totalRemainingSeconds)
            {
                remainingTime = QString::fromAscii("%1:%2:%3").arg(remainingHours, 2, 10, QChar::fromAscii('0'))
                    .arg(remainingMinutes, 2, 10, QChar::fromAscii('0'))
                    .arg(remainingSeconds, 2, 10, QChar::fromAscii('0'));
            }
            else
            {
                remainingTime = QString::fromAscii("--:--:--");
            }

            ui->lRemainingTimeU->setText(remainingTime);
            ui->wUploadDesc->show();
            QString fullPattern = QString::fromAscii("<span style=\"color: rgb(119, 185, 217); \">%1</span>%2");
            QString operation = tr("Uploading ");
            if (operation.size() && operation[operation.size() - 1] != QChar::fromAscii(' '))
            {
                operation.append(QChar::fromAscii(' '));
            }

            QString pattern(tr("%1 of %2 (%3/s)"));
            QString pausedPattern(tr("%1 of %2 (paused)"));
            QString invalidSpeedPattern(tr("%1 of %2"));
            QString uploadString;

            if (uploadSpeed >= 20000)
            {
                uploadString = pattern.arg(currentUpload).arg(totalUploads).arg(Utilities::getSizeString(uploadSpeed));
            }
            else if (uploadSpeed >= 0)
            {
                uploadString = invalidSpeedPattern.arg(currentUpload).arg(totalUploads);
            }
            else
            {
                uploadString += pausedPattern.arg(currentUpload).arg(totalUploads);
            }

            ui->lUploads->setText(fullPattern.arg(operation).arg(uploadString));

            if (!ui->wTransfer2->isActive())
            {
                ui->wUploadDesc->hide();
            }
            else
            {
                ui->wUploadDesc->show();
            }
        }
    }

    if (remainingUploads || remainingDownloads)
    {
        if (!preferences->logged() && gWidget->getTransfer()->isActive())
        {
            gWidget->setIdleState(false);
        }
        else if (ui->wTransfer1->isActive() || ui->wTransfer2->isActive())
        {
            ui->sActiveTransfers->setCurrentWidget(ui->pUpdating);
            updateState();
        }
    }

    lastUpdate = QDateTime::currentMSecsSinceEpoch();
}

void InfoDialog::transferFinished(int error)
{
    remainingUploads = megaApi->getNumPendingUploads() + megaApiGuest->getNumPendingUploads();
    remainingDownloads = megaApi->getNumPendingDownloads() + megaApiGuest->getNumPendingDownloads();

    if (!remainingDownloads && ui->wTransfer1->isActive())
    {
        if (!downloadsFinishedTimer.isActive())
        {
            if (!error)
            {
                downloadsFinishedTimer.start();
            }
            else
            {
                onAllDownloadsFinished();
            }
        }
    }
    else
    {
        downloadsFinishedTimer.stop();
    }

    if (!remainingUploads && ui->wTransfer2->isActive())
    {
        if (!uploadsFinishedTimer.isActive())
        {
            if (!error)
            {
                uploadsFinishedTimer.start();
            }
            else
            {
                onAllUploadsFinished();
            }
        }
    }
    else
    {
        uploadsFinishedTimer.stop();
    }

    if (!remainingDownloads
            && !remainingUploads
            &&  (ui->sActiveTransfers->currentWidget() != ui->pUpdated
                 || (!preferences->logged() && !gWidget->idleState())))
    {
        if (!transfersFinishedTimer.isActive())
        {
            if (!error)
            {
                transfersFinishedTimer.start();
            }
            else
            {
                onAllTransfersFinished();
            }
        }
    }
    else
    {
        transfersFinishedTimer.stop();
    }
}

void InfoDialog::updateSyncsButton()
{
    int num = preferences->getNumSyncedFolders();
    long long firstSyncHandle = mega::INVALID_HANDLE;
    if (num == 1)
    {
        firstSyncHandle = preferences->getMegaFolderHandle(0);
    }

    MegaNode *rootNode = megaApi->getRootNode();
    if (!rootNode)
    {
        preferences->setCrashed(true);
        ui->bSyncFolder->setText(QString::fromAscii("MEGA"));
        return;
    }
    long long rootHandle = rootNode->getHandle();

    if ((num == 1) && (firstSyncHandle == rootHandle))
    {
        ui->bSyncFolder->setText(QString::fromAscii("MEGA"));
    }
    else
    {
        ui->bSyncFolder->setText(tr("Syncs"));
    }

    delete rootNode;
}

void InfoDialog::setIndexing(bool indexing)
{
    this->indexing = indexing;
}

void InfoDialog::setWaiting(bool waiting)
{
    this->waiting = waiting;
}

void InfoDialog::increaseUsedStorage(long long bytes, bool isInShare)
{
    if (isInShare)
    {
        preferences->setInShareStorage(preferences->inShareStorage() + bytes);
        preferences->setInShareFiles(preferences->inShareFiles()+1);
    }
    else
    {
        preferences->setCloudDriveStorage(preferences->cloudDriveStorage() + bytes);
        preferences->setCloudDriveFiles(preferences->cloudDriveFiles()+1);
    }

    preferences->setUsedStorage(preferences->usedStorage() + bytes);
    this->setUsage();
}

void InfoDialog::updateState()
{
    if (ui->bPause->isChecked())
    {
        if (!preferences->logged())
        {
            if (gWidget)
            {
                if (!gWidget->idleState())
                {
                    gWidget->setPauseState(true);
                }
                else
                {
                    gWidget->setPauseState(false);
                }
            }
            return;
        }

        if (scanningTimer.isActive())
        {
            scanningTimer.stop();
        }

        setTransferSpeeds(-1, -1);
        ui->lSyncUpdated->setText(tr("File transfers paused"));
        QIcon icon;
        icon.addFile(QString::fromUtf8(":/images/tray_paused_large_ico.png"), QSize(), QIcon::Normal, QIcon::Off);

        ui->label->setIcon(icon);
        ui->label->setIconSize(QSize(64, 64));

        if (ui->sActiveTransfers->currentWidget() != ui->pUpdated)
        {
            overlay->setVisible(true);
        }
        else
        {
            overlay->setVisible(false);
        }
    }
    else
    {
        if (!preferences->logged())
        {
            if (gWidget)
            {
                gWidget->setPauseState(false);
                if (!gWidget->getTransfer()->isActive())
                {
                    gWidget->setIdleState(true);
                }
            }
            return;
        }
        overlay->setVisible(false);
        if ((downloadSpeed<0) && (uploadSpeed<0))
        {
            setTransferSpeeds(0, 0);
        }

        if (waiting)
        {
            if (scanningTimer.isActive())
            {
                scanningTimer.stop();
            }

            ui->lSyncUpdated->setText(tr("MEGAsync is waiting"));
            QIcon icon;
            icon.addFile(QString::fromUtf8(":/images/tray_scanning_large_ico.png"), QSize(), QIcon::Normal, QIcon::Off);

            ui->label->setIcon(icon);
            ui->label->setIconSize(QSize(64, 64));
        }
        else if (indexing)
        {
            if (!scanningTimer.isActive())
            {
                scanningAnimationIndex = 1;
                scanningTimer.start();
            }

            ui->lSyncUpdated->setText(tr("MEGAsync is scanning"));

            QIcon icon;
            icon.addFile(QString::fromUtf8(":/images/tray_scanning_large_ico.png"), QSize(), QIcon::Normal, QIcon::Off);
            ui->label->setIcon(icon);
            ui->label->setIconSize(QSize(64, 64));
        }
        else
        {
            if (scanningTimer.isActive())
            {
                scanningTimer.stop();
            }

            ui->lSyncUpdated->setText(tr("MEGAsync is up to date"));
            QIcon icon;
            icon.addFile(QString::fromUtf8(":/images/tray_updated_large_ico.png"), QSize(), QIcon::Normal, QIcon::Off);
            ui->label->setIcon(icon);
            ui->label->setIconSize(QSize(64, 64));
        }
    }
}

#ifdef __APPLE__
void InfoDialog::showRecentlyUpdated(bool show)
{
    ui->wRecent->setVisible(show);
    if (!show)
    {
        this->setMinimumHeight(377);
        this->setMaximumHeight(377);
    }
    else
    {
        on_cRecentlyUpdated_stateChanged(0);
    }
}
#endif

void InfoDialog::closeSyncsMenu()
{
#ifdef __APPLE__
    if (syncsMenu && syncsMenu->isVisible())
    {
        syncsMenu->close();
    }

    if (transferMenu && transferMenu->isVisible())
    {
        transferMenu->close();
    }

    ui->wRecent1->closeMenu();
    ui->wRecent2->closeMenu();
    ui->wRecent3->closeMenu();
#endif
}

void InfoDialog::setTransferSpeeds(long long downloadSpeed, long long uploadSpeed)
{
    if (downloadSpeed || this->downloadSpeed < 0)
    {
        this->downloadSpeed = downloadSpeed;
    }

    if (uploadSpeed || this->uploadSpeed < 0)
    {
        this->uploadSpeed = uploadSpeed;
    }
}

void InfoDialog::setTransferredSize(long long totalDownloadedSize, long long totalUploadedSize)
{
    this->totalDownloadedSize = totalDownloadedSize;
    this->totalUploadedSize = totalUploadedSize;
}

void InfoDialog::setTotalTransferSize(long long totalDownloadSize, long long totalUploadSize)
{
    this->totalDownloadSize = totalDownloadSize;
    this->totalUploadSize = totalUploadSize;
}

void InfoDialog::setPaused(bool paused)
{
    ui->bPause->setChecked(paused);
    ui->bPause->setEnabled(true);
}

void InfoDialog::addSync()
{
    addSync(INVALID_HANDLE);
}

void InfoDialog::onTransfer1Cancel(int x, int y)
{
    if (transferMenu)
    {
#ifdef __APPLE__
        transferMenu->close();
        return;
#else
        transferMenu->deleteLater();
#endif
    }

    transferMenu = new QMenu();

#if (QT_VERSION == 0x050500) && defined(_WIN32)
    transferMenu->installEventFilter(app);
#endif

#ifndef __APPLE__
    transferMenu->setStyleSheet(QString::fromAscii(
            "QMenu {background-color: white; border: 2px solid #B8B8B8; padding: 5px; border-radius: 5px;} "
            "QMenu::item {background-color: white; color: black;} "
            "QMenu::item:selected {background-color: rgb(242, 242, 242);}"));
#endif

    QAction *cancelAll = transferMenu->addAction(tr("Cancel all downloads"), this, SLOT(cancelAllDownloads()));
    QAction *cancelCurrent = transferMenu->addAction(tr("Cancel download"), this, SLOT(cancelCurrentDownload()));
    transferMenu->addAction(cancelCurrent);
    transferMenu->addAction(cancelAll);

#ifdef __APPLE__
    transferMenu->exec(ui->wTransfer1->mapToGlobal(QPoint(x, y)));
    if (!this->rect().contains(this->mapFromGlobal(QCursor::pos())))
    {
        this->hide();
    }

    transferMenu->deleteLater();
    transferMenu = NULL;
#else
    transferMenu->popup(ui->wTransfer1->mapToGlobal(QPoint(x, y)));
#endif
}

void InfoDialog::onTransfer2Cancel(int x, int y)
{
    if (transferMenu)
    {
#ifdef __APPLE__
        transferMenu->close();
        return;
#else
        transferMenu->deleteLater();
#endif
    }

    transferMenu = new QMenu();

#if (QT_VERSION == 0x050500) && defined(_WIN32)
    transferMenu->installEventFilter(app);
#endif

#ifndef __APPLE__
    transferMenu->setStyleSheet(QString::fromAscii(
            "QMenu {background-color: white; border: 2px solid #B8B8B8; padding: 5px; border-radius: 5px;} "
            "QMenu::item {background-color: white; color: black;} "
            "QMenu::item:selected {background-color: rgb(242, 242, 242);}"));
#endif

    QAction *cancelAll = transferMenu->addAction(tr("Cancel all uploads"), this, SLOT(cancelAllUploads()));
    QAction *cancelCurrent = transferMenu->addAction(tr("Cancel upload"), this, SLOT(cancelCurrentUpload()));
    transferMenu->addAction(cancelCurrent);
    transferMenu->addAction(cancelAll);

#ifdef __APPLE__
    transferMenu->exec(ui->wTransfer1->mapToGlobal(QPoint(x, y)));
    if (!this->rect().contains(this->mapFromGlobal(QCursor::pos())))
    {
        this->hide();
    }

    transferMenu->deleteLater();
    transferMenu = NULL;
#else
    transferMenu->popup(ui->wTransfer1->mapToGlobal(QPoint(x, y)));
#endif
}

void InfoDialog::cancelAllUploads()
{
    megaApi->cancelTransfers(MegaTransfer::TYPE_UPLOAD);
    megaApiGuest->cancelTransfers(MegaTransfer::TYPE_UPLOAD);
}

void InfoDialog::cancelAllDownloads()
{
    megaApi->cancelTransfers(MegaTransfer::TYPE_DOWNLOAD);
    megaApiGuest->cancelTransfers(MegaTransfer::TYPE_DOWNLOAD);
}

void InfoDialog::cancelCurrentUpload()
{
    megaApi->cancelTransfer(activeUpload);
}

void InfoDialog::cancelCurrentDownload()
{
    if (activeDownload->getPublicMegaNode())
    {
        megaApiGuest->cancelTransfer(activeDownload);
    }
    else
    {
        megaApi->cancelTransfer(activeDownload);
    }
}

void InfoDialog::onAllUploadsFinished()
{
    remainingUploads = megaApi->getNumPendingUploads() + megaApiGuest->getNumPendingUploads();
    if (!remainingUploads)
    {
        ui->wTransfer2->hideTransfer();
        ui->lUploads->setText(QString::fromAscii(""));
        ui->wUploadDesc->hide();
        uploadStartTime = 0;
        uploadSpeed = 0;
        currentUpload = 0;
        totalUploads = 0;
        totalUploadedSize = 0;
        totalUploadSize = 0;
        megaApi->resetTotalUploads();
        megaApiGuest->resetTotalUploads();
    }
}

void InfoDialog::onAllDownloadsFinished()
{
    remainingDownloads = megaApi->getNumPendingDownloads() + megaApiGuest->getNumPendingDownloads();
    if (!remainingDownloads)
    {
        if (!preferences->logged())
        {
            gWidget->getTransfer()->hideTransfer();
            gWidget->setDownloadLabel(QString::fromAscii(""));
            gWidget->hideDownloads();
        }
        else
        {
            ui->wTransfer1->hideTransfer();
            ui->lDownloads->setText(QString::fromAscii(""));
            ui->wDownloadDesc->hide();
        }
        downloadStartTime = 0;
        downloadSpeed = 0;
        currentDownload = 0;
        totalDownloads = 0;
        totalDownloadedSize = 0;
        totalDownloadSize = 0;
        megaApi->resetTotalDownloads();
        megaApiGuest->resetTotalDownloads();
    }
}

void InfoDialog::onAllTransfersFinished()
{
    if (!remainingDownloads && !remainingUploads)
    {
        if (ui->sActiveTransfers->currentWidget() != ui->pUpdated)
        {
            ui->sActiveTransfers->setCurrentWidget(ui->pUpdated);
        }
        else if (!preferences->logged() && !gWidget->idleState())
        {
            gWidget->setIdleState(true);
        }

        if (preferences->logged())
        {
            app->updateUserStats();
        }

        app->showNotificationMessage(tr("All transfers have been completed"));
    }
}


void InfoDialog::on_bSettings_clicked()
{   
    QPoint p = ui->bSettings->mapToGlobal(QPoint(ui->bSettings->width()-6, ui->bSettings->height()));

#ifdef __APPLE__
    QPointer<InfoDialog> iod = this;
#endif

    app->showTrayMenu(&p);

#ifdef __APPLE__
    if (!iod)
    {
        return;
    }

    if (!this->rect().contains(this->mapFromGlobal(QCursor::pos())))
    {
        this->hide();
    }
#endif
}

void InfoDialog::on_bOfficialWeb_clicked()
{
    QString webUrl = QString::fromAscii("https://mega.nz/");
    QtConcurrent::run(QDesktopServices::openUrl, QUrl(webUrl));
}

void InfoDialog::on_bSyncFolder_clicked()
{
    int num = preferences->getNumSyncedFolders();

    MegaNode *rootNode = megaApi->getRootNode();
    if (!rootNode)
    {
        preferences->setCrashed(true);
        return;
    }

    if ((num == 1) && (preferences->getMegaFolderHandle(0) == rootNode->getHandle()))
    {
        openFolder(preferences->getLocalFolder(0));
    }
    else
    {
        syncsMenu = new QMenu();

#if (QT_VERSION == 0x050500) && defined(_WIN32)
        syncsMenu->installEventFilter(app);
#endif

        #ifndef __APPLE__
            syncsMenu->setStyleSheet(QString::fromAscii(
                    "QMenu {background-color: white; border: 2px solid #B8B8B8; padding: 5px; border-radius: 5px;} "
                    "QMenu::item {background-color: white; color: black;} "
                    "QMenu::item:selected {background-color: rgb(242, 242, 242);}"));
        #else
            syncsMenu->setStyleSheet(QString::fromAscii("QMenu {padding-left: -10px; padding-top: 4px; } "
                    "QMenu::separator {height: 8px; margin: 0px; }"));
        #endif
        QAction *addSyncAction = syncsMenu->addAction(tr("Add Sync"), this, SLOT(addSync()));
#ifdef __APPLE__
        addSyncAction->setIcon(QIcon(QString::fromAscii("://images/tray_add_sync_ico.png")));
#else
        addSyncAction->setIcon(QIcon(QString::fromAscii("://images/tray_add_sync_ico2.png")));
#endif
        addSyncAction->setIconVisibleInMenu(true);
        syncsMenu->addSeparator();

        QSignalMapper *menuSignalMapper = new QSignalMapper();
        int activeFolders = 0;
        for (int i = 0; i < num; i++)
        {
            if (!preferences->isFolderActive(i))
            {
                continue;
            }

            activeFolders++;
            QAction *action = syncsMenu->addAction(preferences->getSyncName(i), menuSignalMapper, SLOT(map()));
#ifdef __APPLE__
            action->setIcon(QIcon(QString::fromAscii("://images/tray_sync_ico.png")));
#else
            action->setIcon(QIcon(QString::fromAscii("://images/tray_sync_ico2.png")));
#endif
            action->setIconVisibleInMenu(true);
            menuSignalMapper->setMapping(action, preferences->getLocalFolder(i));
            connect(menuSignalMapper, SIGNAL(mapped(QString)), this, SLOT(openFolder(QString)));
        }

        connect(syncsMenu, SIGNAL(aboutToHide()), syncsMenu, SLOT(deleteLater()));
        connect(syncsMenu, SIGNAL(destroyed(QObject*)), menuSignalMapper, SLOT(deleteLater()));

#ifdef __APPLE__
        syncsMenu->exec(this->mapToGlobal(QPoint(20, this->height() - (activeFolders + 1) * 28 - (activeFolders ? 16 : 8))));
        if (!this->rect().contains(this->mapFromGlobal(QCursor::pos())))
        {
            this->hide();
        }
#else
        syncsMenu->popup(ui->bSyncFolder->mapToGlobal(QPoint(0, -activeFolders*35)));
#endif
        syncsMenu = NULL;
    }
    delete rootNode;
}

void InfoDialog::openFolder(QString path)
{
    QtConcurrent::run(QDesktopServices::openUrl, QUrl::fromLocalFile(path));
}

void InfoDialog::updateRecentFiles()
{
    ui->wRecent1->updateWidget();
    ui->wRecent2->updateWidget();
    ui->wRecent3->updateWidget();
}

void InfoDialog::disableGetLink(bool disable)
{
    ui->wRecent1->disableGetLink(disable);
    ui->wRecent2->disableGetLink(disable);
    ui->wRecent3->disableGetLink(disable);
}

void InfoDialog::addSync(MegaHandle h)
{
    static BindFolderDialog *dialog = NULL;
    if (dialog)
    {
        if (h != mega::INVALID_HANDLE)
        {
            dialog->setMegaFolder(h);
        }

        dialog->activateWindow();
        dialog->raise();
        dialog->setFocus();
        return;
    }

    dialog = new BindFolderDialog(app);
    if (h != mega::INVALID_HANDLE)
    {
        dialog->setMegaFolder(h);
    }

    int result = dialog->exec();
    if (result != QDialog::Accepted)
    {
        delete dialog;
        dialog = NULL;
        return;
    }

    QString localFolderPath = QDir::toNativeSeparators(QDir(dialog->getLocalFolder()).canonicalPath());
    MegaHandle handle = dialog->getMegaFolder();
    MegaNode *node = megaApi->getNodeByHandle(handle);
    QString syncName = dialog->getSyncName();
    delete dialog;
    dialog = NULL;
    if (!localFolderPath.length() || !node)
    {
        delete node;
        return;
    }

   const char *nPath = megaApi->getNodePath(node);
   if (!nPath)
   {
       delete node;
       return;
   }

   preferences->addSyncedFolder(localFolderPath, QString::fromUtf8(nPath), handle, syncName);
   delete [] nPath;
   megaApi->syncFolder(localFolderPath.toUtf8().constData(), node);
   delete node;
   updateSyncsButton();
}

#ifdef __APPLE__
void InfoDialog::moveArrow(QPoint p)
{
    arrow->move(p.x()-(arrow->width()/2+1), 2);
    arrow->show();
}
#endif

void InfoDialog::on_bPause_clicked()
{
    app->pauseTransfers(ui->bPause->isChecked());
}

void InfoDialog::onOverlayClicked()
{
    ui->bPause->setChecked(false);
    on_bPause_clicked();
}

void InfoDialog::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange)
    {
        ui->retranslateUi(this);
        if (preferences->logged())
        {
            if (preferences->totalStorage())
            {
                setUsage();
            }
            updateSyncsButton();
            updateTransfers();
        }
    }
    QDialog::changeEvent(event);
}

void InfoDialog::regenerateLayout()
{
    static bool loggedInMode = true;

    if (loggedInMode == preferences->logged())
    {
        return;
    }
    loggedInMode = preferences->logged();

    QLayout *dialogLayout = layout();
    if (!loggedInMode)
    {
        if (!gWidget)
        {
            gWidget = new GuestWidget();
            connect(gWidget, SIGNAL(actionButtonClicked(int)), this, SLOT(onUserAction(int)));
            connect(gWidget, SIGNAL(cancelCurrentDownload()), this, SLOT(cancelCurrentDownload()));
            connect(gWidget, SIGNAL(cancelAllDownloads()), this, SLOT(cancelAllDownloads()));
            connect(gWidget, SIGNAL(pauseClicked()), this, SLOT(onOverlayClicked()));
        }

        ui->bSyncFolder->setVisible(false);
        dialogLayout->removeWidget(ui->sActiveTransfers);
        ui->sActiveTransfers->setVisible(false);
        dialogLayout->removeWidget(ui->wUsage);
        ui->wUsage->setVisible(false);
        dialogLayout->addWidget(gWidget);
        gWidget->setVisible(true);

        ((QVBoxLayout *)dialogLayout)->insertWidget(dialogLayout->count(), ui->wRecent);
        ((QVBoxLayout *)dialogLayout)->insertWidget(dialogLayout->count(), ui->wBottom);

        overlay->setVisible(false);
    }
    else
    {
        ui->bSyncFolder->setVisible(true);
        dialogLayout->removeWidget(gWidget);
        gWidget->setVisible(false);
        dialogLayout->addWidget(ui->sActiveTransfers);
        ui->sActiveTransfers->setVisible(true);

        ((QVBoxLayout *)dialogLayout)->insertWidget(dialogLayout->count(), ui->wRecent);
        dialogLayout->addWidget(ui->wUsage);
        ui->wUsage->setVisible(true);
        ((QVBoxLayout *)dialogLayout)->insertWidget(dialogLayout->count(), ui->wBottom);
    }

    setTransfer(activeDownload);
    updateTransfers();
    app->onGlobalSyncStateChanged(NULL);
}

void InfoDialog::onUserAction(int action)
{
    app->userAction(action);
}
void InfoDialog::scanningAnimationStep()
{
    scanningAnimationIndex = scanningAnimationIndex%18;
    scanningAnimationIndex++;
    QIcon icon;
    icon.addFile(QString::fromUtf8(":/images/scanning_anime")+
                 QString::number(scanningAnimationIndex) + QString::fromUtf8(".png") , QSize(), QIcon::Normal, QIcon::Off);

    ui->label->setIcon(icon);
    ui->label->setIconSize(QSize(64, 64));
}

#ifdef __APPLE__
void InfoDialog::paintEvent( QPaintEvent * e)
{
    QDialog::paintEvent(e);
    QPainter p( this );
    p.setCompositionMode( QPainter::CompositionMode_Clear);
    p.fillRect( ui->wArrow->rect(), Qt::transparent );
}

void InfoDialog::hideEvent(QHideEvent *event)
{
    arrow->hide();
    QDialog::hideEvent(event);
}

void InfoDialog::on_cRecentlyUpdated_stateChanged(int arg1)
{
    ui->wRecent1->hide();
    ui->wRecent2->hide();
    ui->wRecent3->hide();
    ui->cRecentlyUpdated->setEnabled(false);

    if (ui->cRecentlyUpdated->isChecked())
    {
        minHeightAnimation->setTargetObject(this);
        maxHeightAnimation->setTargetObject(this);
        minHeightAnimation->setPropertyName("minimumHeight");
        maxHeightAnimation->setPropertyName("maximumHeight");
        minHeightAnimation->setStartValue(minimumHeight());
        maxHeightAnimation->setStartValue(maximumHeight());
        minHeightAnimation->setEndValue(408);
        maxHeightAnimation->setEndValue(408);
        minHeightAnimation->setDuration(150);
        maxHeightAnimation->setDuration(150);
        animationGroup->start();
    }
    else
    {
        /*minHeightAnimation->setTargetObject(this);
        maxHeightAnimation->setTargetObject(this);
        minHeightAnimation->setPropertyName("minimumHeight");
        maxHeightAnimation->setPropertyName("maximumHeight");
        minHeightAnimation->setStartValue(minimumHeight());
        maxHeightAnimation->setStartValue(maximumHeight());
        minHeightAnimation->setEndValue(552);
        maxHeightAnimation->setEndValue(552);
        minHeightAnimation->setDuration(150);
        maxHeightAnimation->setDuration(150);
        animationGroup->start();*/

        //this->hide();
        this->setMaximumHeight(552);
        this->setMinimumHeight(552);
        onAnimationFinished();
        //this->show();
    }
}

void InfoDialog::onAnimationFinished()
{
    if (this->minimumHeight() == 552)
    {
        ui->wRecent1->show();
        ui->wRecent2->show();
        ui->wRecent3->show();
    }

    ui->lRecentlyUpdated->show();
    ui->cRecentlyUpdated->show();
    ui->wRecentlyUpdated->show();
    ui->cRecentlyUpdated->setEnabled(true);
}


void InfoDialog::showRecentList()
{
    on_cRecentlyUpdated_stateChanged(0);
}
#endif

#ifndef Q_OS_LINUX
void InfoDialog::on_bOfficialWebIcon_clicked()
{
    on_bOfficialWeb_clicked();
}
#endif

