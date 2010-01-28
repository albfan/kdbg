/*
 * Copyright Johannes Sixt
 * This file is licensed under the GNU General Public License Version 2.
 * See the file COPYING in the toplevel directory of the source directory.
 */

#include <kapplication.h>
#include <klocale.h>			/* i18n */
#include <kmessagebox.h>
#include <kconfig.h>
#include <kstatusbar.h>
#include <kiconloader.h>
#include <kstdaccel.h>
#include <kstdaction.h>
#include <kaction.h>
#include <kpopupmenu.h>
#include <kfiledialog.h>
#include <kprocess.h>
#include <kkeydialog.h>
#include <kanimwidget.h>
#include <kwin.h>
#include <qlistbox.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qtabdialog.h>
#include "dbgmainwnd.h"
#include "debugger.h"
#include "commandids.h"
#include "winstack.h"
#include "brkpt.h"
#include "threadlist.h"
#include "memwindow.h"
#include "ttywnd.h"
#include "watchwindow.h"
#include "procattach.h"
#include "prefdebugger.h"
#include "prefmisc.h"
#include "gdbdriver.h"
#include "xsldbgdriver.h"
#include "mydebug.h"
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>			/* mknod(2) */
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>			/* getpid */
#endif


static const char defaultTermCmdStr[] = "xterm -name kdbgio -title %T -e sh -c %C";
static const char defaultSourceFilter[] = "*.c *.cc *.cpp *.c++ *.C *.CC";
static const char defaultHeaderFilter[] = "*.h *.hh *.hpp *.h++";

DebuggerMainWnd::DebuggerMainWnd(const char* name) :
	KDockMainWindow(0, name),
	m_debugger(0),
#ifdef GDB_TRANSCRIPT
	m_transcriptFile(GDB_TRANSCRIPT),
#endif
	m_outputTermCmdStr(defaultTermCmdStr),
	m_outputTermProc(0),
	m_ttyLevel(-1),			/* no tty yet */
	m_popForeground(false),
	m_backTimeout(1000),
	m_tabWidth(0),
	m_sourceFilter(defaultSourceFilter),
	m_headerFilter(defaultHeaderFilter),
	m_statusActive(i18n("active"))
{
    QPixmap p;

    KDockWidget* dw0 = createDockWidget("Source", p, 0, i18n("Source"));
    m_filesWindow = new WinStack(dw0, "files");
    dw0->setWidget(m_filesWindow);
    dw0->setDockSite(KDockWidget::DockCorner);
    dw0->setEnableDocking(KDockWidget::DockNone);
    setView(dw0);
    setMainDockWidget(dw0);

    KDockWidget* dw1 = createDockWidget("Stack", p, 0, i18n("Stack"));
    m_btWindow = new QListBox(dw1, "backtrace");
    dw1->setWidget(m_btWindow);
    KDockWidget* dw2 = createDockWidget("Locals", p, 0, i18n("Locals"));
    m_localVariables = new ExprWnd(dw2, i18n("Variable"), "locals");
    dw2->setWidget(m_localVariables);
    KDockWidget* dw3 = createDockWidget("Watches", p, 0, i18n("Watches"));
    m_watches = new WatchWindow(dw3, "watches");
    dw3->setWidget(m_watches);
    KDockWidget* dw4 = createDockWidget("Registers", p, 0, i18n("Registers"));
    m_registers = new RegisterView(dw4, "registers");
    dw4->setWidget(m_registers);
    KDockWidget* dw5 = createDockWidget("Breakpoints", p, 0, i18n("Breakpoints"));
    m_bpTable = new BreakpointTable(dw5, "breakpoints");
    dw5->setWidget(m_bpTable);
    KDockWidget* dw6 = createDockWidget("Output", p, 0, i18n("Output"));
    m_ttyWindow = new TTYWindow(dw6, "output");
    dw6->setWidget(m_ttyWindow);
    KDockWidget* dw7 = createDockWidget("Threads", p, 0, i18n("Threads"));
    m_threads = new ThreadList(dw7, "threads");
    dw7->setWidget(m_threads);
    KDockWidget* dw8 = createDockWidget("Memory", p, 0, i18n("Memory"));
    m_memoryWindow = new MemoryWindow(dw8, "memory");
    dw8->setWidget(m_memoryWindow);

    m_debugger = new KDebugger(this, m_localVariables, m_watches->watchVariables(), m_btWindow);

    connect(m_debugger, SIGNAL(updateStatusMessage()), SLOT(slotNewStatusMsg()));
    connect(m_debugger, SIGNAL(updateUI()), SLOT(updateUI()));
    connect(m_debugger, SIGNAL(breakpointsChanged()), SLOT(updateLineItems()));
    connect(m_debugger, SIGNAL(debuggerStarting()), SLOT(slotDebuggerStarting()));
    m_bpTable->setDebugger(m_debugger);
    m_memoryWindow->setDebugger(m_debugger);

    setStandardToolBarMenuEnabled(true);
    initKAction();
    initToolbar(); // kind of obsolete?

    connect(m_watches, SIGNAL(addWatch()), SLOT(slotAddWatch()));
    connect(m_watches, SIGNAL(deleteWatch()), m_debugger, SLOT(slotDeleteWatch()));
    connect(m_watches, SIGNAL(textDropped(const QString&)), SLOT(slotAddWatch(const QString&)));

    connect(&m_filesWindow->m_findDlg, SIGNAL(closed()), SLOT(updateUI()));
    connect(m_filesWindow, SIGNAL(newFileLoaded()),
	    SLOT(slotNewFileLoaded()));
    connect(m_filesWindow, SIGNAL(toggleBreak(const QString&,int,const DbgAddr&,bool)),
	    this, SLOT(slotToggleBreak(const QString&,int,const DbgAddr&,bool)));
    connect(m_filesWindow, SIGNAL(enadisBreak(const QString&,int,const DbgAddr&)),
	    this, SLOT(slotEnaDisBreak(const QString&,int,const DbgAddr&)));
    connect(m_debugger, SIGNAL(activateFileLine(const QString&,int,const DbgAddr&)),
	    m_filesWindow, SLOT(activate(const QString&,int,const DbgAddr&)));
    connect(m_debugger, SIGNAL(executableUpdated()),
	    m_filesWindow, SLOT(reloadAllFiles()));
    connect(m_debugger, SIGNAL(updatePC(const QString&,int,const DbgAddr&,int)),
	    m_filesWindow, SLOT(updatePC(const QString&,int,const DbgAddr&,int)));
    // value popup communication
    connect(m_filesWindow, SIGNAL(initiateValuePopup(const QString&)),
	    m_debugger, SLOT(slotValuePopup(const QString&)));
    connect(m_debugger, SIGNAL(valuePopup(const QString&)),
	    m_filesWindow, SLOT(slotShowValueTip(const QString&)));
    // disassembling
    connect(m_filesWindow, SIGNAL(disassemble(const QString&, int)),
	    m_debugger, SLOT(slotDisassemble(const QString&, int)));
    connect(m_debugger, SIGNAL(disassembled(const QString&,int,const std::list<DisassembledCode>&)),
	    m_filesWindow, SLOT(slotDisassembled(const QString&,int,const std::list<DisassembledCode>&)));
    connect(m_filesWindow, SIGNAL(moveProgramCounter(const QString&,int,const DbgAddr&)),
	    m_debugger, SLOT(setProgramCounter(const QString&,int,const DbgAddr&)));
    // program stopped
    connect(m_debugger, SIGNAL(programStopped()), SLOT(slotProgramStopped()));
    connect(&m_backTimer, SIGNAL(timeout()), SLOT(slotBackTimer()));
    // tab width
    connect(this, SIGNAL(setTabWidth(int)), m_filesWindow, SIGNAL(setTabWidth(int)));

    // connect breakpoint table
    connect(m_bpTable, SIGNAL(activateFileLine(const QString&,int,const DbgAddr&)),
	    m_filesWindow, SLOT(activate(const QString&,int,const DbgAddr&)));
    connect(m_debugger, SIGNAL(updateUI()), m_bpTable, SLOT(updateUI()));
    connect(m_debugger, SIGNAL(breakpointsChanged()), m_bpTable, SLOT(updateBreakList()));
    connect(m_debugger, SIGNAL(breakpointsChanged()), m_bpTable, SLOT(updateUI()));

    connect(m_debugger, SIGNAL(registersChanged(const std::list<RegisterInfo>&)),
	    m_registers, SLOT(updateRegisters(const std::list<RegisterInfo>&)));

    connect(m_debugger, SIGNAL(memoryDumpChanged(const QString&, const std::list<MemoryDump>&)),
	    m_memoryWindow, SLOT(slotNewMemoryDump(const QString&, const std::list<MemoryDump>&)));
    connect(m_debugger, SIGNAL(saveProgramSpecific(KConfigBase*)),
	    m_memoryWindow, SLOT(saveProgramSpecific(KConfigBase*)));
    connect(m_debugger, SIGNAL(restoreProgramSpecific(KConfigBase*)),
	    m_memoryWindow, SLOT(restoreProgramSpecific(KConfigBase*)));

    // thread window
    connect(m_debugger, SIGNAL(threadsChanged(const std::list<ThreadInfo>&)),
	    m_threads, SLOT(updateThreads(const std::list<ThreadInfo>&)));
    connect(m_threads, SIGNAL(setThread(int)),
	    m_debugger, SLOT(setThread(int)));

    // view menu changes when docking state changes
    connect(dockManager, SIGNAL(change()), SLOT(updateUI()));

    // popup menu of the local variables window
    connect(m_localVariables, SIGNAL(contextMenuRequested(QListViewItem*, const QPoint&, int)),
	    this, SLOT(slotLocalsPopup(QListViewItem*, const QPoint&)));

    restoreSettings(kapp->config());

    updateUI();
    m_bpTable->updateUI();
}

DebuggerMainWnd::~DebuggerMainWnd()
{
    saveSettings(kapp->config());
    // must delete m_debugger early since it references our windows
    delete m_debugger;
    m_debugger = 0;

    delete m_memoryWindow;
    delete m_threads;
    delete m_ttyWindow;
    delete m_bpTable;
    delete m_registers;
    delete m_watches;
    delete m_localVariables;
    delete m_btWindow;
    delete m_filesWindow;

    // if the output window is open, close it
    if (m_outputTermProc != 0) {
	m_outputTermProc->disconnect();	/* ignore signals */
	m_outputTermProc->kill();
	shutdownTermWindow();
    }
}

void DebuggerMainWnd::initKAction()
{
    // file menu
    KAction* open = KStdAction::open(this, SLOT(slotFileOpen()), 
                      actionCollection());
    open->setText(i18n("&Open Source..."));
    KStdAction::close(m_filesWindow, SLOT(slotClose()), actionCollection());
    (void)new KAction(i18n("&Reload Source"), "reload", 0, m_filesWindow, 
                      SLOT(slotFileReload()), actionCollection(), 
                      "file_reload");
    (void)new KAction(i18n("&Executable..."), "execopen", 0, this, 
                      SLOT(slotFileExe()), actionCollection(), 
                      "file_executable");
    m_recentExecAction = new KRecentFilesAction(i18n("Recent E&xecutables"), 0,
		      this, SLOT(slotRecentExec(const KURL&)),
		      actionCollection(), "file_executable_recent");
    (void)new KAction(i18n("&Core dump..."), 0, this, SLOT(slotFileCore()),
                      actionCollection(), "file_core_dump");
    KStdAction::quit(kapp, SLOT(closeAllWindows()), actionCollection());

    // settings menu
    (void)new KAction(i18n("This &Program..."), 0, this,
		      SLOT(slotFileProgSettings()), actionCollection(),
		      "settings_program");
    (void)new KAction(i18n("&Global Options..."), 0, this, 
                      SLOT(slotFileGlobalSettings()), actionCollection(),
                      "settings_global");
    KStdAction::keyBindings(this, SLOT(slotConfigureKeys()), actionCollection());
    KStdAction::showStatusbar(this, SLOT(slotViewStatusbar()), actionCollection());

    // view menu
    (void)new KToggleAction(i18n("&Find"), "find", CTRL+Key_F, m_filesWindow,
			    SLOT(slotViewFind()), actionCollection(),
			    "view_find");
    (void)KStdAction::findNext(m_filesWindow, SLOT(slotFindForward()), actionCollection(), "view_findnext");
    (void)KStdAction::findPrev(m_filesWindow, SLOT(slotFindBackward()), actionCollection(), "view_findprev");

    i18n("Source &code");
    struct { QString text; QWidget* w; QString id; } dw[] = {
	{ i18n("Stac&k"), m_btWindow, "view_stack"},
	{ i18n("&Locals"), m_localVariables, "view_locals"},
	{ i18n("&Watched expressions"), m_watches, "view_watched_expressions"},
	{ i18n("&Registers"), m_registers, "view_registers"},
	{ i18n("&Breakpoints"), m_bpTable, "view_breakpoints"},
	{ i18n("T&hreads"), m_threads, "view_threads"},
	{ i18n("&Output"), m_ttyWindow, "view_output"},
	{ i18n("&Memory"), m_memoryWindow, "view_memory"}
    };
    for (unsigned i = 0; i < sizeof(dw)/sizeof(dw[0]); i++) {
	KDockWidget* d = dockParent(dw[i].w);
	(void)new KToggleAction(dw[i].text, 0, d, SLOT(changeHideShowState()),
			  actionCollection(), dw[i].id);
    }

    
    // execution menu
    KAction* a = new KAction(i18n("&Run"), "pgmrun", Key_F5, m_debugger, 
		      SLOT(programRun()), actionCollection(), "exec_run");
    connect(a, SIGNAL(activated()), this, SLOT(intoBackground()));
    a = new KAction(i18n("Step &into"), "pgmstep", Key_F8, m_debugger, 
                      SLOT(programStep()), actionCollection(), 
                      "exec_step_into");
    connect(a, SIGNAL(activated()), this, SLOT(intoBackground()));
    a = new KAction(i18n("Step &over"), "pgmnext", Key_F10, m_debugger, 
                      SLOT(programNext()), actionCollection(), 
                      "exec_step_over");
    connect(a, SIGNAL(activated()), this, SLOT(intoBackground()));
    a = new KAction(i18n("Step o&ut"), "pgmfinish", Key_F6, m_debugger,
                      SLOT(programFinish()), actionCollection(), 
                      "exec_step_out");
    connect(a, SIGNAL(activated()), this, SLOT(intoBackground()));
    a = new KAction(i18n("Run to &cursor"), Key_F7, this,
                      SLOT(slotExecUntil()), actionCollection(), 
                      "exec_run_to_cursor");
    connect(a, SIGNAL(activated()), this, SLOT(intoBackground()));
    a = new KAction(i18n("Step i&nto by instruction"), "pgmstepi", 
		      SHIFT+Key_F8, m_debugger, SLOT(programStepi()), 
		      actionCollection(), "exec_step_into_by_insn");
    connect(a, SIGNAL(activated()), this, SLOT(intoBackground()));
    a = new KAction(i18n("Step o&ver by instruction"), "pgmnexti", 
		      SHIFT+Key_F10, m_debugger, SLOT(programNexti()), 
		      actionCollection(), "exec_step_over_by_insn");
    connect(a, SIGNAL(activated()), this, SLOT(intoBackground()));
    (void)new KAction(i18n("&Program counter to current line"), 0,
		      m_filesWindow, SLOT(slotMoveProgramCounter()),
		      actionCollection(), "exec_movepc");
    (void)new KAction(i18n("&Break"), 0, m_debugger,
                      SLOT(programBreak()), actionCollection(),
                      "exec_break");
    (void)new KAction(i18n("&Kill"), 0, m_debugger,
                      SLOT(programKill()), actionCollection(),
                      "exec_kill");
    (void)new KAction(i18n("Re&start"), 0, m_debugger,
                      SLOT(programRunAgain()), actionCollection(),
                      "exec_restart");
    (void)new KAction(i18n("A&ttach..."), 0, this,
                      SLOT(slotExecAttach()), actionCollection(),
                      "exec_attach");
    (void)new KAction(i18n("&Arguments..."), 0, this,
                      SLOT(slotExecArgs()), actionCollection(),
                      "exec_arguments");
   
    // breakpoint menu
    (void)new KAction(i18n("Set/Clear &breakpoint"), "brkpt", Key_F9,
                      m_filesWindow, SLOT(slotBrkptSet()), actionCollection(),
                      "breakpoint_set");
    (void)new KAction(i18n("Set &temporary breakpoint"), SHIFT+Key_F9,
                      m_filesWindow, SLOT(slotBrkptSetTemp()), actionCollection(),
                      "breakpoint_set_temporary");
    (void)new KAction(i18n("&Enable/Disable breakpoint"), CTRL+Key_F9,
                      m_filesWindow, SLOT(slotBrkptEnable()), actionCollection(),
                      "breakpoint_enable");
   
    // only in popup menus
    (void)new KAction(i18n("Watch Expression"), 0, this,
                      SLOT(slotLocalsToWatch()), actionCollection(),
                      "watch_expression");
    (void)new KAction(i18n("Edit Value"), Key_F2, this,
		      SLOT(slotEditValue()), actionCollection(),
		      "edit_value");

    // all actions force an UI update
    QValueList<KAction*> actions = actionCollection()->actions();
    QValueList<KAction*>::Iterator it = actions.begin();
    for (; it != actions.end(); ++it) {
	connect(*it, SIGNAL(activated()), this, SLOT(updateUI()));
    }

    createGUI("kdbgui.rc");
}

void DebuggerMainWnd::initToolbar()
{
    KToolBar* toolbar = toolBar("mainToolBar");
    toolbar->insertAnimatedWidget(ID_STATUS_BUSY,
	actionCollection()->action("exec_break"), SLOT(activate()),
	"pulse", -1);
    toolbar->alignItemRight(ID_STATUS_BUSY, true);
    m_animRunning = false;

    KStatusBar* statusbar = statusBar();
    statusbar->insertItem(m_statusActive, ID_STATUS_ACTIVE);
    m_lastActiveStatusText = m_statusActive;
    statusbar->insertItem("", ID_STATUS_MSG);	/* message pane */

    // reserve some translations
    i18n("Restart");
    i18n("Core dump");
}

bool DebuggerMainWnd::queryClose()
{
    if (m_debugger != 0) {
	m_debugger->shutdown();
    }
    return true;
}


// instance properties
void DebuggerMainWnd::saveProperties(KConfig* config)
{
    // session management
    QString executable = "";
    if (m_debugger != 0) {
	executable = m_debugger->executable();
    }
    config->writeEntry("executable", executable);
}

void DebuggerMainWnd::readProperties(KConfig* config)
{
    // session management
    QString execName = config->readEntry("executable");

    TRACE("readProperties: executable=" + execName);
    if (!execName.isEmpty()) {
	debugProgram(execName, "");
    }
}

static const char WindowGroup[] = "Windows";
static const char RecentExecutables[] = "RecentExecutables";
static const char LastSession[] = "LastSession";
static const char OutputWindowGroup[] = "OutputWindow";
static const char TermCmdStr[] = "TermCmdStr";
static const char KeepScript[] = "KeepScript";
static const char DebuggerGroup[] = "Debugger";
static const char DebuggerCmdStr[] = "DebuggerCmdStr";
static const char PreferencesGroup[] = "Preferences";
static const char PopForeground[] = "PopForeground";
static const char BackTimeout[] = "BackTimeout";
static const char TabWidth[] = "TabWidth";
static const char SourceFileFilter[] = "SourceFileFilter";
static const char HeaderFileFilter[] = "HeaderFileFilter";

void DebuggerMainWnd::saveSettings(KConfig* config)
{
    KConfigGroupSaver g(config, WindowGroup);

    writeDockConfig(config);
    fixDockConfig(config, false);	// downgrade

    m_recentExecAction->saveEntries(config, RecentExecutables);

    KConfigGroupSaver g2(config, LastSession);
    config->writeEntry("Width0Locals", m_localVariables->columnWidth(0));
    config->writeEntry("Width0Watches", m_watches->columnWidth(0));

    if (m_debugger != 0) {
	m_debugger->saveSettings(config);
    }

    KConfigGroupSaver g3(config, OutputWindowGroup);
    config->writeEntry(TermCmdStr, m_outputTermCmdStr);

    config->setGroup(DebuggerGroup);
    config->writeEntry(DebuggerCmdStr, m_debuggerCmdStr);

    config->setGroup(PreferencesGroup);
    config->writeEntry(PopForeground, m_popForeground);
    config->writeEntry(BackTimeout, m_backTimeout);
    config->writeEntry(TabWidth, m_tabWidth);
    config->writeEntry(SourceFileFilter, m_sourceFilter);
    config->writeEntry(HeaderFileFilter, m_headerFilter);
}

void DebuggerMainWnd::restoreSettings(KConfig* config)
{
    KConfigGroupSaver g(config, WindowGroup);

    fixDockConfig(config, true);	// upgrade
    readDockConfig(config);

    // Workaround bug #87787: KDockManager stores the titles of the KDockWidgets
    // in the config files, although they are localized:
    // If the user changes the language, the titles remain in the previous language.
    struct { QString text; QWidget* w; } dw[] = {
	{ i18n("Stack"), m_btWindow },
	{ i18n("Locals"), m_localVariables },
	{ i18n("Watches"), m_watches },
	{ i18n("Registers"), m_registers },
	{ i18n("Breakpoints"), m_bpTable },
	{ i18n("Threads"), m_threads },
	{ i18n("Output"), m_ttyWindow },
	{ i18n("Memory"), m_memoryWindow }
    };
    for (int i = 0; i < int(sizeof(dw)/sizeof(dw[0])); i++)
    {
	KDockWidget* w = dockParent(dw[i].w);
	w->setTabPageLabel(dw[i].text);
	// this actually changes the captions in the tabs:
	QEvent ev(QEvent::CaptionChange);
	w->event(&ev);
    }

    m_recentExecAction->loadEntries(config, RecentExecutables);

    KConfigGroupSaver g2(config, LastSession);
    int w;
    w = config->readNumEntry("Width0Locals", -1);
    if (w >= 0 && w < 30000)
	m_localVariables->setColumnWidth(0, w);
    w = config->readNumEntry("Width0Watches", -1);
    if (w >= 0 && w < 30000)
	m_watches->setColumnWidth(0, w);

    if (m_debugger != 0) {
	m_debugger->restoreSettings(config);
    }

    KConfigGroupSaver g3(config, OutputWindowGroup);
    /*
     * For debugging and emergency purposes, let the config file override
     * the shell script that is used to keep the output window open. This
     * string must have EXACTLY 1 %s sequence in it.
     */
    setTerminalCmd(config->readEntry(TermCmdStr, defaultTermCmdStr));
    m_outputTermKeepScript = config->readEntry(KeepScript);

    config->setGroup(DebuggerGroup);
    setDebuggerCmdStr(config->readEntry(DebuggerCmdStr));

    config->setGroup(PreferencesGroup);
    m_popForeground = config->readBoolEntry(PopForeground, false);
    m_backTimeout = config->readNumEntry(BackTimeout, 1000);
    m_tabWidth = config->readNumEntry(TabWidth, 0);
    m_sourceFilter = config->readEntry(SourceFileFilter, m_sourceFilter);
    m_headerFilter = config->readEntry(HeaderFileFilter, m_headerFilter);

    emit setTabWidth(m_tabWidth);
}

void DebuggerMainWnd::updateUI()
{
    KToggleAction* viewFind =
	static_cast<KToggleAction*>(actionCollection()->action("view_find"));
    viewFind->setChecked(m_filesWindow->m_findDlg.isVisible());
    viewFind->setEnabled(m_filesWindow->hasWindows());
    actionCollection()->action("breakpoint_set")->setEnabled(m_debugger->canChangeBreakpoints());
    actionCollection()->action("breakpoint_set_temporary")->setEnabled(m_debugger->canChangeBreakpoints());
    actionCollection()->action("breakpoint_enable")->setEnabled(m_debugger->canChangeBreakpoints());
    dockUpdateHelper("view_breakpoints", m_bpTable);
    dockUpdateHelper("view_stack", m_btWindow);
    dockUpdateHelper("view_locals", m_localVariables);
    dockUpdateHelper("view_watched_expressions", m_watches);
    dockUpdateHelper("view_registers", m_registers);
    dockUpdateHelper("view_threads", m_threads);
    dockUpdateHelper("view_memory", m_memoryWindow);
    dockUpdateHelper("view_output", m_ttyWindow);

    // AB: maybe in mainwndbase.cpp?
    actionCollection()->action("file_executable")->setEnabled(m_debugger->isIdle());
    actionCollection()->action("settings_program")->setEnabled(m_debugger->haveExecutable());
    actionCollection()->action("file_core_dump")->setEnabled(m_debugger->canStart());
    actionCollection()->action("file_close")->setEnabled(m_filesWindow->hasWindows());
    actionCollection()->action("file_reload")->setEnabled(m_filesWindow->hasWindows());
    actionCollection()->action("exec_step_into")->setEnabled(m_debugger->canSingleStep());
    actionCollection()->action("exec_step_into_by_insn")->setEnabled(m_debugger->canSingleStep());
    actionCollection()->action("exec_step_over")->setEnabled(m_debugger->canSingleStep());
    actionCollection()->action("exec_step_over_by_insn")->setEnabled(m_debugger->canSingleStep());
    actionCollection()->action("exec_step_out")->setEnabled(m_debugger->canSingleStep());
    actionCollection()->action("exec_run_to_cursor")->setEnabled(m_debugger->canSingleStep());
    actionCollection()->action("exec_movepc")->setEnabled(m_debugger->canSingleStep());
    actionCollection()->action("exec_restart")->setEnabled(m_debugger->canSingleStep());
    actionCollection()->action("exec_attach")->setEnabled(m_debugger->isReady());
    actionCollection()->action("exec_run")->setEnabled(m_debugger->canStart() || m_debugger->canSingleStep());
    actionCollection()->action("exec_kill")->setEnabled(m_debugger->haveExecutable() && m_debugger->isProgramActive());
    actionCollection()->action("exec_break")->setEnabled(m_debugger->isProgramRunning());
    actionCollection()->action("exec_arguments")->setEnabled(m_debugger->haveExecutable());
    actionCollection()->action("edit_value")->setEnabled(m_debugger->canSingleStep());

    // animation
    KAnimWidget* w = toolBar("mainToolBar")->animatedWidget(ID_STATUS_BUSY);
    if (m_debugger->isIdle()) {
	if (m_animRunning) {
	    w->stop();
	    m_animRunning = false;
	}
    } else {
	if (!m_animRunning) {
	    w->start();
	    m_animRunning = true;
	}
    }

    // update statusbar
    QString newStatus;
    if (m_debugger->isProgramActive())
	newStatus = m_statusActive;
    if (newStatus != m_lastActiveStatusText) {
	statusBar()->changeItem(newStatus, ID_STATUS_ACTIVE);
	m_lastActiveStatusText = newStatus;
    }
}

void DebuggerMainWnd::dockUpdateHelper(QString action, QWidget* w)
{
    KToggleAction* item =
	static_cast<KToggleAction*>(actionCollection()->action(action));
    bool canChange = canChangeDockVisibility(w);
    item->setEnabled(canChange);
    item->setChecked(canChange && isDockVisible(w));
}

void DebuggerMainWnd::updateLineItems()
{
    m_filesWindow->updateLineItems(m_debugger);
}

void DebuggerMainWnd::slotAddWatch()
{
    if (m_debugger != 0) {
	QString t = m_watches->watchText();
	m_debugger->addWatch(t);
    }
}

void DebuggerMainWnd::slotAddWatch(const QString& text)
{
    if (m_debugger != 0) {
	m_debugger->addWatch(text);
    }
}

void DebuggerMainWnd::slotNewFileLoaded()
{
    // updates program counter in the new file
    if (m_debugger != 0)
	m_filesWindow->updateLineItems(m_debugger);
}

KDockWidget* DebuggerMainWnd::dockParent(QWidget* w)
{
    while ((w = w->parentWidget()) != 0) {
	if (w->isA("KDockWidget"))
	    return static_cast<KDockWidget*>(w);
    }
    return 0;
}

bool DebuggerMainWnd::isDockVisible(QWidget* w)
{
    KDockWidget* d = dockParent(w);
    return d != 0 && d->mayBeHide();
}

bool DebuggerMainWnd::canChangeDockVisibility(QWidget* w)
{
    KDockWidget* d = dockParent(w);
    return d != 0 && (d->mayBeHide() || d->mayBeShow());
}

// upgrades the entries from version 0.0.4 to 0.0.5 and back
void DebuggerMainWnd::fixDockConfig(KConfig* c, bool upgrade)
{
    static const char dockGroup[] = "dock_setting_default";
    if (!c->hasGroup(dockGroup))
	return;

    static const char oldVersion[] = "0.0.4";
    static const char newVersion[] = "0.0.5";
    const char* from = upgrade ? oldVersion : newVersion;
    const char* to   = upgrade ? newVersion : oldVersion;
    QMap<QString,QString> e = c->entryMap(dockGroup);
    if (e["Version"] != from)
	return;

    KConfigGroupSaver g(c, dockGroup);
    c->writeEntry("Version", to);
    TRACE(upgrade ? "upgrading dockconfig" : "downgrading dockconfig");

    // turn all orientation entries from 0 to 1 and from 1 to 0
    QMap<QString,QString>::Iterator i;
    for (i = e.begin(); i != e.end(); ++i)
    {
	if (i.key().right(12) == ":orientation") {
	    TRACE("upgrading " + i.key() + " old value: " + *i);
	    int orientation = c->readNumEntry(i.key(), -1);
	    if (orientation >= 0) {	// paranoia
		c->writeEntry(i.key(), 1 - orientation);
	    }
	}
    }
}

bool DebuggerMainWnd::debugProgram(const QString& exe, const QString& lang)
{
    // check the file name
    QFileInfo fi(exe);

    bool success = fi.isFile();
    if (!success)
    {
	QString msg = i18n("`%1' is not a file or does not exist");
	KMessageBox::sorry(this, msg.arg(exe));
    }
    else
    {
	success = startDriver(fi.absFilePath(), lang);
    }

    if (success)
    {
	m_recentExecAction->addURL(KURL(fi.absFilePath()));

	// keep the directory
	m_lastDirectory = fi.dirPath(true);
	m_filesWindow->setExtraDirectory(m_lastDirectory);

	// set caption to basename part of executable
	QString caption = fi.fileName();
	setCaption(caption);
    }
    else
    {
	m_recentExecAction->removeURL(KURL(fi.absFilePath()));
    }

    return success;
}

static const char GeneralGroup[] = "General";

bool DebuggerMainWnd::startDriver(const QString& executable, QString lang)
{
    assert(m_debugger != 0);

    TRACE(QString("trying language '%1'...").arg(lang));
    DebuggerDriver* driver = driverFromLang(lang);

    if (driver == 0)
    {
	// see if there is a language in the per-program config file
	QString configName = m_debugger->getConfigForExe(executable);
	if (QFile::exists(configName))
	{
	    KSimpleConfig c(configName, true);	// read-only
	    c.setGroup(GeneralGroup);

	    // Using "GDB" as default here is for backwards compatibility:
	    // The config file exists but doesn't have an entry,
	    // so it must have been created by an old version of KDbg
	    // that had only the GDB driver.
	    lang = c.readEntry(KDebugger::DriverNameEntry, "GDB");

	    TRACE(QString("...bad, trying config driver %1...").arg(lang));
	    driver = driverFromLang(lang);
	}

    }
    if (driver == 0)
    {
	QString name = driverNameFromFile(executable);

	TRACE(QString("...no luck, trying %1 derived"
		" from file contents").arg(name));
	driver = driverFromLang(name);
    }
    if (driver == 0)
    {
	// oops
	QString msg = i18n("Don't know how to debug language `%1'");
	KMessageBox::sorry(this, msg.arg(lang));
	return false;
    }

    driver->setLogFileName(m_transcriptFile);

    bool success = m_debugger->debugProgram(executable, driver);

    if (!success)
    {
	delete driver;

	QString msg = i18n("Could not start the debugger process.\n"
			   "Please shut down KDbg and resolve the problem.");
	KMessageBox::sorry(this, msg);
    }

    return success;
}

// derive driver from language
DebuggerDriver* DebuggerMainWnd::driverFromLang(QString lang)
{
    // lang is needed in all lowercase
    lang = lang.lower();

    // The following table relates languages and debugger drivers
    static const struct L {
	const char* shortest;	// abbreviated to this is still unique
	const char* full;	// full name of language
	int driver;
    } langs[] = {
	{ "c",       "c++",     1 },
	{ "f",       "fortran", 1 },
	{ "p",       "python",  3 },
	{ "x",       "xslt",    2 },
	// the following are actually driver names
	{ "gdb",     "gdb",     1 },
	{ "xsldbg",  "xsldbg",  2 },
    };
    const int N = sizeof(langs)/sizeof(langs[0]);

    // lookup the language name
    int driverID = 0;
    for (int i = 0; i < N; i++)
    {
	const L& l = langs[i];

	// shortest must match
	if (!lang.startsWith(l.shortest))
	    continue;

	// lang must not be longer than the full name, and it must match
	if (QString(l.full).startsWith(lang))
	{
	    driverID = l.driver;
	    break;
	}
    }
    DebuggerDriver* driver = 0;
    switch (driverID) {
    case 1:
	{
	    GdbDriver* gdb = new GdbDriver;
	    gdb->setDefaultInvocation(m_debuggerCmdStr);
	    driver = gdb;
	}
	break;
    case 2:
	driver = new XsldbgDriver;
	break;
    default:
	// unknown language
	break;
    }
    return driver;
}

/**
 * Try to guess the language to use from the contents of the file.
 */
QString DebuggerMainWnd::driverNameFromFile(const QString& exe)
{
    /* Inprecise but simple test to see if file is in XSLT language */
    if (exe.right(4).lower() == ".xsl")
	return "XSLT";

    return "GDB";
}

void DebuggerMainWnd::setCoreFile(const QString& corefile)
{
    assert(m_debugger != 0);
    m_debugger->useCoreFile(corefile, true);
}

void DebuggerMainWnd::setRemoteDevice(const QString& remoteDevice)
{
    if (m_debugger != 0) {
	m_debugger->setRemoteDevice(remoteDevice);
    }
}

void DebuggerMainWnd::overrideProgramArguments(const QString& args)
{
    assert(m_debugger != 0);
    m_debugger->overrideProgramArguments(args);
}

void DebuggerMainWnd::setTranscript(const QString& name)
{
    m_transcriptFile = name;
    if (m_debugger != 0 && m_debugger->driver() != 0)
	m_debugger->driver()->setLogFileName(m_transcriptFile);
}

void DebuggerMainWnd::setAttachPid(const QString& pid)
{
    assert(m_debugger != 0);
    m_debugger->setAttachPid(pid);
}

void DebuggerMainWnd::slotNewStatusMsg()
{
    QString msg = m_debugger->statusMessage();
    statusBar()->changeItem(msg, ID_STATUS_MSG);
}

void DebuggerMainWnd::slotFileGlobalSettings()
{
    int oldTabWidth = m_tabWidth;

    QTabDialog dlg(this, "global_options", true);
    QString title = kapp->caption();
    title += i18n(": Global options");
    dlg.setCaption(title);
    dlg.setCancelButton(i18n("Cancel"));
    dlg.setOKButton(i18n("OK"));

    PrefDebugger prefDebugger(&dlg);
    prefDebugger.setDebuggerCmd(m_debuggerCmdStr.isEmpty()  ?
				GdbDriver::defaultGdb()  :  m_debuggerCmdStr);
    prefDebugger.setTerminal(m_outputTermCmdStr);

    PrefMisc prefMisc(&dlg);
    prefMisc.setPopIntoForeground(m_popForeground);
    prefMisc.setBackTimeout(m_backTimeout);
    prefMisc.setTabWidth(m_tabWidth);
    prefMisc.setSourceFilter(m_sourceFilter);
    prefMisc.setHeaderFilter(m_headerFilter);

    dlg.addTab(&prefDebugger, i18n("&Debugger"));
    dlg.addTab(&prefMisc, i18n("&Miscellaneous"));
    if (dlg.exec() == QDialog::Accepted)
    {
	setDebuggerCmdStr(prefDebugger.debuggerCmd());
	setTerminalCmd(prefDebugger.terminal());
	m_popForeground = prefMisc.popIntoForeground();
	m_backTimeout = prefMisc.backTimeout();
	m_tabWidth = prefMisc.tabWidth();
	m_sourceFilter = prefMisc.sourceFilter();
	if (m_sourceFilter.isEmpty())
	    m_sourceFilter = defaultSourceFilter;
	m_headerFilter = prefMisc.headerFilter();
	if (m_headerFilter.isEmpty())
	    m_headerFilter = defaultHeaderFilter;
    }

    if (m_tabWidth != oldTabWidth) {
	emit setTabWidth(m_tabWidth);
    }
}

void DebuggerMainWnd::setTerminalCmd(const QString& cmd)
{
    m_outputTermCmdStr = cmd;
    // revert to default if empty
    if (m_outputTermCmdStr.isEmpty()) {
	m_outputTermCmdStr = defaultTermCmdStr;
    }
}

void DebuggerMainWnd::setDebuggerCmdStr(const QString& cmd)
{
    m_debuggerCmdStr = cmd;
    // make empty if it is the default
    if (m_debuggerCmdStr == GdbDriver::defaultGdb()) {
	m_debuggerCmdStr = QString();
    }
}

void DebuggerMainWnd::slotDebuggerStarting()
{
    if (m_debugger == 0)		/* paranoia check */
	return;

    if (m_ttyLevel == m_debugger->ttyLevel())
	return;

    // shut down terminal emulations we will not need
    switch (m_ttyLevel) {
    case KDebugger::ttySimpleOutputOnly:
	m_ttyWindow->deactivate();
	break;
    case KDebugger::ttyFull:
	if (m_outputTermProc != 0) {
	    m_outputTermProc->kill();
	    // will be deleted in slot
	}
	break;
    default: break;
    }

    m_ttyLevel = m_debugger->ttyLevel();

    QString ttyName;
    switch (m_ttyLevel) {
    case KDebugger::ttySimpleOutputOnly:
	ttyName = m_ttyWindow->activate();
	break;
    case KDebugger::ttyFull:
	if (m_outputTermProc == 0) {
	    // create an output window
	    ttyName = createOutputWindow();
	    TRACE(ttyName.isEmpty() ?
		  "createOuputWindow failed" : "successfully created output window");
	}
	break;
    default: break;
    }

    m_debugger->setTerminal(ttyName);
}

void DebuggerMainWnd::slotToggleBreak(const QString& fileName, int lineNo,
				      const DbgAddr& address, bool temp)
{
    // lineNo is zero-based
    if (m_debugger != 0) {
	m_debugger->setBreakpoint(fileName, lineNo, address, temp);
    }
}

void DebuggerMainWnd::slotEnaDisBreak(const QString& fileName, int lineNo,
				      const DbgAddr& address)
{
    // lineNo is zero-based
    if (m_debugger != 0) {
	m_debugger->enableDisableBreakpoint(fileName, lineNo, address);
    }
}

QString DebuggerMainWnd::createOutputWindow()
{
    // create a name for a fifo
    QString fifoName;
    fifoName.sprintf("/tmp/kdbgttywin%05d", ::getpid());

    // create a fifo that will pass in the tty name
    QFile::remove(fifoName);		// remove remnants
#ifdef HAVE_MKFIFO
    if (::mkfifo(fifoName.local8Bit(), S_IRUSR|S_IWUSR) < 0) {
	// failed
	TRACE("mkfifo " + fifoName + " failed");
	return QString();
    }
#else
    if (::mknod(fifoName.local8Bit(), S_IFIFO | S_IRUSR|S_IWUSR, 0) < 0) {
	// failed
	TRACE("mknod " + fifoName + " failed");
	return QString();
    }
#endif

    m_outputTermProc = new KProcess;

    /*
     * Spawn an xterm that in turn runs a shell script that passes us
     * back the terminal name and then only sits and waits.
     */
    static const char shellScriptFmt[] =
	"tty>%s;"
	"trap \"\" INT QUIT TSTP;"	/* ignore various signals */
	"exec<&-;exec>&-;"		/* close stdin and stdout */
	"while :;do sleep 3600;done";
    // let config file override this script
    QString shellScript;
    if (!m_outputTermKeepScript.isEmpty()) {
	shellScript = m_outputTermKeepScript;
    } else {
	shellScript = shellScriptFmt;
    }

    shellScript.replace("%s", fifoName);
    TRACE("output window script is " + shellScript);

    QString title = kapp->caption();
    title += i18n(": Program output");

    // parse the command line specified in the preferences
    QStringList cmdParts = QStringList::split(' ', m_outputTermCmdStr);

    /*
     * Build the argv array. Thereby substitute special sequences:
     */
    struct {
	char seq[4];
	QString replace;
    } substitute[] = {
	{ "%T", title },
	{ "%C", shellScript }
    };

    for (QStringList::iterator i = cmdParts.begin(); i != cmdParts.end(); ++i)
    {
	QString& str = *i;
	for (int j = sizeof(substitute)/sizeof(substitute[0])-1; j >= 0; j--) {
	    int pos = str.find(substitute[j].seq);
	    if (pos >= 0) {
		str.replace(pos, 2, substitute[j].replace);
		break;		/* substitute only one sequence */
	    }
	}
	*m_outputTermProc << str;
    }

    QString tty;

    if (m_outputTermProc->start())
    {
	// read the ttyname from the fifo
	QFile f(fifoName);
	if (f.open(IO_ReadOnly))
	{
	    QByteArray t = f.readAll();
	    tty = QString::fromLocal8Bit(t, t.size());
	    f.close();
	}
	f.remove();

	// remove whitespace
	tty = tty.stripWhiteSpace();
	TRACE("tty=" + tty);

	connect(m_outputTermProc, SIGNAL(processExited(KProcess*)),
		SLOT(slotTermEmuExited()));
    }
    else
    {
	// error, could not start xterm
	TRACE("fork failed for fifo " + fifoName);
	QFile::remove(fifoName);
	shutdownTermWindow();
    }

    return tty;
}

void DebuggerMainWnd::slotTermEmuExited()
{
    shutdownTermWindow();
}

void DebuggerMainWnd::shutdownTermWindow()
{
    delete m_outputTermProc;
    m_outputTermProc = 0;
}

void DebuggerMainWnd::slotProgramStopped()
{
    // when the program stopped, move the window to the foreground
    if (m_popForeground) {
	// unfortunately, this requires quite some force to work :-(
	KWin::raiseWindow(winId());
	KWin::forceActiveWindow(winId());
    }
    m_backTimer.stop();
}

void DebuggerMainWnd::intoBackground()
{
    if (m_popForeground) {
        m_backTimer.start(m_backTimeout, true);	/* single-shot */
    }
}

void DebuggerMainWnd::slotBackTimer()
{
    lower();
}

void DebuggerMainWnd::slotRecentExec(const KURL& url)
{
    QString exe = url.path();
    debugProgram(exe, "");
}

QString DebuggerMainWnd::makeSourceFilter()
{
    QString f;
    f = m_sourceFilter + " " + m_headerFilter + i18n("|All source files\n");
    f += m_sourceFilter + i18n("|Source files\n");
    f += m_headerFilter + i18n("|Header files\n");
    f += i18n("*|All files");
    return f;
}

/*
 * Pop up the context menu in the locals window
 */
void DebuggerMainWnd::slotLocalsPopup(QListViewItem*, const QPoint& pt)
{
    QPopupMenu* popup =
	static_cast<QPopupMenu*>(factory()->container("popup_locals", this));
    if (popup == 0) {
        return;
    }
    if (popup->isVisible()) {
	popup->hide();
    } else {
	popup->popup(pt);
    }
}

/*
 * Copies the currently selected item to the watch window.
 */
void DebuggerMainWnd::slotLocalsToWatch()
{
    VarTree* item = m_localVariables->selectedItem();

    if (item != 0 && m_debugger != 0) {
	QString text = item->computeExpr();
	m_debugger->addWatch(text);
    }
}

/*
 * Starts editing a value in a value display
 */
void DebuggerMainWnd::slotEditValue()
{
    // does one of the value trees have the focus
    QWidget* f = kapp->focusWidget();
    ExprWnd* wnd;
    if (f == m_localVariables) {
	wnd = m_localVariables;
    } else if (f == m_watches->watchVariables()) {
	wnd = m_watches->watchVariables();
    } else {
	return;
    }

    if (m_localVariables->isEditing() ||
	m_watches->watchVariables()->isEditing())
    {
	return;				/* don't edit twice */
    }
    
    VarTree* expr = wnd->currentItem();
    if (expr != 0 && m_debugger != 0 && m_debugger->canSingleStep())
    {
	TRACE("edit value");
	// determine the text to edit
	QString text = m_debugger->driver()->editableValue(expr);
	wnd->editValue(expr, text);
    }
}

// helper that gets a file name (it only differs in the caption of the dialog)
static QString myGetFileName(QString caption,
			     QString dir, QString filter,
	 		     QWidget* parent)
{
    QString filename;
    KFileDialog dlg(dir, filter, parent, "filedialog", true);

    dlg.setCaption(caption);

    if (dlg.exec() == QDialog::Accepted)
	filename = dlg.selectedFile();

    return filename;
}

void DebuggerMainWnd::slotFileOpen()
{
    // start browsing in the active file's directory
    // fall back to last used directory (executable)
    QString dir = m_lastDirectory;
    QString fileName = m_filesWindow->activeFileName();
    if (!fileName.isEmpty()) {
	QFileInfo fi(fileName);
	dir = fi.dirPath();
    }

    fileName = myGetFileName(i18n("Open"),
			     dir,
			     makeSourceFilter(), this);

    if (!fileName.isEmpty())
    {
	QFileInfo fi(fileName);
	m_lastDirectory = fi.dirPath();
	m_filesWindow->setExtraDirectory(m_lastDirectory);
	m_filesWindow->activateFile(fileName);
    }
}

void DebuggerMainWnd::slotFileExe()
{
    if (m_debugger->isIdle())
    {
	// open a new executable
	QString executable = myGetFileName(i18n("Select the executable to debug"),
					   m_lastDirectory, 0, this);
	if (executable.isEmpty())
	    return;

	debugProgram(executable, "");
    }
}

void DebuggerMainWnd::slotFileCore()
{
    if (m_debugger->canStart())
    {
	QString corefile = myGetFileName(i18n("Select core dump"),
					 m_lastDirectory, 0, this);
	if (!corefile.isEmpty()) {
	    m_debugger->useCoreFile(corefile, false);
	}
    }
}

void DebuggerMainWnd::slotFileProgSettings()
{
    if (m_debugger != 0) {
	m_debugger->programSettings(this);
    }
}

void DebuggerMainWnd::slotViewStatusbar()
{
    if (statusBar()->isVisible())
	statusBar()->hide();
    else
	statusBar()->show();
    setSettingsDirty();
}

void DebuggerMainWnd::slotExecUntil()
{
    if (m_debugger != 0)
    {
	QString file;
	int lineNo;
	if (m_filesWindow->activeLine(file, lineNo))
	    m_debugger->runUntil(file, lineNo);
    }
}

void DebuggerMainWnd::slotExecAttach()
{
#ifdef PS_COMMAND
    ProcAttachPS dlg(this);
    // seed filter with executable name
    QFileInfo fi = m_debugger->executable();
    dlg.filterEdit->setText(fi.fileName());
#else
    ProcAttach dlg(this);
    dlg.setText(m_debugger->attachedPid());
#endif
    if (dlg.exec()) {
	m_debugger->attachProgram(dlg.text());
    }
}

void DebuggerMainWnd::slotExecArgs()
{
    if (m_debugger != 0) {
	m_debugger->programArgs(this);
    }
}

void DebuggerMainWnd::slotConfigureKeys()
{
    KKeyDialog::configure(actionCollection(), this);
}

#include "dbgmainwnd.moc"
