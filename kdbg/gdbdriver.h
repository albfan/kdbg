// $Id$

// Copyright by Johannes Sixt
// This file is under GPL, the GNU General Public Licence

#ifndef GDBDRIVER_H
#define GDBDRIVER_H

#include "dbgdriver.h"


class GdbDriver : public DebuggerDriver
{
    Q_OBJECT
public:
    GdbDriver();
    ~GdbDriver();
    
    virtual QString driverName() const;
    virtual bool startup();
    virtual void commandFinished(CmdQueueItem* cmd);
    virtual CmdQueueItem* executeCmd(DbgCommand,
				     bool clearLow = false);
    virtual CmdQueueItem* executeCmd(DbgCommand, QString strArg,
				     bool clearLow = false);
    virtual CmdQueueItem* executeCmd(DbgCommand, int intArg,
				     bool clearLow = false);
    virtual CmdQueueItem* executeCmd(DbgCommand, QString strArg, int intArg,
				     bool clearLow = false);
    virtual CmdQueueItem* executeCmd(DbgCommand, QString strArg1, QString strArg2,
				     bool clearLow = false);
    virtual CmdQueueItem* executeCmd(DbgCommand, int intArg1, int intArg2,
				     bool clearLow = false);
    virtual CmdQueueItem* queueCmd(DbgCommand,
				   QueueMode mode);
    virtual CmdQueueItem* queueCmd(DbgCommand, QString strArg,
				   QueueMode mode);
    virtual CmdQueueItem* queueCmd(DbgCommand, int intArg,
				   QueueMode mode);
    virtual CmdQueueItem* queueCmd(DbgCommand, QString strArg, int intArg,
				   QueueMode mode);

    virtual VarTree* parseQCharArray(const char* output, bool wantErrorValue);
    virtual void parseBackTrace(const char* output, QList<StackFrame>& stack);
    virtual bool parseFrameChange(const char* output,
				  int& frameNo, QString& file, int& lineNo);
    virtual bool parseBreakList(const char* output, QList<Breakpoint>& brks);
    virtual bool parseBreakpoint(const char* output, int& id,
				 QString& file, int& lineNo);
    virtual void parseLocals(const char* output, QList<VarTree>& newVars);
    virtual bool parsePrintExpr(const char* output, bool wantErrorValue,
				VarTree*& var);
    virtual bool parseChangeWD(const char* output, QString& message);
    virtual bool parseChangeExecutable(const char* output, QString& message);
    virtual bool parseCoreFile(const char* output);
    virtual uint parseProgramStopped(const char* output, QString& message);
    virtual void parseSharedLibs(const char* output, QStrList& shlibs);
    virtual bool parseFindType(const char* output, QString& type);
protected:
    int m_gdbMajor, m_gdbMinor;
    QString m_programWD;		/* just an intermediate storage */
    bool m_haveCoreFile;

    QString makeCmdString(DbgCommand cmd, QString strArg);
    QString makeCmdString(DbgCommand cmd, int intArg);
    QString makeCmdString(DbgCommand cmd, QString strArg, int intArg);
    QString makeCmdString(DbgCommand cmd, QString strArg1, QString strArg2);
    QString makeCmdString(DbgCommand cmd, int intArg1, int intArg2);
    void parseMarker();
};

#endif // GDBDRIVER_H
