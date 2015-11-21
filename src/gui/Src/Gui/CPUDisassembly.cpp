#include "CPUDisassembly.h"
#include "CPUWidget.h"
#include <QMessageBox>
#include <QClipboard>
#include "Configuration.h"
#include "Bridge.h"
#include "LineEditDialog.h"
#include "WordEditDialog.h"
#include "HexEditDialog.h"
#include "YaraRuleSelectionDialog.h"

CPUDisassembly::CPUDisassembly(CPUWidget* parent) : Disassembly(parent)
{
    // Set specific widget handles
    mGoto = nullptr;
    mParentCPUWindow = parent;

    // Create the action list for the right click context menu
    setupRightClickContextMenu();

    // Connect bridge<->disasm calls
    connect(Bridge::getBridge(), SIGNAL(disassembleAt(dsint, dsint)), this, SLOT(disassembleAt(dsint, dsint)));
    connect(Bridge::getBridge(), SIGNAL(dbgStateChanged(DBGSTATE)), this, SLOT(debugStateChangedSlot(DBGSTATE)));
    connect(Bridge::getBridge(), SIGNAL(selectionDisasmGet(SELECTIONDATA*)), this, SLOT(selectionGetSlot(SELECTIONDATA*)));
    connect(Bridge::getBridge(), SIGNAL(selectionDisasmSet(const SELECTIONDATA*)), this, SLOT(selectionSetSlot(const SELECTIONDATA*)));

    Initialize();
}

void CPUDisassembly::mousePressEvent(QMouseEvent* event)
{
    if(event->buttons() == Qt::MiddleButton) //copy address to clipboard
    {
        if(!DbgIsDebugging())
            return;
        MessageBeep(MB_OK);
        copyAddressSlot();
    }
    else
    {
        Disassembly::mousePressEvent(event);
        if(mHighlightingMode) //disable highlighting mode after clicked
        {
            mHighlightingMode = false;
            reloadData();
        }
    }
}

void CPUDisassembly::mouseDoubleClickEvent(QMouseEvent* event)
{
    if(event->button() != Qt::LeftButton)
        return;
    switch(getColumnIndexFromX(event->x()))
    {
    case 0: //address
    {
        dsint mSelectedVa = rvaToVa(getInitialSelection());
        if(mRvaDisplayEnabled && mSelectedVa == mRvaDisplayBase)
            mRvaDisplayEnabled = false;
        else
        {
            mRvaDisplayEnabled = true;
            mRvaDisplayBase = mSelectedVa;
            mRvaDisplayPageBase = getBase();
        }
        reloadData();
    }
    break;

    // (Opcodes) Set INT3 breakpoint
    case 1:
        toggleInt3BPActionSlot();
        break;

    // (Disassembly) Assemble dialog
    case 2:
        assembleSlot();
        break;

    // (Comments) Set comment dialog
    case 3:
        setCommentSlot();
        break;

    // Undefined area
    default:
        Disassembly::mouseDoubleClickEvent(event);
        break;
    }
}

void CPUDisassembly::addFollowReferenceMenuItem(QString name, dsint value, QMenu* menu, bool isReferences)
{
    foreach(QAction * action, menu->actions()) //check for duplicate action
    if(action->text() == name)
        return;
    QAction* newAction = new QAction(name, this);
    newAction->setFont(QFont("Courier New", 8));
    menu->addAction(newAction);
    newAction->setObjectName(QString(isReferences ? "REF|" : "DUMP|") + QString("%1").arg(value, sizeof(dsint) * 2, 16, QChar('0')).toUpper());
    connect(newAction, SIGNAL(triggered()), this, SLOT(followActionSlot()));
}

void CPUDisassembly::setupFollowReferenceMenu(dsint wVA, QMenu* menu, bool isReferences)
{
    //remove previous actions
    QList<QAction*> list = menu->actions();
    for(int i = 0; i < list.length(); i++)
        menu->removeAction(list.at(i));

    //most basic follow action
    if(isReferences)
        menu->addAction(mReferenceSelectedAddressAction);
    else
        addFollowReferenceMenuItem("&Selected Address", wVA, menu, isReferences);

    //add follow actions
    DISASM_INSTR instr;
    DbgDisasmAt(wVA, &instr);

    if(!isReferences) //follow in dump
    {
        for(int i = 0; i < instr.argcount; i++)
        {
            const DISASM_ARG arg = instr.arg[i];
            if(arg.type == arg_memory)
            {
                if(DbgMemIsValidReadPtr(arg.value))
                    addFollowReferenceMenuItem("&Address: " + QString(arg.mnemonic).toUpper().trimmed(), arg.value, menu, isReferences);
                if(arg.value != arg.constant)
                {
                    QString constant = QString("%1").arg(arg.constant, 1, 16, QChar('0')).toUpper();
                    if(DbgMemIsValidReadPtr(arg.constant))
                        addFollowReferenceMenuItem("&Constant: " + constant, arg.constant, menu, isReferences);
                }
                if(DbgMemIsValidReadPtr(arg.memvalue))
                    addFollowReferenceMenuItem("&Value: [" + QString(arg.mnemonic) + "]", arg.memvalue, menu, isReferences);
            }
            else //arg_normal
            {
                if(DbgMemIsValidReadPtr(arg.value))
                    addFollowReferenceMenuItem(QString(arg.mnemonic).toUpper().trimmed(), arg.value, menu, isReferences);
            }
        }
    }
    else //find references
    {
        for(int i = 0; i < instr.argcount; i++)
        {
            const DISASM_ARG arg = instr.arg[i];
            QString constant = QString("%1").arg(arg.constant, 1, 16, QChar('0')).toUpper();
            if(DbgMemIsValidReadPtr(arg.constant))
                addFollowReferenceMenuItem("Address: " + constant, arg.constant, menu, isReferences);
            else if(arg.constant)
                addFollowReferenceMenuItem("Constant: " + constant, arg.constant, menu, isReferences);
        }
    }
}


/************************************************************************************
                            Mouse Management
************************************************************************************/
/**
 * @brief       This method has been reimplemented. It manages the richt click context menu.
 *
 * @param[in]   event       Context menu event
 *
 * @return      Nothing.
 */
void CPUDisassembly::contextMenuEvent(QContextMenuEvent* event)
{
    if(getSize() != 0)
    {
        int wI;
        QMenu* wMenu = new QMenu(this);
        duint wVA = rvaToVa(getInitialSelection());
        BPXTYPE wBpType = DbgGetBpxTypeAt(wVA);

        // Build Menu
        wMenu->addMenu(mBinaryMenu);
        wMenu->addMenu(mCopyMenu);
        dsint start = rvaToVa(getSelectionStart());
        dsint end = rvaToVa(getSelectionEnd());
        if(DbgFunctions()->PatchInRange(start, end)) //nothing patched in selected range
            wMenu->addAction(mUndoSelection);

        // BP Menu
        mBPMenu->clear();
        // Soft BP
        mBPMenu->addAction(mToggleInt3BpAction);
        // Hardware BP
        if((wBpType & bp_hardware) == bp_hardware)
        {
            mBPMenu->addAction(mClearHwBpAction);
        }
        else
        {
            BPMAP wBPList;
            DbgGetBpList(bp_hardware, &wBPList);

            //get enabled hwbp count
            int enabledCount = wBPList.count;
            for(int i = 0; i < wBPList.count; i++)
                if(!wBPList.bp[i].enabled)
                    enabledCount--;

            if(enabledCount < 4)
            {
                mBPMenu->addAction(mSetHwBpAction);
            }
            else
            {
                REGDUMP wRegDump;
                DbgGetRegDump(&wRegDump);

                for(wI = 0; wI < 4; wI++)
                {
                    switch(wBPList.bp[wI].slot)
                    {
                    case 0:
                        msetHwBPOnSlot0Action->setText("Replace Slot 0 (0x" + QString("%1").arg(wBPList.bp[wI].addr, 8, 16, QChar('0')).toUpper() + ")");
                        break;
                    case 1:
                        msetHwBPOnSlot1Action->setText("Replace Slot 1 (0x" + QString("%1").arg(wBPList.bp[wI].addr, 8, 16, QChar('0')).toUpper() + ")");
                        break;
                    case 2:
                        msetHwBPOnSlot2Action->setText("Replace Slot 2 (0x" + QString("%1").arg(wBPList.bp[wI].addr, 8, 16, QChar('0')).toUpper() + ")");
                        break;
                    case 3:
                        msetHwBPOnSlot3Action->setText("Replace Slot 3 (0x" + QString("%1").arg(wBPList.bp[wI].addr, 8, 16, QChar('0')).toUpper() + ")");
                        break;
                    default:
                        break;
                    }
                }

                mHwSlotSelectMenu->addAction(msetHwBPOnSlot0Action);
                mHwSlotSelectMenu->addAction(msetHwBPOnSlot1Action);
                mHwSlotSelectMenu->addAction(msetHwBPOnSlot2Action);
                mHwSlotSelectMenu->addAction(msetHwBPOnSlot3Action);
                mBPMenu->addMenu(mHwSlotSelectMenu);
            }
            if(wBPList.count)
                BridgeFree(wBPList.bp);
        }
        wMenu->addMenu(mBPMenu);
        wMenu->addMenu(mFollowMenu);
        setupFollowReferenceMenu(wVA, mFollowMenu, false);
        if(DbgFunctions()->GetSourceFromAddr(wVA, 0, 0))
            wMenu->addAction(mOpenSourceAction);

        mDecompileMenu->clear();
        if(DbgFunctionGet(wVA, 0, 0))
            mDecompileMenu->addAction(mDecompileFunctionAction);
        mDecompileMenu->addAction(mDecompileSelectionAction);
        wMenu->addMenu(mDecompileMenu);

        wMenu->addAction(mEnableHighlightingMode);
        wMenu->addSeparator();

        wMenu->addAction(mSetLabelAction);
        wMenu->addAction(mSetCommentAction);
        wMenu->addAction(mSetBookmarkAction);

        duint selection_start = rvaToVa(getSelectionStart());
        duint selection_end = rvaToVa(getSelectionEnd());
        if(!DbgFunctionOverlaps(selection_start, selection_end))
        {
            mToggleFunctionAction->setText("Add function");
            wMenu->addAction(mToggleFunctionAction);
        }
        else if(DbgFunctionOverlaps(selection_start, selection_end))
        {
            mToggleFunctionAction->setText("Delete function");
            wMenu->addAction(mToggleFunctionAction);
        }

        wMenu->addAction(mAssembleAction);

        wMenu->addAction(mPatchesAction);
        wMenu->addAction(mYaraAction);

        wMenu->addSeparator();

        // New origin
        wMenu->addAction(mSetNewOriginHere);

        // Goto Menu
        mGotoMenu->clear();
        mGotoMenu->addAction(mGotoOriginAction);
        if(historyHasPrevious())
            mGotoMenu->addAction(mGotoPreviousAction);
        if(historyHasNext())
            mGotoMenu->addAction(mGotoNextAction);
        mGotoMenu->addAction(mGotoExpressionAction);
        char modname[MAX_MODULE_SIZE] = "";
        if(DbgGetModuleAt(wVA, modname))
            mGotoMenu->addAction(mGotoFileOffsetAction);
        mGotoMenu->addAction(mGotoStartAction);
        mGotoMenu->addAction(mGotoEndAction);
        wMenu->addMenu(mGotoMenu);
        wMenu->addSeparator();

        wMenu->addMenu(mSearchMenu);

        wMenu->addMenu(mReferencesMenu);
        setupFollowReferenceMenu(wVA, mReferencesMenu, true);

        wMenu->addSeparator();
        wMenu->addActions(mPluginMenu->actions());

        wMenu->exec(event->globalPos());
    }
}


/************************************************************************************
                         Context Menu Management
************************************************************************************/
void CPUDisassembly::setupRightClickContextMenu()
{
    mBinaryMenu = new QMenu("&Binary", this);
    mBinaryMenu->setIcon(QIcon(":/icons/images/binary.png"));
    mBinaryEditAction = makeShortcutMenuAction(mBinaryMenu, "&Edit", SLOT(binaryEditSlot()), "ActionBinaryEdit");
    mBinaryFillAction = makeShortcutMenuAction(mBinaryMenu, "&Fill...", SLOT(binaryFillSlot()), "ActionBinaryFill");
    mBinaryFillNopsAction = makeShortcutMenuAction(mBinaryMenu, "Fill with &NOPs", SLOT(binaryFillNopsSlot()), "ActionBinaryFillNops");
    mBinaryMenu->addSeparator();
    mBinaryCopyAction = makeShortcutMenuAction(mBinaryMenu, "&Copy", SLOT(binaryCopySlot()), "ActionBinaryCopy");
    mBinaryPasteAction = makeShortcutMenuAction(mBinaryMenu, "&Paste", SLOT(binaryPasteSlot()), "ActionBinaryPaste");
    mBinaryPasteIgnoreSizeAction = makeShortcutMenuAction(mBinaryMenu, "Paste (&Ignore Size)", SLOT(binaryPasteIgnoreSizeSlot()), "ActionBinaryPasteIgnoreSize");

    mUndoSelection = makeShortcutAction("&Restore selection", SLOT(undoSelectionSlot()), "ActionUndoSelection");
    mSetLabelAction = makeShortcutAction(QIcon(":/icons/images/label.png"), "Label", SLOT(setLabelSlot()), "ActionSetLabel");
    mSetCommentAction = makeShortcutAction(QIcon(":/icons/images/comment.png"), "Comment", SLOT(setCommentSlot()), "ActionSetComment");
    mSetBookmarkAction = makeShortcutAction(QIcon(":/icons/images/bookmark.png"), "Bookmark", SLOT(setBookmarkSlot()), "ActionToggleBookmark");
    mToggleFunctionAction = makeShortcutAction(QIcon(":/icons/images/functions.png"), "Function", SLOT(toggleFunctionSlot()), "ActionToggleFunction");
    mAssembleAction = makeShortcutAction("Assemble", SLOT(assembleSlot()), "ActionAssemble");

    mBPMenu = new QMenu("Breakpoint", this);
    mBPMenu->setIcon(QIcon(":/icons/images/breakpoint.png"));
    mToggleInt3BpAction = makeShortcutAction("Toggle", SLOT(toggleInt3BPActionSlot()), "ActionToggleBreakpoint");

    mHwSlotSelectMenu = new QMenu("Set Hardware on Execution", this);
    mSetHwBpAction = makeAction("Set Hardware on Execution", SLOT(toggleHwBpActionSlot()));
    mClearHwBpAction = makeAction("Remove Hardware", SLOT(toggleHwBpActionSlot()));
    msetHwBPOnSlot0Action = makeAction("Set Hardware on Execution on Slot 0 (Free)", SLOT(setHwBpOnSlot0ActionSlot()));
    msetHwBPOnSlot1Action = makeAction("Set Hardware on Execution on Slot 1 (Free)", SLOT(setHwBpOnSlot1ActionSlot()));
    msetHwBPOnSlot2Action = makeAction("Set Hardware on Execution on Slot 2 (Free)", SLOT(setHwBpOnSlot2ActionSlot()));
    msetHwBPOnSlot3Action = makeAction("Set Hardware on Execution on Slot 3 (Free)", SLOT(setHwBpOnSlot3ActionSlot()));

    mPatchesAction = makeShortcutAction(QIcon(":/icons/images/patch.png"), "Patches", SLOT(showPatchesSlot()), "ViewPatches");
    removeAction(mPatchesAction); //prevent conflicting shortcut with the MainWindow
    mYaraAction = makeShortcutAction(QIcon(":/icons/images/yara.png"), "&Yara...", SLOT(yaraSlot()), "ActionYara");
    mSetNewOriginHere = makeShortcutAction("Set New Origin Here", SLOT(setNewOriginHereActionSlot()), "ActionSetNewOriginHere");

    mGotoMenu = new QMenu("Go to", this);
    mGotoOriginAction = makeShortcutAction("Origin", SLOT(gotoOriginSlot()), "ActionGotoOrigin");
    mGotoPreviousAction = makeShortcutAction("Previous", SLOT(gotoPreviousSlot()), "ActionGotoPrevious");
    mGotoNextAction = makeShortcutAction("Next", SLOT(gotoNextSlot()), "ActionGotoNext");
    mGotoExpressionAction = makeShortcutAction("Expression", SLOT(gotoExpressionSlot()), "ActionGotoExpression");
    mGotoFileOffsetAction = makeShortcutAction("File Offset", SLOT(gotoFileOffsetSlot()), "ActionGotoFileOffset");
    mGotoStartAction = makeShortcutAction("Start of Page", SLOT(gotoStartSlot()), "ActionGotoStart");
    mGotoEndAction = makeShortcutAction("End of Page", SLOT(gotoEndSlot()), "ActionGotoEnd");

    mFollowMenu = new QMenu("&Follow in Dump", this);

    mCopyMenu = new QMenu("&Copy", this);
    mCopyMenu->setIcon(QIcon(":/icons/images/copy.png"));
    mCopySelectionAction = makeShortcutMenuAction(mCopyMenu, "&Selection", SLOT(copySelectionSlot()), "ActionCopy");
    mCopySelectionNoBytesAction = makeMenuAction(mCopyMenu, "Selection (&No Bytes)", SLOT(copySelectionNoBytesSlot()));
    mCopyAddressAction = makeShortcutMenuAction(mCopyMenu, "&Address", SLOT(copyAddressSlot()), "ActionCopyAddress");
    mCopyRvaAction = makeMenuAction(mCopyMenu, "&RVA", SLOT(copyRvaSlot()));
    mCopyDisassemblyAction = makeMenuAction(mCopyMenu, "Disassembly", SLOT(copyDisassemblySlot()));

    mOpenSourceAction = makeAction(QIcon(":/icons/images/source.png"), "Open Source File", SLOT(openSourceSlot()));

    mDecompileMenu = new QMenu("Decompile");
    mDecompileMenu->setIcon(QIcon(":/icons/images/snowman.png"));
    mDecompileSelectionAction = makeShortcutAction("Selection", SLOT(decompileSelectionSlot()), "ActionDecompileSelection");
    mDecompileFunctionAction = makeShortcutAction("Function", SLOT(decompileFunctionSlot()), "ActionDecompileFunction");

    mReferencesMenu = new QMenu("Find &references to", this);
    mReferenceSelectedAddressAction = makeShortcutAction("&Selected Address(es)", SLOT(findReferencesSlot()), "ActionFindReferencesToSelectedAddress");
    mReferenceSelectedAddressAction->setFont(QFont("Courier New", 8));

    mSearchMenu = new QMenu("&Search for", this);
    mSearchMenu->setIcon(QIcon(":/icons/images/search-for.png"));
    mSearchCommand = makeShortcutMenuAction(mSearchMenu, "C&ommand", SLOT(findCommandSlot()), "ActionFind");
    mSearchConstant = makeMenuAction(mSearchMenu, "&Constant", SLOT(findConstantSlot()));
    mSearchStrings = makeMenuAction(mSearchMenu, "&String references", SLOT(findStringsSlot()));
    mSearchCalls = makeMenuAction(mSearchMenu, "&Intermodular calls", SLOT(findCallsSlot()));
    mSearchPattern = makeShortcutMenuAction(mSearchMenu, "&Pattern", SLOT(findPatternSlot()), "ActionFindPattern");

    mEnableHighlightingMode = makeShortcutAction(QIcon(":/icons/images/highlight.png"), "&Highlighting mode", SLOT(enableHighlightingModeSlot()), "ActionHighlightingMode");

    // Plugins
    mPluginMenu = new QMenu(this);
    Bridge::getBridge()->emitMenuAddToList(this, mPluginMenu, GUI_DISASM_MENU);
}

void CPUDisassembly::gotoOriginSlot()
{
    if(!DbgIsDebugging())
        return;
    DbgCmdExec("disasm cip");
}


void CPUDisassembly::toggleInt3BPActionSlot()
{
    if(!DbgIsDebugging())
        return;
    duint wVA = rvaToVa(getInitialSelection());
    BPXTYPE wBpType = DbgGetBpxTypeAt(wVA);
    QString wCmd;

    if((wBpType & bp_normal) == bp_normal)
    {
        wCmd = "bc " + QString("%1").arg(wVA, sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    }
    else
    {
        wCmd = "bp " + QString("%1").arg(wVA, sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    }

    DbgCmdExec(wCmd.toUtf8().constData());
    //emit Disassembly::repainted();
}


void CPUDisassembly::toggleHwBpActionSlot()
{
    duint wVA = rvaToVa(getInitialSelection());
    BPXTYPE wBpType = DbgGetBpxTypeAt(wVA);
    QString wCmd;

    if((wBpType & bp_hardware) == bp_hardware)
    {
        wCmd = "bphwc " + QString("%1").arg(wVA, sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    }
    else
    {
        wCmd = "bphws " + QString("%1").arg(wVA, sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    }

    DbgCmdExec(wCmd.toUtf8().constData());
}


void CPUDisassembly::setHwBpOnSlot0ActionSlot()
{
    setHwBpAt(rvaToVa(getInitialSelection()), 0);
}

void CPUDisassembly::setHwBpOnSlot1ActionSlot()
{
    setHwBpAt(rvaToVa(getInitialSelection()), 1);
}

void CPUDisassembly::setHwBpOnSlot2ActionSlot()
{
    setHwBpAt(rvaToVa(getInitialSelection()), 2);
}

void CPUDisassembly::setHwBpOnSlot3ActionSlot()
{
    setHwBpAt(rvaToVa(getInitialSelection()), 3);
}

void CPUDisassembly::setHwBpAt(duint va, int slot)
{
    BPXTYPE wBpType = DbgGetBpxTypeAt(va);

    if((wBpType & bp_hardware) == bp_hardware)
    {
        mBPMenu->addAction(mClearHwBpAction);
    }


    int wI = 0;
    int wSlotIndex = -1;
    BPMAP wBPList;
    QString wCmd = "";

    DbgGetBpList(bp_hardware, &wBPList);

    // Find index of slot slot in the list
    for(wI = 0; wI < wBPList.count; wI++)
    {
        if(wBPList.bp[wI].slot == (unsigned short)slot)
        {
            wSlotIndex = wI;
            break;
        }
    }

    if(wSlotIndex < 0) // Slot not used
    {
        wCmd = "bphws " + QString("%1").arg(va, sizeof(dsint) * 2, 16, QChar('0')).toUpper();
        DbgCmdExec(wCmd.toUtf8().constData());
    }
    else // Slot used
    {
        wCmd = "bphwc " + QString("%1").arg((duint)(wBPList.bp[wSlotIndex].addr), sizeof(duint) * 2, 16, QChar('0')).toUpper();
        DbgCmdExec(wCmd.toUtf8().constData());

        Sleep(200);

        wCmd = "bphws " + QString("%1").arg(va, sizeof(dsint) * 2, 16, QChar('0')).toUpper();
        DbgCmdExec(wCmd.toUtf8().constData());
    }
    if(wBPList.count)
        BridgeFree(wBPList.bp);
}

void CPUDisassembly::setNewOriginHereActionSlot()
{
    if(!DbgIsDebugging())
        return;
    duint wVA = rvaToVa(getInitialSelection());
    QString wCmd = "cip=" + QString("%1").arg(wVA, sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(wCmd.toUtf8().constData());
}

void CPUDisassembly::setLabelSlot()
{
    if(!DbgIsDebugging())
        return;
    duint wVA = rvaToVa(getInitialSelection());
    LineEditDialog mLineEdit(this);
    QString addr_text = QString("%1").arg(wVA, sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    char label_text[MAX_COMMENT_SIZE] = "";
    if(DbgGetLabelAt((duint)wVA, SEG_DEFAULT, label_text))
        mLineEdit.setText(QString(label_text));
    mLineEdit.setWindowTitle("Add label at " + addr_text);
    if(mLineEdit.exec() != QDialog::Accepted)
        return;
    if(!DbgSetLabelAt(wVA, mLineEdit.editText.toUtf8().constData()))
    {
        QMessageBox msg(QMessageBox::Critical, "Error!", "DbgSetLabelAt failed!");
        msg.setWindowIcon(QIcon(":/icons/images/compile-error.png"));
        msg.setParent(this, Qt::Dialog);
        msg.setWindowFlags(msg.windowFlags() & (~Qt::WindowContextHelpButtonHint));
        msg.exec();
    }
    GuiUpdateAllViews();
}

void CPUDisassembly::setCommentSlot()
{
    if(!DbgIsDebugging())
        return;
    duint wVA = rvaToVa(getInitialSelection());
    LineEditDialog mLineEdit(this);
    QString addr_text = QString("%1").arg(wVA, sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    char comment_text[MAX_COMMENT_SIZE] = "";
    if(DbgGetCommentAt((duint)wVA, comment_text))
    {
        if(comment_text[0] == '\1') //automatic comment
            mLineEdit.setText(QString(comment_text + 1));
        else
            mLineEdit.setText(QString(comment_text));
    }
    mLineEdit.setWindowTitle("Add comment at " + addr_text);
    if(mLineEdit.exec() != QDialog::Accepted)
        return;
    if(!DbgSetCommentAt(wVA, mLineEdit.editText.replace('\r', "").replace('\n', "").toUtf8().constData()))
    {
        QMessageBox msg(QMessageBox::Critical, "Error!", "DbgSetCommentAt failed!");
        msg.setWindowIcon(QIcon(":/icons/images/compile-error.png"));
        msg.setParent(this, Qt::Dialog);
        msg.setWindowFlags(msg.windowFlags() & (~Qt::WindowContextHelpButtonHint));
        msg.exec();
    }
    GuiUpdateAllViews();
}

void CPUDisassembly::setBookmarkSlot()
{
    if(!DbgIsDebugging())
        return;
    duint wVA = rvaToVa(getInitialSelection());
    bool result;
    if(DbgGetBookmarkAt(wVA))
        result = DbgSetBookmarkAt(wVA, false);
    else
        result = DbgSetBookmarkAt(wVA, true);
    if(!result)
    {
        QMessageBox msg(QMessageBox::Critical, "Error!", "DbgSetBookmarkAt failed!");
        msg.setWindowIcon(QIcon(":/icons/images/compile-error.png"));
        msg.setParent(this, Qt::Dialog);
        msg.setWindowFlags(msg.windowFlags() & (~Qt::WindowContextHelpButtonHint));
        msg.exec();
    }
    GuiUpdateAllViews();
}

void CPUDisassembly::toggleFunctionSlot()
{
    if(!DbgIsDebugging())
        return;
    duint start = rvaToVa(getSelectionStart());
    duint end = rvaToVa(getSelectionEnd());
    duint function_start = 0;
    duint function_end = 0;
    if(!DbgFunctionOverlaps(start, end))
    {
        QString start_text = QString("%1").arg(start, sizeof(dsint) * 2, 16, QChar('0')).toUpper();
        QString end_text = QString("%1").arg(end, sizeof(dsint) * 2, 16, QChar('0')).toUpper();
        char labeltext[MAX_LABEL_SIZE] = "";
        QString label_text = "";
        if(DbgGetLabelAt(start, SEG_DEFAULT, labeltext))
            label_text = " (" + QString(labeltext) + ")";

        QMessageBox msg(QMessageBox::Question, "Define function?", start_text + "-" + end_text + label_text, QMessageBox::Yes | QMessageBox::No);
        msg.setWindowIcon(QIcon(":/icons/images/compile.png"));
        msg.setParent(this, Qt::Dialog);
        msg.setWindowFlags(msg.windowFlags() & (~Qt::WindowContextHelpButtonHint));
        if(msg.exec() != QMessageBox::Yes)
            return;
        QString cmd = "functionadd " + start_text + "," + end_text;
        DbgCmdExec(cmd.toUtf8().constData());
    }
    else
    {
        for(duint i = start; i <= end; i++)
        {
            if(DbgFunctionGet(i, &function_start, &function_end))
                break;
        }
        QString start_text = QString("%1").arg(function_start, sizeof(dsint) * 2, 16, QChar('0')).toUpper();
        QString end_text = QString("%1").arg(function_end, sizeof(dsint) * 2, 16, QChar('0')).toUpper();
        char labeltext[MAX_LABEL_SIZE] = "";
        QString label_text = "";
        if(DbgGetLabelAt(function_start, SEG_DEFAULT, labeltext))
            label_text = " (" + QString(labeltext) + ")";

        QMessageBox msg(QMessageBox::Warning, "Delete function?", start_text + "-" + end_text + label_text, QMessageBox::Ok | QMessageBox::Cancel);
        msg.setDefaultButton(QMessageBox::Cancel);
        msg.setWindowIcon(QIcon(":/icons/images/compile-warning.png"));
        msg.setParent(this, Qt::Dialog);
        msg.setWindowFlags(msg.windowFlags() & (~Qt::WindowContextHelpButtonHint));
        if(msg.exec() != QMessageBox::Ok)
            return;
        QString cmd = "functiondel " + start_text;
        DbgCmdExec(cmd.toUtf8().constData());
    }
}

void CPUDisassembly::assembleSlot()
{
    if(!DbgIsDebugging())
        return;

    do
    {
        dsint wRVA = getInitialSelection();
        duint wVA = rvaToVa(wRVA);
        QString addr_text = QString("%1").arg(wVA, sizeof(dsint) * 2, 16, QChar('0')).toUpper();

        QByteArray wBuffer;

        dsint wMaxByteCountToRead = 16 * 2;

        //TODO: fix size problems
        dsint size = getSize();
        if(!size)
            size = wRVA;

        // Bounding
        wMaxByteCountToRead = wMaxByteCountToRead > (size - wRVA) ? (size - wRVA) : wMaxByteCountToRead;

        wBuffer.resize(wMaxByteCountToRead);

        mMemPage->read(reinterpret_cast<byte_t*>(wBuffer.data()), wRVA, wMaxByteCountToRead);

        QBeaEngine disasm(-1);
        Instruction_t instr = disasm.DisassembleAt(reinterpret_cast<byte_t*>(wBuffer.data()), wMaxByteCountToRead, 0, 0, wVA);

        QString actual_inst = instr.instStr;

        bool assembly_error;
        do
        {
            assembly_error = false;

            LineEditDialog mLineEdit(this);
            mLineEdit.setText(actual_inst);
            mLineEdit.setWindowTitle("Assemble at " + addr_text);
            mLineEdit.setCheckBoxText("&Fill with NOPs");
            mLineEdit.enableCheckBox(true);
            mLineEdit.setCheckBox(ConfigBool("Disassembler", "FillNOPs"));
            if(mLineEdit.exec() != QDialog::Accepted)
                return;

            //if the instruction its unkown or is the old instruction or empty (easy way to skip from GUI) skipping
            if(mLineEdit.editText == QString("???") || mLineEdit.editText.toLower() == instr.instStr.toLower() || mLineEdit.editText == QString(""))
                break;

            Config()->setBool("Disassembler", "FillNOPs", mLineEdit.bChecked);

            char error[MAX_ERROR_SIZE] = "";
            if(!DbgFunctions()->AssembleAtEx(wVA, mLineEdit.editText.toUtf8().constData(), error, mLineEdit.bChecked))
            {
                QMessageBox msg(QMessageBox::Critical, "Error!", "Failed to assemble instruction \"" + mLineEdit.editText + "\" (" + error + ")");
                msg.setWindowIcon(QIcon(":/icons/images/compile-error.png"));
                msg.setParent(this, Qt::Dialog);
                msg.setWindowFlags(msg.windowFlags() & (~Qt::WindowContextHelpButtonHint));
                msg.exec();
                actual_inst = mLineEdit.editText;
                assembly_error = true;
            }
        }
        while(assembly_error);

        //select next instruction after assembling
        setSingleSelection(wRVA);

        dsint botRVA = getTableOffset();
        dsint topRVA = getInstructionRVA(getTableOffset(), getNbrOfLineToPrint() - 1);

        dsint wInstrSize = getInstructionRVA(wRVA, 1) - wRVA - 1;

        expandSelectionUpTo(wRVA + wInstrSize);
        selectNext(false);

        if(getSelectionStart() < botRVA)
            setTableOffset(getSelectionStart());
        else if(getSelectionEnd() >= topRVA)
            setTableOffset(getInstructionRVA(getSelectionEnd(), -getNbrOfLineToPrint() + 2));

        //refresh view
        GuiUpdateAllViews();
    }
    while(1);
}

void CPUDisassembly::gotoExpressionSlot()
{
    if(!DbgIsDebugging())
        return;
    if(!mGoto)
        mGoto = new GotoDialog(this);
    if(mGoto->exec() == QDialog::Accepted)
    {
        DbgCmdExec(QString().sprintf("disasm \"%s\"", mGoto->expressionText.toUtf8().constData()).toUtf8().constData());
    }
}

void CPUDisassembly::gotoFileOffsetSlot()
{
    if(!DbgIsDebugging())
        return;
    char modname[MAX_MODULE_SIZE] = "";
    if(!DbgFunctions()->ModNameFromAddr(rvaToVa(getInitialSelection()), modname, true))
    {
        QMessageBox::critical(this, "Error!", "Not inside a module...");
        return;
    }
    GotoDialog mGotoDialog(this);
    mGotoDialog.fileOffset = true;
    mGotoDialog.modName = QString(modname);
    mGotoDialog.setWindowTitle("Goto File Offset in " + QString(modname));
    if(mGotoDialog.exec() != QDialog::Accepted)
        return;
    duint value = DbgValFromString(mGotoDialog.expressionText.toUtf8().constData());
    value = DbgFunctions()->FileOffsetToVa(modname, value);
    DbgCmdExec(QString().sprintf("disasm \"%p\"", value).toUtf8().constData());
}

void CPUDisassembly::gotoStartSlot()
{
    duint dest = mMemPage->getBase();
    DbgCmdExec(QString().sprintf("disasm \"%p\"", dest).toUtf8().constData());
}

void CPUDisassembly::gotoEndSlot()
{
    duint dest = mMemPage->getBase() + mMemPage->getSize() - (getViewableRowsCount() * 16);
    DbgCmdExec(QString().sprintf("disasm \"%p\"", dest).toUtf8().constData());
}

void CPUDisassembly::followActionSlot()
{
    QAction* action = qobject_cast<QAction*>(sender());
    if(!action)
        return;
    if(action->objectName().startsWith("DUMP|"))
        DbgCmdExec(QString().sprintf("dump \"%s\"", action->objectName().mid(5).toUtf8().constData()).toUtf8().constData());
    else if(action->objectName().startsWith("REF|"))
    {
        QString addrText = QString("%1").arg(rvaToVa(getInitialSelection()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
        QString value = action->objectName().mid(4);
        DbgCmdExec(QString("findref \"" + value +  "\", " + addrText).toUtf8().constData());
        emit displayReferencesWidget();
    }
}

void CPUDisassembly::gotoPreviousSlot()
{
    historyPrevious();
}

void CPUDisassembly::gotoNextSlot()
{
    historyNext();
}

void CPUDisassembly::findReferencesSlot()
{
    QString addrStart = QString("%1").arg(rvaToVa(getSelectionStart()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    QString addrEnd = QString("%1").arg(rvaToVa(getSelectionEnd()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    QString addrDisasm = QString("%1").arg(rvaToVa(getInitialSelection()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("findrefrange " + addrStart + ", " + addrEnd + ", " + addrDisasm).toUtf8().constData());
    emit displayReferencesWidget();
}

void CPUDisassembly::findConstantSlot()
{
    WordEditDialog wordEdit(this);
    wordEdit.setup("Enter Constant", 0, sizeof(dsint));
    if(wordEdit.exec() != QDialog::Accepted) //cancel pressed
        return;
    QString addrText = QString("%1").arg(rvaToVa(getInitialSelection()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    QString constText = QString("%1").arg(wordEdit.getVal(), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("findref " + constText + ", " + addrText).toUtf8().constData());
    emit displayReferencesWidget();
}

void CPUDisassembly::findStringsSlot()
{
    QString addrText = QString("%1").arg(rvaToVa(getInitialSelection()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("strref " + addrText).toUtf8().constData());
    emit displayReferencesWidget();
}

void CPUDisassembly::findCallsSlot()
{
    QString addrText = QString("%1").arg(rvaToVa(getInitialSelection()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("modcallfind " + addrText).toUtf8().constData());
    emit displayReferencesWidget();
}

void CPUDisassembly::findPatternSlot()
{
    HexEditDialog hexEdit(this);
    hexEdit.showEntireBlock(true);
    hexEdit.mHexEdit->setOverwriteMode(false);
    hexEdit.setWindowTitle("Find Pattern...");
    if(hexEdit.exec() != QDialog::Accepted)
        return;
    dsint addr = rvaToVa(getSelectionStart());
    if(hexEdit.entireBlock())
        addr = DbgMemFindBaseAddr(addr, 0);
    QString addrText = QString("%1").arg(addr, sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    DbgCmdExec(QString("findall " + addrText + ", " + hexEdit.mHexEdit->pattern()).toUtf8().constData());
    emit displayReferencesWidget();
}

void CPUDisassembly::selectionGetSlot(SELECTIONDATA* selection)
{
    selection->start = rvaToVa(getSelectionStart());
    selection->end = rvaToVa(getSelectionEnd());
    Bridge::getBridge()->setResult(1);
}

void CPUDisassembly::selectionSetSlot(const SELECTIONDATA* selection)
{
    dsint selMin = getBase();
    dsint selMax = selMin + getSize();
    dsint start = selection->start;
    dsint end = selection->end;
    if(start < selMin || start >= selMax || end < selMin || end >= selMax) //selection out of range
    {
        Bridge::getBridge()->setResult(0);
        return;
    }
    setSingleSelection(start - selMin);
    expandSelectionUpTo(end - selMin);
    reloadData();
    Bridge::getBridge()->setResult(1);
}

void CPUDisassembly::enableHighlightingModeSlot()
{
    if(mHighlightingMode)
        mHighlightingMode = false;
    else
        mHighlightingMode = true;
    reloadData();
}

void CPUDisassembly::binaryEditSlot()
{
    HexEditDialog hexEdit(this);
    dsint selStart = getSelectionStart();
    dsint selSize = getSelectionEnd() - selStart + 1;
    byte_t* data = new byte_t[selSize];
    mMemPage->read(data, selStart, selSize);
    hexEdit.mHexEdit->setData(QByteArray((const char*)data, selSize));
    delete [] data;
    hexEdit.setWindowTitle("Edit code at " + QString("%1").arg(rvaToVa(selStart), sizeof(dsint) * 2, 16, QChar('0')).toUpper());
    if(hexEdit.exec() != QDialog::Accepted)
        return;
    dsint dataSize = hexEdit.mHexEdit->data().size();
    dsint newSize = selSize > dataSize ? selSize : dataSize;
    data = new byte_t[newSize];
    mMemPage->read(data, selStart, newSize);
    QByteArray patched = hexEdit.mHexEdit->applyMaskedData(QByteArray((const char*)data, newSize));
    mMemPage->write(patched.constData(), selStart, patched.size());
    GuiUpdateAllViews();
}

void CPUDisassembly::binaryFillSlot()
{
    HexEditDialog hexEdit(this);
    hexEdit.mHexEdit->setOverwriteMode(false);
    dsint selStart = getSelectionStart();
    hexEdit.setWindowTitle("Fill code at " + QString("%1").arg(rvaToVa(selStart), sizeof(dsint) * 2, 16, QChar('0')).toUpper());
    if(hexEdit.exec() != QDialog::Accepted)
        return;
    QString pattern = hexEdit.mHexEdit->pattern();
    dsint selSize = getSelectionEnd() - selStart + 1;
    byte_t* data = new byte_t[selSize];
    mMemPage->read(data, selStart, selSize);
    hexEdit.mHexEdit->setData(QByteArray((const char*)data, selSize));
    delete [] data;
    hexEdit.mHexEdit->fill(0, QString(pattern));
    QByteArray patched(hexEdit.mHexEdit->data());
    mMemPage->write(patched, selStart, patched.size());
    GuiUpdateAllViews();
}

void CPUDisassembly::binaryFillNopsSlot()
{
    HexEditDialog hexEdit(this);
    dsint selStart = getSelectionStart();
    dsint selSize = getSelectionEnd() - selStart + 1;
    byte_t* data = new byte_t[selSize];
    mMemPage->read(data, selStart, selSize);
    hexEdit.mHexEdit->setData(QByteArray((const char*)data, selSize));
    delete [] data;
    hexEdit.mHexEdit->fill(0, QString("90"));
    QByteArray patched(hexEdit.mHexEdit->data());
    mMemPage->write(patched, selStart, patched.size());
    GuiUpdateAllViews();
}

void CPUDisassembly::binaryCopySlot()
{
    HexEditDialog hexEdit(this);
    dsint selStart = getSelectionStart();
    dsint selSize = getSelectionEnd() - selStart + 1;
    byte_t* data = new byte_t[selSize];
    mMemPage->read(data, selStart, selSize);
    hexEdit.mHexEdit->setData(QByteArray((const char*)data, selSize));
    delete [] data;
    Bridge::CopyToClipboard(hexEdit.mHexEdit->pattern(true));
}

void CPUDisassembly::binaryPasteSlot()
{
    HexEditDialog hexEdit(this);
    dsint selStart = getSelectionStart();
    dsint selSize = getSelectionEnd() - selStart + 1;
    QClipboard* clipboard = QApplication::clipboard();
    hexEdit.mHexEdit->setData(clipboard->text());

    byte_t* data = new byte_t[selSize];
    mMemPage->read(data, selStart, selSize);
    QByteArray patched = hexEdit.mHexEdit->applyMaskedData(QByteArray((const char*)data, selSize));
    if(patched.size() < selSize)
        selSize = patched.size();
    mMemPage->write(patched.constData(), selStart, selSize);
    GuiUpdateAllViews();
}

void CPUDisassembly::undoSelectionSlot()
{
    dsint start = rvaToVa(getSelectionStart());
    dsint end = rvaToVa(getSelectionEnd());
    if(!DbgFunctions()->PatchInRange(start, end)) //nothing patched in selected range
        return;
    DbgFunctions()->PatchRestoreRange(start, end);
    reloadData();
}

void CPUDisassembly::binaryPasteIgnoreSizeSlot()
{
    HexEditDialog hexEdit(this);
    dsint selStart = getSelectionStart();
    dsint selSize = getSelectionEnd() - selStart + 1;
    QClipboard* clipboard = QApplication::clipboard();
    hexEdit.mHexEdit->setData(clipboard->text());

    byte_t* data = new byte_t[selSize];
    mMemPage->read(data, selStart, selSize);
    QByteArray patched = hexEdit.mHexEdit->applyMaskedData(QByteArray((const char*)data, selSize));
    delete [] data;
    mMemPage->write(patched.constData(), selStart, patched.size());
    GuiUpdateAllViews();
}

void CPUDisassembly::showPatchesSlot()
{
    emit showPatches();
}

void CPUDisassembly::yaraSlot()
{
    YaraRuleSelectionDialog yaraDialog(this);
    if(yaraDialog.exec() == QDialog::Accepted)
    {
        QString addrText = QString("%1").arg(rvaToVa(getInitialSelection()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
        DbgCmdExec(QString("yara \"%0\",%1").arg(yaraDialog.getSelectedFile()).arg(addrText).toUtf8().constData());
        emit displayReferencesWidget();
    }
}

void CPUDisassembly::copySelectionSlot(bool copyBytes)
{
    QList<Instruction_t> instBuffer;
    prepareDataRange(getSelectionStart(), getSelectionEnd(), &instBuffer);
    QString clipboard = "";
    const int addressLen = getColumnWidth(0) / getCharWidth() - 1;
    const int bytesLen = getColumnWidth(1) / getCharWidth() - 1;
    const int disassemblyLen = getColumnWidth(2) / getCharWidth() - 1;
    for(int i = 0; i < instBuffer.size(); i++)
    {
        if(i)
            clipboard += "\r\n";
        dsint cur_addr = rvaToVa(instBuffer.at(i).rva);
        QString address = getAddrText(cur_addr, 0);
        QString bytes;
        for(int j = 0; j < instBuffer.at(i).dump.size(); j++)
        {
            if(j)
                bytes += " ";
            bytes += QString("%1").arg((unsigned char)(instBuffer.at(i).dump.at(j)), 2, 16, QChar('0')).toUpper();
        }
        QString disassembly;
        for(const auto & token : instBuffer.at(i).tokens.tokens)
            disassembly += token.text;
        char comment[MAX_COMMENT_SIZE] = "";
        QString fullComment;
        if(DbgGetCommentAt(cur_addr, comment))
        {
            if(comment[0] == '\1') //automatic comment
                fullComment = " ;" + QString(comment + 1);
            else
                fullComment = " ;" + QString(comment);
        }
        clipboard += address.leftJustified(addressLen, QChar(' '), true);
        if(copyBytes)
            clipboard += " | " + bytes.leftJustified(bytesLen, QChar(' '), true);
        clipboard += " | " + disassembly.leftJustified(disassemblyLen, QChar(' '), true) + " |" + fullComment;
    }
    Bridge::CopyToClipboard(clipboard);
}

void CPUDisassembly::copySelectionSlot()
{
    copySelectionSlot(true);
}

void CPUDisassembly::copySelectionNoBytesSlot()
{
    copySelectionSlot(false);
}

void CPUDisassembly::copyAddressSlot()
{
    QString addrText = QString("%1").arg(rvaToVa(getInitialSelection()), sizeof(dsint) * 2, 16, QChar('0')).toUpper();
    Bridge::CopyToClipboard(addrText);
}

void CPUDisassembly::copyRvaSlot()
{
    duint addr = rvaToVa(getInitialSelection());
    duint base = DbgFunctions()->ModBaseFromAddr(addr);
    if(base)
    {
        QString addrText = QString("%1").arg(addr - base, 0, 16, QChar('0')).toUpper();
        Bridge::CopyToClipboard(addrText);
    }
    else
        QMessageBox::warning(this, "Error!", "Selection not in a module...");
}

void CPUDisassembly::copyDisassemblySlot()
{
    QList<Instruction_t> instBuffer;
    prepareDataRange(getSelectionStart(), getSelectionEnd(), &instBuffer);
    QString clipboard = "";
    for(int i = 0; i < instBuffer.size(); i++)
    {
        if(i)
            clipboard += "\r\n";
        for(const auto & token : instBuffer.at(i).tokens.tokens)
            clipboard += token.text;
    }
    Bridge::CopyToClipboard(clipboard);
}

void CPUDisassembly::findCommandSlot()
{
    if(!DbgIsDebugging())
        return;

    LineEditDialog mLineEdit(this);
    mLineEdit.enableCheckBox(true);
    mLineEdit.setCheckBoxText("Entire &Block");
    mLineEdit.setCheckBox(ConfigBool("Disassembler", "FindCommandEntireBlock"));
    mLineEdit.setWindowTitle("Find Command");
    if(mLineEdit.exec() != QDialog::Accepted)
        return;
    Config()->setBool("Disassembler", "FindCommandEntireBlock", mLineEdit.bChecked);

    char error[MAX_ERROR_SIZE] = "";
    unsigned char dest[16];
    int asmsize = 0;
    duint va = rvaToVa(getInitialSelection());

    if(!DbgFunctions()->Assemble(va + mMemPage->getSize() / 2, dest, &asmsize, mLineEdit.editText.toUtf8().constData(), error))
    {
        QMessageBox msg(QMessageBox::Critical, "Error!", "Failed to assemble instruction \"" + mLineEdit.editText + "\" (" + error + ")");
        msg.setWindowIcon(QIcon(":/icons/images/compile-error.png"));
        msg.setParent(this, Qt::Dialog);
        msg.setWindowFlags(msg.windowFlags() & (~Qt::WindowContextHelpButtonHint));
        msg.exec();
        return;
    }

    QString addr_text = QString("%1").arg(va, sizeof(dsint) * 2, 16, QChar('0')).toUpper();

    if(!mLineEdit.bChecked)
    {
        dsint size = mMemPage->getSize();
        DbgCmdExec(QString("findasm \"%1\", %2, .%3").arg(mLineEdit.editText).arg(addr_text).arg(size).toUtf8().constData());
    }
    else
        DbgCmdExec(QString("findasm \"%1\", %2").arg(mLineEdit.editText).arg(addr_text).toUtf8().constData());

    emit displayReferencesWidget();
}

void CPUDisassembly::openSourceSlot()
{
    char szSourceFile[MAX_STRING_SIZE] = "";
    int line = 0;
    if(!DbgFunctions()->GetSourceFromAddr(rvaToVa(getInitialSelection()), szSourceFile, &line))
        return;
    Bridge::getBridge()->emitLoadSourceFile(szSourceFile, 0, line);
    emit displaySourceManagerWidget();
}

void CPUDisassembly::decompileSelectionSlot()
{
    dsint addr = rvaToVa(getSelectionStart());
    dsint size = getSelectionSize();
    emit displaySnowmanWidget();
    emit decompileAt(addr, addr + size);
}

void CPUDisassembly::decompileFunctionSlot()
{
    dsint addr = rvaToVa(getInitialSelection());
    duint start;
    duint end;
    if(DbgFunctionGet(addr, &start, &end))
    {
        emit displaySnowmanWidget();
        emit decompileAt(start, end);
    }
}

void CPUDisassembly::paintEvent(QPaintEvent* event)
{
    // Hook/hack to update the sidebar at the same time as this widget.
    // Ensures the two widgets are synced and prevents "draw lag"
    auto sidebar = mParentCPUWindow->getSidebarWidget();

    if (sidebar)
        sidebar->repaint();

    // Signal to render the original content
    Disassembly::paintEvent(event);
}
