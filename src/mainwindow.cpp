/*
 * Copyright (C) 2009, 2010, 2011, 2013, 2014 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

// This file implements MainWindow. It is responsible for creating and
// managing the menus, the toolbar, and the CrawlerWidget. It also
// load/save the settings on opening/closing of the app

#include <iostream>
/*#include <QtGui>
#if QT_VERSION >= 0x050000
  #include <QtWidgets>
#endif
*/
#include <cassert>

#include <QAction>
#include <QDesktopWidget>
#include <QMenuBar>
#include <QToolBar>
#include <QFileInfo>
#include <QFileDialog>
#include <QClipboard>
#include <QMessageBox>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QUrl>

#include "log.h"

#include "mainwindow.h"

#include "sessioninfo.h"
#include "recentfiles.h"
#include "crawlerwidget.h"
#include "filtersdialog.h"
#include "optionsdialog.h"
#include "persistentinfo.h"
#include "menuactiontooltipbehavior.h"
#include "tabbedcrawlerwidget.h"

// Returns the size in human readable format
static QString readableSize( qint64 size );

MainWindow::MainWindow( std::unique_ptr<Session> session ) :
    session_( std::move( session )  ),
    recentFiles_( Persistent<RecentFiles>( "recentFiles" ) ),
    mainIcon_(),
    signalMux_(),
    quickFindMux_( session_->getQuickFindPattern() ),
    mainTabWidget_()
{
    createActions();
    createMenus();
    createToolBars();
    // createStatusBar();

    setAcceptDrops( true );

    // Default geometry
    const QRect geometry = QApplication::desktop()->availableGeometry( this );
    setGeometry( geometry.x() + 20, geometry.y() + 40,
            geometry.width() - 140, geometry.height() - 140 );

    mainIcon_.addFile( ":/images/hicolor/16x16/glogg.png" );
    mainIcon_.addFile( ":/images/hicolor/24x24/glogg.png" );
    mainIcon_.addFile( ":/images/hicolor/32x32/glogg.png" );
    mainIcon_.addFile( ":/images/hicolor/48x48/glogg.png" );

    setWindowIcon( mainIcon_ );

    readSettings();

    // Connect the signals to the mux (they will be forwarded to the
    // "current" crawlerwidget

    // Send actions to the crawlerwidget
    signalMux_.connect( this, SIGNAL( followSet( bool ) ),
            SIGNAL( followSet( bool ) ) );
    signalMux_.connect( this, SIGNAL( optionsChanged() ),
            SLOT( applyConfiguration() ) );
    signalMux_.connect( this, SIGNAL( enteringQuickFind() ),
            SLOT( enteringQuickFind() ) );
    signalMux_.connect( &quickFindWidget_, SIGNAL( close() ),
            SLOT( exitingQuickFind() ) );

    // Actions from the CrawlerWidget
    signalMux_.connect( SIGNAL( followDisabled() ),
            this, SLOT( disableFollow() ) );
    signalMux_.connect( SIGNAL( updateLineNumber( int ) ),
            this, SLOT( lineNumberHandler( int ) ) );

    // Register for progress status bar
    signalMux_.connect( SIGNAL( loadingProgressed( int ) ),
            this, SLOT( updateLoadingProgress( int ) ) );
    signalMux_.connect( SIGNAL( loadingFinished( bool ) ),
            this, SLOT( displayNormalStatus( bool ) ) );

    // Configure the main tabbed widget
    mainTabWidget_.setDocumentMode( true );
    mainTabWidget_.setMovable( true );
    //mainTabWidget_.setTabShape( QTabWidget::Triangular );
    mainTabWidget_.setTabsClosable( true );

    connect( &mainTabWidget_, SIGNAL( tabCloseRequested( int ) ),
            this, SLOT( closeTab( int ) ) );
    connect( &mainTabWidget_, SIGNAL( currentChanged( int ) ),
            this, SLOT( currentTabChanged( int ) ) );

    // Establish the QuickFindWidget and mux ( to send requests from the
    // QFWidget to the right window )
    connect( &quickFindWidget_, SIGNAL( patternConfirmed( const QString&, bool ) ),
             &quickFindMux_, SLOT( confirmPattern( const QString&, bool ) ) );
    connect( &quickFindWidget_, SIGNAL( patternUpdated( const QString&, bool ) ),
             &quickFindMux_, SLOT( setNewPattern( const QString&, bool ) ) );
    connect( &quickFindWidget_, SIGNAL( cancelSearch() ),
             &quickFindMux_, SLOT( cancelSearch() ) );
    connect( &quickFindWidget_, SIGNAL( searchForward() ),
             &quickFindMux_, SLOT( searchForward() ) );
    connect( &quickFindWidget_, SIGNAL( searchBackward() ),
             &quickFindMux_, SLOT( searchBackward() ) );
    connect( &quickFindWidget_, SIGNAL( searchNext() ),
             &quickFindMux_, SLOT( searchNext() ) );

    // QuickFind changes coming from the views
    connect( &quickFindMux_, SIGNAL( patternChanged( const QString& ) ),
             this, SLOT( changeQFPattern( const QString& ) ) );
    connect( &quickFindMux_, SIGNAL( notify( const QFNotification& ) ),
             &quickFindWidget_, SLOT( notify( const QFNotification& ) ) );
    connect( &quickFindMux_, SIGNAL( clearNotification() ),
             &quickFindWidget_, SLOT( clearNotification() ) );

    // Construct the QuickFind bar
    quickFindWidget_.hide();

    QWidget* central_widget = new QWidget();
    QVBoxLayout* main_layout = new QVBoxLayout();
    main_layout->setContentsMargins( 0, 0, 0, 0 );
    main_layout->addWidget( &mainTabWidget_ );
    main_layout->addWidget( &quickFindWidget_ );
    central_widget->setLayout( main_layout );

    setCentralWidget( central_widget );
}

void MainWindow::reloadSession()
{
    int current_file_index = -1;

    for ( auto open_file: session_->restore(
               []() { return new CrawlerWidget(); },
               &current_file_index ) )
    {
        QString file_name = { open_file.first.c_str() };
        CrawlerWidget* crawler_widget = dynamic_cast<CrawlerWidget*>(
                open_file.second );

        assert( crawler_widget );

        mainTabWidget_.addTab( crawler_widget, strippedName( file_name ) );
    }

    if ( current_file_index >= 0 )
        mainTabWidget_.setCurrentIndex( current_file_index );
}

void MainWindow::loadInitialFile( QString fileName )
{
    LOG(logDEBUG) << "loadInitialFile";

    // Is there a file passed as argument?
    if ( !fileName.isEmpty() )
        loadFile( fileName );
}

//
// Private functions
//

// Menu actions
void MainWindow::createActions()
{
    std::shared_ptr<Configuration> config =
        Persistent<Configuration>( "settings" );

    openAction = new QAction(tr("&Open..."), this);
    openAction->setShortcut(QKeySequence::Open);
    openAction->setIcon( QIcon(":/images/open16.png") );
    openAction->setStatusTip(tr("Open a file"));
    connect(openAction, SIGNAL(triggered()), this, SLOT(open()));

    // Recent files
    for (int i = 0; i < MaxRecentFiles; ++i) {
        recentFileActions[i] = new QAction(this);
        recentFileActions[i]->setVisible(false);
        connect(recentFileActions[i], SIGNAL(triggered()),
                this, SLOT(openRecentFile()));
    }

    exitAction = new QAction(tr("E&xit"), this);
    exitAction->setShortcut(tr("Ctrl+Q"));
    exitAction->setStatusTip(tr("Exit the application"));
    connect( exitAction, SIGNAL(triggered()), this, SLOT(close()) );

    copyAction = new QAction(tr("&Copy"), this);
    copyAction->setShortcut(QKeySequence::Copy);
    copyAction->setStatusTip(tr("Copy the selection"));
    connect( copyAction, SIGNAL(triggered()), this, SLOT(copy()) );

    selectAllAction = new QAction(tr("Select &All"), this);
    selectAllAction->setShortcut(tr("Ctrl+A"));
    selectAllAction->setStatusTip(tr("Select all the text"));
    connect( selectAllAction, SIGNAL(triggered()),
             this, SLOT( selectAll() ) );

    findAction = new QAction(tr("&Find..."), this);
    findAction->setShortcut(QKeySequence::Find);
    findAction->setStatusTip(tr("Find the text"));
    connect( findAction, SIGNAL(triggered()),
            this, SLOT( find() ) );

    overviewVisibleAction = new QAction( tr("Matches &overview"), this );
    overviewVisibleAction->setCheckable( true );
    overviewVisibleAction->setChecked( config->isOverviewVisible() );
    connect( overviewVisibleAction, SIGNAL( toggled( bool ) ),
            this, SLOT( toggleOverviewVisibility( bool )) );

    lineNumbersVisibleInMainAction =
        new QAction( tr("Line &numbers in main view"), this );
    lineNumbersVisibleInMainAction->setCheckable( true );
    lineNumbersVisibleInMainAction->setChecked( config->mainLineNumbersVisible() );
    connect( lineNumbersVisibleInMainAction, SIGNAL( toggled( bool ) ),
            this, SLOT( toggleMainLineNumbersVisibility( bool )) );

    lineNumbersVisibleInFilteredAction =
        new QAction( tr("Line &numbers in filtered view"), this );
    lineNumbersVisibleInFilteredAction->setCheckable( true );
    lineNumbersVisibleInFilteredAction->setChecked( config->filteredLineNumbersVisible() );
    connect( lineNumbersVisibleInFilteredAction, SIGNAL( toggled( bool ) ),
            this, SLOT( toggleFilteredLineNumbersVisibility( bool )) );

    followAction = new QAction( tr("&Follow File"), this );
    followAction->setShortcut(Qt::Key_F);
    followAction->setCheckable(true);
    connect( followAction, SIGNAL(toggled( bool )),
            this, SIGNAL(followSet( bool )) );

    reloadAction = new QAction( tr("&Reload"), this );
    reloadAction->setShortcut(QKeySequence::Refresh);
    reloadAction->setIcon( QIcon(":/images/reload16.png") );
    signalMux_.connect( reloadAction, SIGNAL(triggered()), SLOT(reload()) );

    stopAction = new QAction( tr("&Stop"), this );
    stopAction->setIcon( QIcon(":/images/stop16.png") );
    stopAction->setEnabled( true );
    signalMux_.connect( stopAction, SIGNAL(triggered()), SLOT(stopLoading()) );

    filtersAction = new QAction(tr("&Filters..."), this);
    filtersAction->setStatusTip(tr("Show the Filters box"));
    connect( filtersAction, SIGNAL(triggered()), this, SLOT(filters()) );

    optionsAction = new QAction(tr("&Options..."), this);
    optionsAction->setStatusTip(tr("Show the Options box"));
    connect( optionsAction, SIGNAL(triggered()), this, SLOT(options()) );

    aboutAction = new QAction(tr("&About"), this);
    aboutAction->setStatusTip(tr("Show the About box"));
    connect( aboutAction, SIGNAL(triggered()), this, SLOT(about()) );

    aboutQtAction = new QAction(tr("About &Qt"), this);
    aboutAction->setStatusTip(tr("Show the Qt library's About box"));
    connect( aboutQtAction, SIGNAL(triggered()), this, SLOT(aboutQt()) );
}

void MainWindow::createMenus()
{
    fileMenu = menuBar()->addMenu( tr("&File") );
    fileMenu->addAction( openAction );
    fileMenu->addSeparator();
    for (int i = 0; i < MaxRecentFiles; ++i) {
        fileMenu->addAction( recentFileActions[i] );
        recentFileActionBehaviors[i] =
            new MenuActionToolTipBehavior(recentFileActions[i], fileMenu, this);
    }
    fileMenu->addSeparator();
    fileMenu->addAction( exitAction );

    editMenu = menuBar()->addMenu( tr("&Edit") );
    editMenu->addAction( copyAction );
    editMenu->addAction( selectAllAction );
    editMenu->addSeparator();
    editMenu->addAction( findAction );

    viewMenu = menuBar()->addMenu( tr("&View") );
    viewMenu->addAction( overviewVisibleAction );
    viewMenu->addSeparator();
    viewMenu->addAction( lineNumbersVisibleInMainAction );
    viewMenu->addAction( lineNumbersVisibleInFilteredAction );
    viewMenu->addSeparator();
    viewMenu->addAction( followAction );
    viewMenu->addSeparator();
    viewMenu->addAction( reloadAction );

    toolsMenu = menuBar()->addMenu( tr("&Tools") );
    toolsMenu->addAction( filtersAction );
    toolsMenu->addSeparator();
    toolsMenu->addAction( optionsAction );

    menuBar()->addSeparator();

    helpMenu = menuBar()->addMenu( tr("&Help") );
    helpMenu->addAction( aboutAction );
}

void MainWindow::createToolBars()
{
    infoLine = new InfoLine();
    infoLine->setFrameStyle( QFrame::WinPanel | QFrame::Sunken );
    infoLine->setLineWidth( 0 );

    lineNbField = new QLabel( );
    lineNbField->setText( "Line 0" );
    lineNbField->setAlignment( Qt::AlignLeft | Qt::AlignVCenter );
    lineNbField->setMinimumSize(
            lineNbField->fontMetrics().size( 0, "Line 0000000") );

    toolBar = addToolBar( tr("&Toolbar") );
    toolBar->setIconSize( QSize( 16, 16 ) );
    toolBar->setMovable( false );
    toolBar->addAction( openAction );
    toolBar->addAction( reloadAction );
    toolBar->addWidget( infoLine );
    toolBar->addAction( stopAction );
    toolBar->addWidget( lineNbField );
}

//
// Slots
//

// Opens the file selection dialog to select a new log file
void MainWindow::open()
{
    QString defaultDir = ".";

    // Default to the path of the current file if there is one
    if ( auto current = currentCrawlerWidget() )
    {
        std::string current_file = session_->getFilename( current );
        QFileInfo fileInfo = QFileInfo( QString( current_file.c_str() ) );
        defaultDir = fileInfo.path();
    }

    QString fileName = QFileDialog::getOpenFileName(this,
            tr("Open file"), defaultDir, tr("All files (*)"));
    if (!fileName.isEmpty())
        loadFile(fileName);
}

// Opens a log file from the recent files list
void MainWindow::openRecentFile()
{
    QAction* action = qobject_cast<QAction*>(sender());
    if (action)
        loadFile(action->data().toString());
}

// Select all the text in the currently selected view
void MainWindow::selectAll()
{
    CrawlerWidget* current = currentCrawlerWidget();

    if ( current )
        current->selectAll();
}

// Copy the currently selected line into the clipboard
void MainWindow::copy()
{
    static QClipboard* clipboard = QApplication::clipboard();
    CrawlerWidget* current = currentCrawlerWidget();

    if ( current ) {
        clipboard->setText( current->getSelectedText() );

        // Put it in the global selection as well (X11 only)
        clipboard->setText( current->getSelectedText(),
                QClipboard::Selection );
    }
}

// Display the QuickFind bar
void MainWindow::find()
{
    displayQuickFindBar( QuickFindMux::Forward );
}

// Opens the 'Filters' dialog box
void MainWindow::filters()
{
    FiltersDialog dialog(this);
    signalMux_.connect(&dialog, SIGNAL( optionsChanged() ), SLOT( applyConfiguration() ));
    dialog.exec();
    signalMux_.disconnect(&dialog, SIGNAL( optionsChanged() ), SLOT( applyConfiguration() ));
}

// Opens the 'Options' modal dialog box
void MainWindow::options()
{
    OptionsDialog dialog(this);
    signalMux_.connect(&dialog, SIGNAL( optionsChanged() ), SLOT( applyConfiguration() ));
    dialog.exec();
    signalMux_.disconnect(&dialog, SIGNAL( optionsChanged() ), SLOT( applyConfiguration() ));
}

// Opens the 'About' dialog box.
void MainWindow::about()
{
    QMessageBox::about(this, tr("About glogg"),
            tr("<h2>glogg " GLOGG_VERSION "</h2>"
                "<p>A fast, advanced log explorer."
#ifdef GLOGG_COMMIT
                "<p>Built " GLOGG_DATE " from " GLOGG_COMMIT
#endif
                "<p>Copyright &copy; 2009, 2010, 2011, 2012, 2013, 2014 Nicolas Bonnefon and other contributors"
                "<p>You may modify and redistribute the program under the terms of the GPL (version 3 or later)." ) );
}

// Opens the 'About Qt' dialog box.
void MainWindow::aboutQt()
{
}

void MainWindow::toggleOverviewVisibility( bool isVisible )
{
    std::shared_ptr<Configuration> config =
        Persistent<Configuration>( "settings" );
    config->setOverviewVisible( isVisible );
    emit optionsChanged();
}

void MainWindow::toggleMainLineNumbersVisibility( bool isVisible )
{
    std::shared_ptr<Configuration> config =
        Persistent<Configuration>( "settings" );
    config->setMainLineNumbersVisible( isVisible );
    emit optionsChanged();
}

void MainWindow::toggleFilteredLineNumbersVisibility( bool isVisible )
{
    std::shared_ptr<Configuration> config =
        Persistent<Configuration>( "settings" );
    config->setFilteredLineNumbersVisible( isVisible );
    emit optionsChanged();
}

void MainWindow::disableFollow()
{
    followAction->setChecked( false );
}

void MainWindow::lineNumberHandler( int line )
{
    // The line number received is the internal (starts at 0)
    lineNbField->setText( tr( "Line %1" ).arg( line + 1 ) );
}

void MainWindow::updateLoadingProgress( int progress )
{
    LOG(logDEBUG) << "Loading progress: " << progress;

    QString current_file =
        session_->getFilename( currentCrawlerWidget() ).c_str();

    // We ignore 0% and 100% to avoid a flash when the file (or update)
    // is very short.
    if ( progress > 0 && progress < 100 ) {
        infoLine->setText( current_file +
                tr( " - Indexing lines... (%1 %)" ).arg( progress ) );
        infoLine->displayGauge( progress );

        stopAction->setEnabled( true );
        reloadAction->setEnabled( false );
    }
}

void MainWindow::displayNormalStatus( bool success )
{
    QLocale defaultLocale;

    LOG(logDEBUG) << "displayNormalStatus success=" << success;

    // No file is loading
    loadingFileName.clear();

    if ( success )
    {
        // Following should always work as we will only receive enter
        // this slot if there is a crawler connected.
        QString current_file =
            session_->getFilename( currentCrawlerWidget() ).c_str();

        uint64_t fileSize;
        uint32_t fileNbLine;
        QDateTime lastModified;

        session_->getFileInfo( currentCrawlerWidget(),
                &fileSize, &fileNbLine, &lastModified );
        if ( lastModified.isValid() ) {
            const QString date =
                defaultLocale.toString( lastModified, QLocale::NarrowFormat );
            infoLine->setText( tr( "%1 (%2 - %3 lines - modified on %4)" )
                    .arg(current_file).arg(readableSize(fileSize))
                    .arg(fileNbLine).arg( date ) );
        }
        else {
            infoLine->setText( tr( "%1 (%2 - %3 lines)" )
                    .arg(current_file).arg(readableSize(fileSize))
                    .arg(fileNbLine) );
        }

        infoLine->hideGauge();
        stopAction->setEnabled( false );
        reloadAction->setEnabled( true );

        // Now everything is ready, we can finally show the file!
        currentCrawlerWidget()->show();
    }
    else
    {
        closeTab( mainTabWidget_.currentIndex()  );
    }

    // mainTabWidget_.setEnabled( true );
}

void MainWindow::closeTab( int index )
{
    auto widget = dynamic_cast<CrawlerWidget*>(
            mainTabWidget_.widget( index ) );

    assert( widget );

    widget->stopLoading();
    mainTabWidget_.removeTab( index );
    session_->close( widget );
    delete widget;
}

void MainWindow::currentTabChanged( int index )
{
    LOG(logDEBUG) << "currentTabChanged";

    if ( index >= 0 )
    {
        CrawlerWidget* crawler_widget = dynamic_cast<CrawlerWidget*>(
                mainTabWidget_.widget( index ) );
        signalMux_.setCurrentDocument( crawler_widget );
        quickFindMux_.registerSelector( crawler_widget );

        // New tab is set up with fonts etc...
        emit optionsChanged();

        // Update the title bar
        updateTitleBar( QString(
                    session_->getFilename( crawler_widget ).c_str() ) );
    }
    else
    {
        // No tab left
        signalMux_.setCurrentDocument( nullptr );
        quickFindMux_.registerSelector( nullptr );

        infoLine->hideGauge();
        infoLine->clear();

        updateTitleBar( QString() );
    }
}

void MainWindow::changeQFPattern( const QString& newPattern )
{
    quickFindWidget_.changeDisplayedPattern( newPattern );
}

//
// Events
//

// Closes the application
void MainWindow::closeEvent( QCloseEvent *event )
{
    writeSettings();
    event->accept();
}

// Accepts the drag event if it looks like a filename
void MainWindow::dragEnterEvent( QDragEnterEvent* event )
{
    if ( event->mimeData()->hasFormat( "text/uri-list" ) )
        event->acceptProposedAction();
}

// Tries and loads the file if the URL dropped is local
void MainWindow::dropEvent( QDropEvent* event )
{
    QList<QUrl> urls = event->mimeData()->urls();
    if ( urls.isEmpty() )
        return;

    QString fileName = urls.first().toLocalFile();
    if ( fileName.isEmpty() )
        return;

    loadFile( fileName );
}

void MainWindow::keyPressEvent( QKeyEvent* keyEvent )
{
    LOG(logDEBUG4) << "keyPressEvent received";

    switch ( (keyEvent->text())[0].toLatin1() ) {
        case '/':
            displayQuickFindBar( QuickFindMux::Forward );
            break;
        case '?':
            displayQuickFindBar( QuickFindMux::Backward );
            break;
        default:
            keyEvent->ignore();
    }

    if ( !keyEvent->isAccepted() )
        QMainWindow::keyPressEvent( keyEvent );
}

//
// Private functions
//

// Create a CrawlerWidget for the passed file, start its loading
// and update the title bar.
// The loading is done asynchronously.
bool MainWindow::loadFile( const QString& fileName )
{
    LOG(logDEBUG) << "loadFile ( " << fileName.toStdString() << " )";

    // Load the file
    loadingFileName = fileName;

    try {
        CrawlerWidget* crawler_widget = dynamic_cast<CrawlerWidget*>(
                session_->open( fileName.toStdString(),
                    []() { return new CrawlerWidget(); } ) );
        assert( crawler_widget );

        // We won't show the widget until the file is fully loaded
        crawler_widget->hide();

        // We disable the tab widget to avoid having someone switch
        // tab during loading. (maybe FIXME)
        //mainTabWidget_.setEnabled( false );

        int index = mainTabWidget_.addTab(
                crawler_widget, strippedName( fileName ) );

        // Setting the new tab, the user will see a blank page for the duration
        // of the loading, with no way to switch to another tab
        mainTabWidget_.setCurrentIndex( index );

        // Update the recent files list
        // (reload the list first in case another glogg changed it)
        GetPersistentInfo().retrieve( "recentFiles" );
        recentFiles_->addRecent( fileName );
        GetPersistentInfo().save( "recentFiles" );
        updateRecentFileActions();
    }
    catch ( FileUnreadableErr ) {
        LOG(logDEBUG) << "Can't open file " << fileName.toStdString();
        return false;
    }

    LOG(logDEBUG) << "Success loading file " << fileName.toStdString();
    return true;

}

// Strips the passed filename from its directory part.
QString MainWindow::strippedName( const QString& fullFileName ) const
{
    return QFileInfo( fullFileName ).fileName();
}

// Return the currently active CrawlerWidget, or NULL if none
CrawlerWidget* MainWindow::currentCrawlerWidget() const
{
    auto current = dynamic_cast<CrawlerWidget*>(
            mainTabWidget_.currentWidget() );

    return current;
}

// Update the title bar.
void MainWindow::updateTitleBar( const QString& file_name )
{
    QString shownName = tr( "Untitled" );
    if ( !file_name.isEmpty() )
        shownName = strippedName( file_name );

    setWindowTitle(
            tr("%1 - %2").arg(shownName).arg(tr("glogg"))
#ifdef GLOGG_COMMIT
            + " (dev build " GLOGG_VERSION ")"
#endif
            );
}

// Updates the actions for the recent files.
// Must be called after having added a new name to the list.
void MainWindow::updateRecentFileActions()
{
    QStringList recent_files = recentFiles_->recentFiles();

    for ( int j = 0; j < MaxRecentFiles; ++j ) {
        if ( j < recent_files.count() ) {
            QString text = tr("&%1 %2").arg(j + 1).arg(strippedName(recent_files[j]));
            recentFileActions[j]->setText( text );
            recentFileActions[j]->setToolTip( recent_files[j] );
            recentFileActions[j]->setData( recent_files[j] );
            recentFileActions[j]->setVisible( true );
        }
        else {
            recentFileActions[j]->setVisible( false );
        }
    }

    // separatorAction->setVisible(!recentFiles.isEmpty());
}

// Write settings to permanent storage
void MainWindow::writeSettings()
{
    // Save the session
    // Generate the ordered list of widgets and their topLine
    std::vector<std::pair<const ViewInterface*, uint64_t>> widget_list;
    for ( int i = 0; i < mainTabWidget_.count(); ++i )
        widget_list.push_back( {
                dynamic_cast<const ViewInterface*>( mainTabWidget_.widget( i ) ),
                0 } );
    session_->save( widget_list );
    //SessionInfo& session = Persistent<SessionInfo>( "session" );
    //session.setGeometry( saveGeometry() );
    //session.setCrawlerState( crawlerWidget->saveState() );
    //GetPersistentInfo().save( QString( "session" ) );

    // User settings
    GetPersistentInfo().save( QString( "settings" ) );
}

// Read settings from permanent storage
void MainWindow::readSettings()
{
    // Get and restore the session
    // GetPersistentInfo().retrieve( QString( "session" ) );
    // SessionInfo session = Persistent<SessionInfo>( "session" );
    //restoreGeometry( session.geometry() );
    /*
     * FIXME: should be in the session
    crawlerWidget->restoreState( session.crawlerState() );
    */

    // History of recent files
    GetPersistentInfo().retrieve( QString( "recentFiles" ) );
    updateRecentFileActions();

    // GetPersistentInfo().retrieve( QString( "settings" ) );
    GetPersistentInfo().retrieve( QString( "filterSet" ) );
}

void MainWindow::displayQuickFindBar( QuickFindMux::QFDirection direction )
{
    LOG(logDEBUG) << "MainWindow::displayQuickFindBar";

    // Warn crawlers so they can save the position of the focus in order
    // to do incremental search in the right view.
    emit enteringQuickFind();

    quickFindMux_.setDirection( direction );
    quickFindWidget_.userActivate();
}

// Returns the size in human readable format
static QString readableSize( qint64 size )
{
    static const QString sizeStrs[] = {
        QObject::tr("B"), QObject::tr("KiB"), QObject::tr("MiB"),
        QObject::tr("GiB"), QObject::tr("TiB") };

    QLocale defaultLocale;
    unsigned int i;
    double humanSize = size;

    for ( i=0; i+1 < (sizeof(sizeStrs)/sizeof(QString)) && (humanSize/1024.0) >= 1024.0; i++ )
        humanSize /= 1024.0;

    if ( humanSize >= 1024.0 ) {
        humanSize /= 1024.0;
        i++;
    }

    QString output;
    if ( i == 0 )
        // No decimal part if we display straight bytes.
        output = defaultLocale.toString( (int) humanSize );
    else
        output = defaultLocale.toString( humanSize, 'f', 1 );

    output += QString(" ") + sizeStrs[i];

    return output;
}
