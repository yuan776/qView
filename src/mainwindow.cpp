#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "qvapplication.h"
#include "qvcocoafunctions.h"
#include "qvrenamedialog.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QString>
#include <QGraphicsPixmapItem>
#include <QPixmap>
#include <QClipboard>
#include <QCoreApplication>
#include <QFileSystemWatcher>
#include <QProcess>
#include <QDesktopServices>
#include <QContextMenuEvent>
#include <QMovie>
#include <QImageWriter>
#include <QSettings>
#include <QStyle>
#include <QIcon>
#include <QMimeDatabase>
#include <QScreen>
#include <QCursor>
#include <QInputDialog>
#include <QProgressDialog>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QMenu>
#include <QWindow>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTemporaryFile>

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    // Initialize variables
    justLaunchedWithImage = false;
    storedWindowState = Qt::WindowNoState;

    // Initialize graphicsview
    graphicsView = new QVGraphicsView(this);
    centralWidget()->layout()->addWidget(graphicsView);

    // Hide fullscreen label by default
    ui->fullscreenLabel->hide();

    // Connect graphicsview signals
    connect(graphicsView, &QVGraphicsView::fileChanged, this, &MainWindow::fileChanged);
    connect(graphicsView, &QVGraphicsView::updatedLoadedPixmapItem, this, &MainWindow::setWindowSize);
    connect(graphicsView, &QVGraphicsView::cancelSlideshow, this, &MainWindow::cancelSlideshow);

    // Initialize escape shortcut
    escShortcut = new QShortcut(Qt::Key_Escape, this);
    connect(escShortcut, &QShortcut::activated, this, [this](){
        if (windowState() == Qt::WindowFullScreen)
            toggleFullScreen();
    });

    // Enable drag&dropping
    setAcceptDrops(true);

    // Make info dialog object
    info = new QVInfoDialog(this);

    // Timer for slideshow
    slideshowTimer = new QTimer(this);
    connect(slideshowTimer, &QTimer::timeout, this, &MainWindow::slideshowAction);

    // Context menu
    auto &actionManager = qvApp->getActionManager();

    contextMenu = new QMenu(this);

    actionManager.addCloneOfAction(contextMenu, "open");
    actionManager.addCloneOfAction(contextMenu, "openurl");
    contextMenu->addMenu(actionManager.buildRecentsMenu(true, contextMenu));
    contextMenu->addMenu(actionManager.buildOpenWithMenu(contextMenu));
    actionManager.addCloneOfAction(contextMenu, "opencontainingfolder");
    actionManager.addCloneOfAction(contextMenu, "showfileinfo");
    contextMenu->addSeparator();
    actionManager.addCloneOfAction(contextMenu, "rename");
    actionManager.addCloneOfAction(contextMenu, "delete");
    contextMenu->addSeparator();
    actionManager.addCloneOfAction(contextMenu, "nextfile");
    actionManager.addCloneOfAction(contextMenu, "previousfile");
    contextMenu->addSeparator();
    contextMenu->addMenu(actionManager.buildViewMenu(true, contextMenu));
    contextMenu->addMenu(actionManager.buildToolsMenu(true, contextMenu));
    contextMenu->addMenu(actionManager.buildHelpMenu(true, contextMenu));

    connect(contextMenu, &QMenu::triggered, this, [this](QAction *triggeredAction){
        ActionManager::actionTriggered(triggeredAction, this);
    });

    // Initialize menubar
    setMenuBar(actionManager.buildMenuBar(this));
    // Stop actions conflicting with the window's actions
    const auto menubarActions = ActionManager::getAllNestedActions(menuBar()->actions());
    for (auto action : menubarActions)
    {
        action->setShortcutContext(Qt::WidgetShortcut);
    }
    connect(menuBar(), &QMenuBar::triggered, this, [this](QAction *triggeredAction){
        ActionManager::actionTriggered(triggeredAction, this);
    });

    // Add all actions to this window so keyboard shortcuts are always triggered
    // using virtual menu to hold them so i can connect to the triggered signal
    virtualMenu = new QMenu(this);
    const auto &actionKeys = actionManager.getActionLibrary().keys();
    for (const QString &key : actionKeys)
    {
        actionManager.addCloneOfAction(virtualMenu, key);
    }
    addActions(virtualMenu->actions());
    connect(virtualMenu, &QMenu::triggered, this, [this](QAction *triggeredAction){
       ActionManager::actionTriggered(triggeredAction, this);
    });

    // Connect functions to application components
    connect(&qvApp->getShortcutManager(), &ShortcutManager::shortcutsUpdated, this, &MainWindow::shortcutsUpdated);
    connect(&qvApp->getSettingsManager(), &SettingsManager::settingsUpdated, this, &MainWindow::settingsUpdated);
    settingsUpdated();
    shortcutsUpdated();

    // Timer for delayed-load Open With menu
    populateOpenWithTimer = new QTimer(this);
    populateOpenWithTimer->setSingleShot(true);
    populateOpenWithTimer->setInterval(250);
    connect(populateOpenWithTimer, &QTimer::timeout, this, &MainWindow::requestPopulateOpenWithMenu);

    // Connection for open with menu population futurewatcher
    connect(&openWithFutureWatcher, &QFutureWatcher<QList<OpenWith::OpenWithItem>>::finished, this, [this](){
        populateOpenWithMenu(openWithFutureWatcher.result());
    });

#ifdef COCOA_LOADED
    QVCocoaFunctions::setFullSizeContentView(windowHandle());
#endif

    // Load window geometry
    QSettings settings;
    restoreGeometry(settings.value("geometry").toByteArray());

    // Show welcome dialog on first launch
    if (!settings.value("firstlaunch", false).toBool())
    {
        settings.setValue("firstlaunch", true);
        settings.setValue("configversion", VERSION);
        qvApp->openWelcomeDialog(this);
    }
}

MainWindow::~MainWindow()
{
    delete ui;
}

bool MainWindow::event(QEvent *event)
{
    if (event->type() == QEvent::WindowActivate)
    {
        qvApp->addToLastActiveWindows(this);
    }
    return QMainWindow::event(event);
}

void MainWindow::contextMenuEvent(QContextMenuEvent *event)
{
    QMainWindow::contextMenuEvent(event);

    // Show native menu on macOS with cocoa framework loaded
#ifdef COCOA_LOADED
    // On regular context menu, recents submenu updates right before it is shown.
    // The native cocoa menu does not update elements until the entire menu is reopened, so we update first
    qvApp->getActionManager().loadRecentsList();
    QVCocoaFunctions::showMenu(contextMenu, event->pos(), windowHandle());
#else
    contextMenu->popup(event->globalPos());
#endif
}

void MainWindow::showEvent(QShowEvent *event)
{
    if (!menuBar()->sizeHint().isEmpty())
    {
        ui->fullscreenLabel->setMargin(0);
        ui->fullscreenLabel->setMinimumHeight(menuBar()->sizeHint().height());
    }

    QMainWindow::showEvent(event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QSettings settings;
    settings.setValue("geometry", saveGeometry());

    qvApp->deleteFromLastActiveWindows(this);
    qvApp->getActionManager().untrackClonedActions(contextMenu);
    qvApp->getActionManager().untrackClonedActions(menuBar());
    qvApp->getActionManager().untrackClonedActions(virtualMenu);

    QMainWindow::closeEvent(event);
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::WindowStateChange)
    {
        const auto fullscreenActions = qvApp->getActionManager().getAllClonesOfAction("fullscreen", this);
        for (const auto &fullscreenAction : fullscreenActions)
        {
            if (windowState() == Qt::WindowFullScreen)
            {
                fullscreenAction->setText(tr("Exit F&ull Screen"));
                fullscreenAction->setIcon(QIcon::fromTheme("view-restore"));
            }
            else
            {
                fullscreenAction->setText(tr("Enter F&ull Screen"));
                fullscreenAction->setIcon(QIcon::fromTheme("view-fullscreen"));
            }
        }

        if (qvApp->getSettingsManager().getBoolean("fullscreendetails"))
            ui->fullscreenLabel->setVisible(windowState() == Qt::WindowFullScreen);
    }
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MouseButton::BackButton)
        previousFile();
    else if (event->button() == Qt::MouseButton::ForwardButton)
        nextFile();
    else if (event->button() == Qt::MouseButton::MiddleButton)
        resetZoom();

    QMainWindow::mousePressEvent(event);
}

void MainWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MouseButton::LeftButton)
        toggleFullScreen();
    QMainWindow::mouseDoubleClickEvent(event);
}

void MainWindow::openFile(const QString &fileName)
{
    graphicsView->loadFile(fileName);
    cancelSlideshow();
}

void MainWindow::settingsUpdated()
{
    auto &settingsManager = qvApp->getSettingsManager();

    buildWindowTitle();

    // menubarenabled
    bool menuBarEnabled = settingsManager.getBoolean("menubarenabled");
#ifdef Q_OS_MACOS
    // Menu bar is effectively always enabled on macOS
    menuBarEnabled = true;
#endif
    menuBar()->setVisible(menuBarEnabled);

#ifdef COCOA_LOADED
    // titlebaralwaysdark
    QVCocoaFunctions::setVibrancy(settingsManager.getBoolean("titlebaralwaysdark"), windowHandle());
    // quitonlastwindow
    qvApp->setQuitOnLastWindowClosed(settingsManager.getBoolean("quitonlastwindow"));
#endif

    //slideshow timer
    slideshowTimer->setInterval(static_cast<int>(settingsManager.getDouble("slideshowtimer")*1000));


    ui->fullscreenLabel->setVisible(qvApp->getSettingsManager().getBoolean("fullscreendetails") && (windowState() == Qt::WindowFullScreen));

    setWindowSize();
}

void MainWindow::shortcutsUpdated()
{
    // If esc is not used in a shortcut, let it exit fullscreen
    escShortcut->setKey(Qt::Key_Escape);

    const auto &actionLibrary = qvApp->getActionManager().getActionLibrary();
    for (const auto &action : actionLibrary)
    {
        if (action->shortcuts().contains(QKeySequence(Qt::Key_Escape)))
        {
            escShortcut->setKey({});
            break;
        }
    }
}

void MainWindow::openRecent(int i)
{
    auto recentsList = qvApp->getActionManager().getRecentsList();
    graphicsView->loadFile(recentsList.value(i).filePath);
    cancelSlideshow();
}

void MainWindow::fileChanged()
{
    populateOpenWithTimer->start();
    disableActions();

    refreshProperties();
    buildWindowTitle();
}

void MainWindow::disableActions()
{
    const auto &actionLibrary = qvApp->getActionManager().getActionLibrary();
    for (const auto &action : actionLibrary)
    {
        const auto &data = action->data().toStringList();
        const auto &clonesOfAction = qvApp->getActionManager().getAllClonesOfAction(data.first(), this);

        // Enable this window's actions when a file is loaded
        if (data.last().contains("disable"))
        {
            for (const auto &clone : clonesOfAction)
            {
                const auto &cloneData = clone->data().toStringList();
                if (cloneData.last() == "disable")
                {
                    clone->setEnabled(getCurrentFileDetails().isPixmapLoaded);
                }
                else if (cloneData.last() == "gifdisable")
                {
                    clone->setEnabled(getCurrentFileDetails().isMovieLoaded);
                }
                else if (cloneData.last() == "undodisable")
                {
                    clone->setEnabled(!lastDeletedFiles.isEmpty() && !lastDeletedFiles.top().pathInTrash.isEmpty());
                }
                else if (cloneData.last() == "folderdisable")
                {
                    clone->setEnabled(!getCurrentFileDetails().folderFileInfoList.isEmpty());
                }
            }
        }
    }

    const auto &openWithMenus = qvApp->getActionManager().getAllClonesOfMenu("openwith");
    for (const auto &menu : openWithMenus)
    {
        menu->setEnabled(getCurrentFileDetails().isPixmapLoaded);
    }
}

void MainWindow::requestPopulateOpenWithMenu()
{
    openWithFutureWatcher.setFuture(QtConcurrent::run([&]{
        const auto &curFilePath = getCurrentFileDetails().fileInfo.absoluteFilePath();
        return OpenWith::getOpenWithItems(curFilePath);
    }));
}

void MainWindow::populateOpenWithMenu(const QList<OpenWith::OpenWithItem> openWithItems)
{
    for (int i = 0; i < qvApp->getActionManager().getOpenWithMaxLength(); i++)
    {
        const auto clonedActions = qvApp->getActionManager().getAllClonesOfAction("openwith" + QString::number(i), this);
        for (const auto &action : clonedActions)
        {
            // If we are within the bounds of the open with list
            if (i < openWithItems.length())
            {
                auto openWithItem = openWithItems.value(i);

                action->setVisible(true);
                action->setIconVisibleInMenu(false); // Hide icon temporarily to speed up updates in certain cases
                action->setText(openWithItem.name);
                if (!openWithItem.iconName.isEmpty())
                    action->setIcon(QIcon::fromTheme(openWithItem.iconName));
                else
                    action->setIcon(openWithItem.icon);
                auto data = action->data().toList();
                data.replace(1, QVariant::fromValue(openWithItem));
                action->setData(data);
                action->setIconVisibleInMenu(true);
            }
            else
            {
                action->setVisible(false);
            }
        }
    }
}

void MainWindow::refreshProperties()
{
    int value4;
    if (getCurrentFileDetails().isMovieLoaded)
        value4 = graphicsView->getLoadedMovie().frameCount();
    else
        value4 = 0;
    info->setInfo(getCurrentFileDetails().fileInfo, getCurrentFileDetails().baseImageSize.width(), getCurrentFileDetails().baseImageSize.height(), value4);
}

void MainWindow::buildWindowTitle()
{
    QString newString = "qView";
    if (getCurrentFileDetails().fileInfo.isFile())
    {
        switch (qvApp->getSettingsManager().getInteger("titlebarmode")) {
        case 1:
        {
            newString = getCurrentFileDetails().fileInfo.fileName();
            break;
        }
        case 2:
        {
            newString = QString::number(getCurrentFileDetails().loadedIndexInFolder+1);
            newString += "/" + QString::number(getCurrentFileDetails().folderFileInfoList.count());
            newString += " - " + getCurrentFileDetails().fileInfo.fileName();
            break;
        }
        case 3:
        {
            newString = QString::number(getCurrentFileDetails().loadedIndexInFolder+1);
            newString += "/" + QString::number(getCurrentFileDetails().folderFileInfoList.count());
            newString += " - " + getCurrentFileDetails().fileInfo.fileName();
            newString += " - "  + QString::number(getCurrentFileDetails().baseImageSize.width());
            newString += "x" + QString::number(getCurrentFileDetails().baseImageSize.height());
            newString += " - " + QVInfoDialog::formatBytes(getCurrentFileDetails().fileInfo.size());
            newString += " - qView";
            break;
        }
        }
    }

    setWindowTitle(newString);

    // Update fullscreen label to titlebar text as well
    ui->fullscreenLabel->setText(newString);

    if (windowHandle() != nullptr)
    {
        if (getCurrentFileDetails().isPixmapLoaded)
            windowHandle()->setFilePath(getCurrentFileDetails().fileInfo.absoluteFilePath());
        else
            windowHandle()->setFilePath("");
    }
}

void MainWindow::setWindowSize()
{
    if (!getCurrentFileDetails().isPixmapLoaded)
        return;

    //check if the program is configured to resize the window
    int windowResizeMode = qvApp->getSettingsManager().getInteger("windowresizemode");
    if (!(windowResizeMode == 2 || (windowResizeMode == 1 && justLaunchedWithImage)))
        return;

    justLaunchedWithImage = false;

    //check if window is maximized or fullscreened
    if (windowState() == Qt::WindowMaximized || windowState() == Qt::WindowFullScreen)
        return;


    qreal minWindowResizedPercentage = qvApp->getSettingsManager().getInteger("minwindowresizedpercentage")/100.0;
    qreal maxWindowResizedPercentage = qvApp->getSettingsManager().getInteger("maxwindowresizedpercentage")/100.0;


    QSize imageSize = getCurrentFileDetails().loadedPixmapSize;
    imageSize -= QSize(4, 4);


    // Try to grab the current screen
    QScreen *currentScreen = screenContaining(frameGeometry());

    // makeshift validity check
    bool screenValid = QGuiApplication::screens().contains(currentScreen);
    // Use first screen as fallback
    if (!screenValid)
        currentScreen = QGuiApplication::screens().at(0);

    QSize extraWidgetsSize { 0, 0 };

    if (menuBar()->isVisible())
        extraWidgetsSize.rheight() += menuBar()->height();

    int titlebarOverlap = 0;
#ifdef COCOA_LOADED
    // To account for fullsizecontentview on mac
    titlebarOverlap = QVCocoaFunctions::getObscuredHeight(window()->windowHandle());
#endif
    if (titlebarOverlap != 0)
        extraWidgetsSize.rheight() += titlebarOverlap;

    const QSize windowFrameSize = frameGeometry().size() - geometry().size();
    const QSize hardLimitSize = currentScreen->availableSize() - windowFrameSize - extraWidgetsSize;
    const QSize screenSize = currentScreen->size();
    const QSize minWindowSize = (screenSize * minWindowResizedPercentage).boundedTo(hardLimitSize);
    const QSize maxWindowSize = (screenSize * maxWindowResizedPercentage).boundedTo(hardLimitSize);

    if (imageSize.width() < minWindowSize.width() && imageSize.height() < minWindowSize.height())
    {
        imageSize.scale(minWindowSize, Qt::KeepAspectRatio);
    }
    else if (imageSize.width() > maxWindowSize.width() || imageSize.height() > maxWindowSize.height())
    {
        imageSize.scale(maxWindowSize, Qt::KeepAspectRatio);
    }

    // Windows reports the wrong minimum width, so we constrain the image size relative to the dpi to stop weirdness with tiny images
#ifdef Q_OS_WIN
    auto minimumImageSize = QSize(qRound(logicalDpiX()*1.5), logicalDpiY()/2);
    if (imageSize.boundedTo(minimumImageSize) == imageSize)
        imageSize = minimumImageSize;
#endif

    // Match center after new geometry
    // This is smoother than a single geometry set for some reason
    QRect oldRect = geometry();
    resize(imageSize + extraWidgetsSize);
    QRect newRect = geometry();
    newRect.moveCenter(oldRect.center());

    // Ensure titlebar is not above or below the available screen area
    const QRect availableScreenRect = currentScreen->availableGeometry();
    const int topFrameHeight = geometry().top() - frameGeometry().top();
    const int windowMinY = availableScreenRect.top() + topFrameHeight;
    const int windowMaxY = availableScreenRect.top() + availableScreenRect.height() - titlebarOverlap;
    if (newRect.top() < windowMinY)
        newRect.moveTop(windowMinY);
    if (newRect.top() > windowMaxY)
        newRect.moveTop(windowMaxY);

    setGeometry(newRect);
}

// Initially copied from Qt source code (QGuiApplication::screenAt) and then customized
QScreen *MainWindow::screenContaining(const QRect &rect)
{
    QScreen *bestScreen = nullptr;
    int bestScreenArea = 0;
    QVarLengthArray<const QScreen *, 8> visitedScreens;
    const auto screens = QGuiApplication::screens();
    for (const QScreen *screen : screens) {
        if (visitedScreens.contains(screen))
            continue;
        // The virtual siblings include the screen itself, so iterate directly
        const auto siblings = screen->virtualSiblings();
        for (QScreen *sibling : siblings) {
            const QRect intersect = sibling->geometry().intersected(rect);
            const int area = intersect.width() * intersect.height();
            if (area > bestScreenArea) {
                bestScreen = sibling;
                bestScreenArea = area;
            }
            visitedScreens.append(sibling);
        }
    }
    return bestScreen;
}

bool MainWindow::getIsPixmapLoaded() const
{
    return getCurrentFileDetails().isPixmapLoaded;
}

void MainWindow::setJustLaunchedWithImage(bool value)
{
    justLaunchedWithImage = value;
}

void MainWindow::openUrl(const QUrl &url)
{
    if (!url.isValid()) {
        QMessageBox::critical(this, tr("Error"), tr("Error: URL is invalid"));
        return;
    }

    auto request = QNetworkRequest(url);
    auto *reply = networkAccessManager.get(request);
    auto *progressDialog = new QProgressDialog(tr("Downloading image..."), tr("Cancel"), 0, 100);
    progressDialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    progressDialog->setAutoClose(false);
    progressDialog->setAutoReset(false);
    progressDialog->setWindowTitle(tr("Open URL..."));
    progressDialog->open();

    connect(progressDialog, &QProgressDialog::canceled, reply, [reply]{
        reply->abort();
    });

    connect(reply, &QNetworkReply::downloadProgress, progressDialog, [progressDialog](qreal bytesReceived, qreal bytesTotal){
        auto percent = (bytesReceived/bytesTotal)*100;
        progressDialog->setValue(qRound(percent));
    });


    connect(reply, &QNetworkReply::finished, progressDialog, [progressDialog, reply, this]{
        if (reply->error())
        {
            progressDialog->close();
            QMessageBox::critical(this, tr("Error"), tr("Error ") + QString::number(reply->error()) + ": " + reply->errorString());

            progressDialog->deleteLater();
            return;
        }

        progressDialog->setMaximum(0);

        auto *tempFile = new QTemporaryFile(this);
        tempFile->setFileTemplate(QDir::tempPath() + "/" + qvApp->applicationName() + ".XXXXXX.png");

        auto *saveFutureWatcher = new QFutureWatcher<bool>();
        connect(saveFutureWatcher, &QFutureWatcher<bool>::finished, this, [progressDialog, tempFile, saveFutureWatcher, this](){
            progressDialog->close();
            if (saveFutureWatcher->result())
            {
                if (tempFile->open())
                {
                    openFile(tempFile->fileName());
                }
            }
            else
            {
                 QMessageBox::critical(this, tr("Error"), tr("Error: Invalid image"));
                 tempFile->deleteLater();
            }
            progressDialog->deleteLater();
            saveFutureWatcher->deleteLater();
        });

        saveFutureWatcher->setFuture(QtConcurrent::run([reply, tempFile]{
            return QImage::fromData(reply->readAll()).save(tempFile, "png");
        }));
    });
}

void MainWindow::pickUrl()
{
    auto inputDialog = new QInputDialog(this);
    inputDialog->setWindowTitle(tr("Open URL..."));
    inputDialog->setLabelText(tr("URL of a supported image file:"));
    inputDialog->resize(350, inputDialog->height());
    inputDialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    connect(inputDialog, &QInputDialog::finished, this, [inputDialog, this](int result) {
        if (result)
        {
            const auto url = QUrl(inputDialog->textValue());
            openUrl(url);
        }
        inputDialog->deleteLater();
    });
    inputDialog->open();
}

void MainWindow::openWith(const OpenWith::OpenWithItem &openWithItem)
{
    OpenWith::openWith(getCurrentFileDetails().fileInfo.absoluteFilePath(), openWithItem);
}

void MainWindow::openContainingFolder()
{
    if (!getCurrentFileDetails().isPixmapLoaded)
        return;

    const QFileInfo selectedFileInfo = getCurrentFileDetails().fileInfo;

#ifdef Q_OS_WIN
    QProcess::startDetached("explorer", QStringList() << "/select," << QDir::toNativeSeparators(selectedFileInfo.absoluteFilePath()));
#elif defined Q_OS_MACOS
    QProcess::execute("open", QStringList() << "-R" << selectedFileInfo.absoluteFilePath());
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(selectedFileInfo.absolutePath()));
#endif
}

void MainWindow::showFileInfo()
{
    refreshProperties();
    info->show();
    info->raise();
}

void MainWindow::askDeleteFile()
{
    if (!qvApp->getSettingsManager().getBoolean("askdelete"))
    {
        deleteFile();
        return;
    }

    const QFileInfo &fileInfo = getCurrentFileDetails().fileInfo;
    const QString fileName = getCurrentFileDetails().fileInfo.fileName();

    if (!fileInfo.isWritable())
    {
        QMessageBox::critical(this, tr("Error"), tr("Can't delete %1:\nNo write permission or file is read-only.").arg(fileName));
        return;
    }

    auto trashString = tr("Are you sure you want to move %1 to the Trash?").arg(fileName);
#ifdef Q_OS_WIN
    trashString = tr("Are you sure you want to move %1 to the Recycle Bin?").arg(fileName);
#endif

    auto *msgBox = new QMessageBox(QMessageBox::Question, tr("Delete"), trashString,
                       QMessageBox::Yes | QMessageBox::No, this);
    msgBox->setCheckBox(new QCheckBox(tr("Do not ask again")));

    connect(msgBox, &QMessageBox::finished, this, [this, msgBox](int result){
        if (result != 16384)
            return;

        QSettings settings;
        settings.beginGroup("options");
        settings.setValue("askdelete", !msgBox->checkBox()->isChecked());
        qvApp->getSettingsManager().loadSettings();
        this->deleteFile();
    });

    msgBox->open();
}

void MainWindow::deleteFile()
{
    const QFileInfo &fileInfo = getCurrentFileDetails().fileInfo;
    const QString filePath = fileInfo.absoluteFilePath();
    QString trashFilePath = "";

    graphicsView->closeImage();

#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
    const QString fileName = fileInfo.fileName();

    QFile file(filePath);
    bool success = file.moveToTrash();
    if (!success || QFile::exists(filePath))
    {
        openFile(filePath);
        QMessageBox::critical(this, tr("Error"), tr("Can't delete %1.").arg(fileName));
        return;
    }

    trashFilePath = file.fileName();
#elif defined Q_OS_MACOS && COCOA_LOADED
    QString trashedFile = QVCocoaFunctions::deleteFile(filePath);
    trashFilePath = QUrl(trashedFile).toLocalFile(); // remove file:// protocol
#elif defined Q_OS_UNIX && !defined Q_OS_MACOS
    trashFilePath = deleteFileLinuxFallback(filePath, false);
#else
    QMessageBox::critical(this, tr("Not Supported"), tr("This program was compiled with an old version of Qt and this feature is not available.\n"
                                                        "If you see this message, please report a bug!"));

    return;
#endif

    auto afterDelete = qvApp->getSettingsManager().getInteger("afterdelete");
    if (afterDelete > 1)
        nextFile();
    else if (afterDelete < 1)
        previousFile();

    lastDeletedFiles.push({trashFilePath, filePath});
    disableActions();
}

QString MainWindow::deleteFileLinuxFallback(const QString &path, bool putBack)
{
    QStringList gioArgs = {"trash", path};
    if (putBack)
        gioArgs.insert(1, "--restore");

    QProcess process;
    process.start("gio", gioArgs);
    process.waitForFinished();

    if (process.error() != QProcess::FailedToStart && !putBack)
    {
        process.start("gio", {"trash", "--list"});
        process.waitForFinished();

        const auto &output = QString(process.readAllStandardOutput()).split("\n");
        for (const auto &line : output)
        {
            if (line.contains(path))
                return line.split("\t").at(0);
        }
    }


    qWarning("Failed to use linux fallback delete");
    return "";
}

void MainWindow::undoDelete()
{
    if (lastDeletedFiles.isEmpty())
        return;

    const DeletedPaths lastDeletedFile = lastDeletedFiles.pop();
    if (lastDeletedFile.pathInTrash.isEmpty() || lastDeletedFile.previousPath.isEmpty())
        return;

#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)) || (defined Q_OS_MACOS && COCOA_LOADED)
    const QFileInfo fileInfo(lastDeletedFile.pathInTrash);
    if (!fileInfo.isWritable())
    {
        QMessageBox::critical(this, tr("Error"), tr("Can't undo deletion of %1:\n"
                                                    "No write permission or file is read-only.").arg(fileInfo.fileName()));
        return;
    }

    bool success = QFile::rename(lastDeletedFile.pathInTrash, lastDeletedFile.previousPath);
    if (!success)
    {
        QMessageBox::critical(this, tr("Error"), tr("Failed undoing deletion of %1.").arg(fileInfo.fileName()));
    }
#elif defined Q_OS_UNIX && !defined Q_OS_MACOS
    deleteFileLinuxFallback(lastDeletedFile.pathInTrash, true);
#else
    QMessageBox::critical(this, tr("Not Supported"), tr("This program was compiled with an old version of Qt and this feature is not available.\n"
                                                        "If you see this message, please report a bug!"));

    return;
#endif

    openFile(lastDeletedFile.previousPath);
    disableActions();
}

void MainWindow::copy()
{
    auto *mimeData = graphicsView->getMimeData();
    if (!mimeData->hasImage() || !mimeData->hasUrls())
    {
        mimeData->deleteLater();
        return;
    }

    QApplication::clipboard()->setMimeData(mimeData);
}

void MainWindow::paste()
{
    const QMimeData *mimeData = QApplication::clipboard()->mimeData();
    if (mimeData == nullptr)
        return;

    if (mimeData->hasText())
    {
        auto url = QUrl(mimeData->text());

        if (url.isValid() && (url.scheme() == "http" || url.scheme() == "https"))
        {
            openUrl(url);
            return;
        }
    }

    graphicsView->loadMimeData(mimeData);
}

void MainWindow::rename()
{
    if (!getCurrentFileDetails().isPixmapLoaded)
        return;

    auto *renameDialog = new QVRenameDialog(this, getCurrentFileDetails().fileInfo);
    connect(renameDialog, &QVRenameDialog::newFileToOpen, this, &MainWindow::openFile);
    connect(renameDialog, &QVRenameDialog::readyToRenameFile, this, [this] () {
        if (auto device = graphicsView->getLoadedMovie().device()) {
            device->close();
        }
    });

    renameDialog->open();
}

void MainWindow::zoomIn()
{
    graphicsView->zoomIn();
}

void MainWindow::zoomOut()
{
    graphicsView->zoomOut();
}

void MainWindow::resetZoom()
{
    graphicsView->resetScale();
}

void MainWindow::originalSize()
{
    graphicsView->originalSize();
}

void MainWindow::rotateRight()
{
    graphicsView->rotateImage(90);
    resetZoom();
}

void MainWindow::rotateLeft()
{
    graphicsView->rotateImage(-90);
    resetZoom();
}

void MainWindow::mirror()
{
    graphicsView->scale(-1, 1);
    resetZoom();
}

void MainWindow::flip()
{
    graphicsView->scale(1, -1);
    resetZoom();
}

void MainWindow::firstFile()
{
    graphicsView->goToFile(QVGraphicsView::GoToFileMode::first);
}

void MainWindow::previousFile()
{
    graphicsView->goToFile(QVGraphicsView::GoToFileMode::previous);
}

void MainWindow::nextFile()
{
    graphicsView->goToFile(QVGraphicsView::GoToFileMode::next);
}

void MainWindow::lastFile()
{
    graphicsView->goToFile(QVGraphicsView::GoToFileMode::last);
}

void MainWindow::saveFrameAs()
{
    QSettings settings;
    settings.beginGroup("recents");
    if (!getCurrentFileDetails().isMovieLoaded)
        return;

    if (graphicsView->getLoadedMovie().state() == QMovie::Running)
    {
        pause();
    }
    QFileDialog *saveDialog = new QFileDialog(this, tr("Save Frame As..."));
    saveDialog->setDirectory(settings.value("lastFileDialogDir", QDir::homePath()).toString());
    saveDialog->setNameFilters(qvApp->getNameFilterList());
    saveDialog->selectFile(getCurrentFileDetails().fileInfo.baseName() + "-" + QString::number(graphicsView->getLoadedMovie().currentFrameNumber()) + ".png");
    saveDialog->setDefaultSuffix("png");
    saveDialog->setAcceptMode(QFileDialog::AcceptSave);
    saveDialog->open();
    connect(saveDialog, &QFileDialog::fileSelected, this, [=](const QString &fileName){
        graphicsView->originalSize();
        for(int i=0; i < graphicsView->getLoadedMovie().frameCount(); i++)
            nextFrame();

        graphicsView->getLoadedMovie().currentPixmap().save(fileName, nullptr, 100);
        graphicsView->resetScale();
    });
}

void MainWindow::pause()
{
    if (!getCurrentFileDetails().isMovieLoaded)
        return;

    const auto pauseActions = qvApp->getActionManager().getAllClonesOfAction("pause", this);

    if (graphicsView->getLoadedMovie().state() == QMovie::Running)
    {
        graphicsView->setPaused(true);
        for (const auto &pauseAction : pauseActions)
        {
            pauseAction->setText(tr("Res&ume"));
            pauseAction->setIcon(QIcon::fromTheme("media-playback-start"));
        }
    }
    else
    {
        graphicsView->setPaused(false);
        for (const auto &pauseAction : pauseActions)
        {
            pauseAction->setText(tr("Pause"));
            pauseAction->setIcon(QIcon::fromTheme("media-playback-pause"));
        }
    }
}

void MainWindow::nextFrame()
{
    if (!getCurrentFileDetails().isMovieLoaded)
        return;

    graphicsView->jumpToNextFrame();
}

void MainWindow::toggleSlideshow()
{
    const auto slideshowActions = qvApp->getActionManager().getAllClonesOfAction("slideshow", this);

    if (slideshowTimer->isActive())
    {
        slideshowTimer->stop();
        for (const auto &slideshowAction : slideshowActions)
        {
            slideshowAction->setText(tr("Start S&lideshow"));
            slideshowAction->setIcon(QIcon::fromTheme("media-playback-start"));
        }
    }
    else
    {
        slideshowTimer->start();
        for (const auto &slideshowAction : slideshowActions)
        {
            slideshowAction->setText(tr("Stop S&lideshow"));
            slideshowAction->setIcon(QIcon::fromTheme("media-playback-stop"));
        }
    }
}

void MainWindow::cancelSlideshow()
{
    if (slideshowTimer->isActive())
        toggleSlideshow();
}

void MainWindow::slideshowAction()
{
    if (qvApp->getSettingsManager().getBoolean("slideshowreversed"))
        previousFile();
    else
        nextFile();
}

void MainWindow::decreaseSpeed()
{
    if (!getCurrentFileDetails().isMovieLoaded)
        return;

    graphicsView->setSpeed(graphicsView->getLoadedMovie().speed()-25);
}

void MainWindow::resetSpeed()
{
    if (!getCurrentFileDetails().isMovieLoaded)
        return;

    graphicsView->setSpeed(100);
}

void MainWindow::increaseSpeed()
{
    if (!getCurrentFileDetails().isMovieLoaded)
        return;

    graphicsView->setSpeed(graphicsView->getLoadedMovie().speed()+25);
}

void MainWindow::toggleFullScreen()
{
    if (windowState() == Qt::WindowFullScreen)
    {
        setWindowState(storedWindowState);
        setWindowSize();
    }
    else
    {
        storedWindowState = windowState();
        showFullScreen();
    }
}
