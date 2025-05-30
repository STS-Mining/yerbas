// Copyright (c) 2011-2015 The Bitcoin Core developers
// Copyright (c) 2014-2020 The Dash Core developers
// Copyright (c) 2020 The Memeium developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "guiutil.h"

#include "bitcoinaddressvalidator.h"
#include "bitcoinunits.h"
#include "qvalidatedlineedit.h"
#include "walletmodel.h"

#include "fs.h"
#include "init.h"
#include "policy/policy.h"
#include "primitives/transaction.h"
#include "protocol.h"
#include "script/script.h"
#include "script/standard.h"
#include "util.h"

#ifdef WIN32
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0501
#ifdef _WIN32_IE
#undef _WIN32_IE
#endif
#define _WIN32_IE 0x0501
#define WIN32_LEAN_AND_MEAN 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "shellapi.h"
#include "shlobj.h"
#include "shlwapi.h"
#endif

#include <boost/scoped_array.hpp>

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDesktopWidget>
#include <QDoubleValidator>
#include <QFileDialog>
#include <QFont>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QTextDocument> // for Qt::mightBeRichText
#include <QThread>

#if QT_VERSION < 0x050000
#include <QUrl>
#else
#include <QUrlQuery>
#endif

#if QT_VERSION >= 0x50200
#include <QFontDatabase>
#endif

static fs::detail::utf8_codecvt_facet utf8;

#if defined(Q_OS_MAC)
extern double NSAppKitVersionNumber;
#if !defined(NSAppKitVersionNumber10_8)
#define NSAppKitVersionNumber10_8 1187
#endif
#if !defined(NSAppKitVersionNumber10_9)
#define NSAppKitVersionNumber10_9 1265
#endif
#endif

namespace GUIUtil
{

// The theme to set by default if settings are missing or incorrect
static const QString defaultTheme = "Dark";
// The prefix a theme name should have if we want to apply dark colors and styles to it
static const QString darkThemePrefix = "Dark";

static const std::map<ThemedColor, QColor> themedColors = {
    {ThemedColor::DEFAULT, QColor(0, 0, 0)},
    {ThemedColor::UNCONFIRMED, QColor(128, 128, 128)},
    {ThemedColor::NEGATIVE, QColor(255, 0, 0)},
    {ThemedColor::BAREADDRESS, QColor(140, 140, 140)},
    {ThemedColor::TX_STATUS_OPENUNTILDATE, QColor(64, 64, 255)},
    {ThemedColor::TX_STATUS_OFFLINE, QColor(192, 192, 192)},
    {ThemedColor::TX_STATUS_DANGER, QColor(200, 100, 100)},
    {ThemedColor::TX_STATUS_LOCKED, QColor(0, 128, 255)},
};

static const std::map<ThemedColor, QColor> themedDarkColors = {
    {ThemedColor::DEFAULT, QColor(170, 170, 170)},
    {ThemedColor::UNCONFIRMED, QColor(204, 204, 204)},
    {ThemedColor::NEGATIVE, QColor(255, 69, 0)},
    {ThemedColor::BAREADDRESS, QColor(140, 140, 140)},
    {ThemedColor::TX_STATUS_OPENUNTILDATE, QColor(64, 64, 255)},
    {ThemedColor::TX_STATUS_OFFLINE, QColor(192, 192, 192)},
    {ThemedColor::TX_STATUS_DANGER, QColor(200, 100, 100)},
    {ThemedColor::TX_STATUS_LOCKED, QColor(0, 128, 255)},
};

static const std::map<ThemedStyle, QString> themedStyles = {
    {ThemedStyle::TS_INVALID, "background:#FF8080;"},
    {ThemedStyle::TS_ERROR, "color:red;"},
    {ThemedStyle::TS_SUCCESS, "color:green;"},
    {ThemedStyle::TS_COMMAND, "color:#006060;"},
    {ThemedStyle::TS_PRIMARY, "color:black;"},
    {ThemedStyle::TS_SECONDARY, "color:#808080;"},
};

static const std::map<ThemedStyle, QString> themedDarkStyles = {
    {ThemedStyle::TS_INVALID, "background:#ff4500;"},
    {ThemedStyle::TS_ERROR, "color:#ff4500;"},
    {ThemedStyle::TS_SUCCESS, "color:green;"},
    {ThemedStyle::TS_COMMAND, "color:#0cc;"},
    {ThemedStyle::TS_PRIMARY, "color:#ccc;"},
    {ThemedStyle::TS_SECONDARY, "color:#aaa;"},
};

QColor getThemedQColor(ThemedColor color)
{
    QString theme = QSettings().value("theme", "").toString();
    return theme.startsWith(darkThemePrefix) ? themedDarkColors.at(color) : themedColors.at(color);
}

QString getThemedStyleQString(ThemedStyle style)
{
    QString theme = QSettings().value("theme", "").toString();
    return theme.startsWith(darkThemePrefix) ? themedDarkStyles.at(style) : themedStyles.at(style);
}

QString dateTimeStr(const QDateTime& date)
{
    return date.date().toString(Qt::SystemLocaleShortDate) + QString(" ") + date.toString("hh:mm");
}

QString dateTimeStr(qint64 nTime)
{
    return dateTimeStr(QDateTime::fromTime_t((qint32)nTime));
}

QFont fixedPitchFont()
{
#if QT_VERSION >= 0x50200
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
#else
    QFont font("Monospace");
#if QT_VERSION >= 0x040800
    font.setStyleHint(QFont::Monospace);
#else
    font.setStyleHint(QFont::TypeWriter);
#endif
    return font;
#endif
}

// Just some dummy data to generate an convincing random-looking (but consistent) address
static const uint8_t dummydata[] = {0xeb, 0x15, 0x23, 0x1d, 0xfc, 0xeb, 0x60, 0x92, 0x58, 0x86, 0xb6, 0x7d, 0x06, 0x52, 0x99, 0x92, 0x59, 0x15, 0xae, 0xb1, 0x72, 0xc0, 0x66, 0x47};

// Generate a dummy address with invalid CRC, starting with the network prefix.
static std::string DummyAddress(const CChainParams& params)
{
    std::vector<unsigned char> sourcedata = params.Base58Prefix(CChainParams::PUBKEY_ADDRESS);
    sourcedata.insert(sourcedata.end(), dummydata, dummydata + sizeof(dummydata));
    for (int i = 0; i < 256; ++i) { // Try every trailing byte
        std::string s = EncodeBase58(sourcedata.data(), sourcedata.data() + sourcedata.size());
        if (!CBitcoinAddress(s).IsValid())
            return s;
        sourcedata[sourcedata.size() - 1] += 1;
    }
    return "";
}

void setupAddressWidget(QValidatedLineEdit* widget, QWidget* parent)
{
    parent->setFocusProxy(widget);

    widget->setFont(fixedPitchFont());
#if QT_VERSION >= 0x040700
    // We don't want translators to use own addresses in translations
    // and this is the only place, where this address is supplied.
    widget->setPlaceholderText(QObject::tr("Enter a Memeium address (e.g. %1)").arg(QString::fromStdString(DummyAddress(Params()))));
#endif
    widget->setValidator(new BitcoinAddressEntryValidator(parent));
    widget->setCheckValidator(new BitcoinAddressCheckValidator(parent));
}

void setupAmountWidget(QLineEdit* widget, QWidget* parent)
{
    QDoubleValidator* amountValidator = new QDoubleValidator(parent);
    amountValidator->setDecimals(8);
    amountValidator->setBottom(0.0);
    widget->setValidator(amountValidator);
    widget->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
}

bool parseBitcoinURI(const QUrl& uri, SendCoinsRecipient* out)
{
    // return if URI is not valid or is no memeium: URI
    if (!uri.isValid() || uri.scheme() != QString("memeium"))
        return false;

    SendCoinsRecipient rv;
    rv.address = uri.path();
    // Trim any following forward slash which may have been added by the OS
    if (rv.address.endsWith("/")) {
        rv.address.truncate(rv.address.length() - 1);
    }
    rv.amount = 0;

#if QT_VERSION < 0x050000
    QList<QPair<QString, QString>> items = uri.queryItems();
#else
    QUrlQuery uriQuery(uri);
    QList<QPair<QString, QString>> items = uriQuery.queryItems();
#endif

    for (QList<QPair<QString, QString>>::iterator i = items.begin(); i != items.end(); i++) {
        bool fShouldReturnFalse = false;
        if (i->first.startsWith("req-")) {
            i->first.remove(0, 4);
            fShouldReturnFalse = true;
        }

        if (i->first == "label") {
            rv.label = i->second;
            fShouldReturnFalse = false;
        }
        if (i->first == "IS") {
            // we simply ignore IS
            fShouldReturnFalse = false;
        }
        if (i->first == "message") {
            rv.message = i->second;
            fShouldReturnFalse = false;
        } else if (i->first == "amount") {
            if (!i->second.isEmpty()) {
                if (!BitcoinUnits::parse(BitcoinUnits::MMM, i->second, &rv.amount)) {
                    return false;
                }
            }
            fShouldReturnFalse = false;
        }

        if (fShouldReturnFalse)
            return false;
    }
    if (out) {
        *out = rv;
    }
    return true;
}

bool parseBitcoinURI(QString uri, SendCoinsRecipient* out)
{
    // Convert memeium:// to memeium:
    //
    //    Cannot handle this later, because memeium:// will cause Qt to see the part after // as host,
    //    which will lower-case it (and thus invalidate the address).
    if (uri.startsWith("memeium://", Qt::CaseInsensitive)) {
        uri.replace(0, 7, "memeium:");
    }
    QUrl uriInstance(uri);
    return parseBitcoinURI(uriInstance, out);
}

QString formatBitcoinURI(const SendCoinsRecipient& info)
{
    QString ret = QString("memeium:%1").arg(info.address);
    int paramCount = 0;

    if (info.amount) {
        ret += QString("?amount=%1").arg(BitcoinUnits::format(BitcoinUnits::MMM, info.amount, false, BitcoinUnits::separatorNever));
        paramCount++;
    }

    if (!info.label.isEmpty()) {
        QString lbl(QUrl::toPercentEncoding(info.label));
        ret += QString("%1label=%2").arg(paramCount == 0 ? "?" : "&").arg(lbl);
        paramCount++;
    }

    if (!info.message.isEmpty()) {
        QString msg(QUrl::toPercentEncoding(info.message));
        ret += QString("%1message=%2").arg(paramCount == 0 ? "?" : "&").arg(msg);
        paramCount++;
    }

    return ret;
}

bool isDust(const QString& address, const CAmount& amount)
{
    CTxDestination dest = CBitcoinAddress(address.toStdString()).Get();
    CScript script = GetScriptForDestination(dest);
    CTxOut txOut(amount, script);
    return IsDust(txOut, ::dustRelayFee);
}

QString HtmlEscape(const QString& str, bool fMultiLine)
{
#if QT_VERSION < 0x050000
    QString escaped = Qt::escape(str);
#else
    QString escaped = str.toHtmlEscaped();
#endif
    escaped = escaped.replace(" ", "&nbsp;");
    if (fMultiLine) {
        escaped = escaped.replace("\n", "<br>\n");
    }
    return escaped;
}

QString HtmlEscape(const std::string& str, bool fMultiLine)
{
    return HtmlEscape(QString::fromStdString(str), fMultiLine);
}

void copyEntryData(QAbstractItemView* view, int column, int role)
{
    if (!view || !view->selectionModel())
        return;
    QModelIndexList selection = view->selectionModel()->selectedRows(column);

    if (!selection.isEmpty()) {
        // Copy first item
        setClipboard(selection.at(0).data(role).toString());
    }
}

QList<QModelIndex> getEntryData(QAbstractItemView* view, int column)
{
    if (!view || !view->selectionModel())
        return QList<QModelIndex>();
    return view->selectionModel()->selectedRows(column);
}

QString getSaveFileName(QWidget* parent, const QString& caption, const QString& dir, const QString& filter, QString* selectedSuffixOut)
{
    QString selectedFilter;
    QString myDir;
    if (dir.isEmpty()) // Default to user documents location
    {
#if QT_VERSION < 0x050000
        myDir = QDesktopServices::storageLocation(QDesktopServices::DocumentsLocation);
#else
        myDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
#endif
    } else {
        myDir = dir;
    }
    /* Directly convert path to native OS path separators */
    QString result = QDir::toNativeSeparators(QFileDialog::getSaveFileName(parent, caption, myDir, filter, &selectedFilter));

    /* Extract first suffix from filter pattern "Description (*.foo)" or "Description (*.foo *.bar ...) */
    QRegExp filter_re(".* \\(\\*\\.(.*)[ \\)]");
    QString selectedSuffix;
    if (filter_re.exactMatch(selectedFilter)) {
        selectedSuffix = filter_re.cap(1);
    }

    /* Add suffix if needed */
    QFileInfo info(result);
    if (!result.isEmpty()) {
        if (info.suffix().isEmpty() && !selectedSuffix.isEmpty()) {
            /* No suffix specified, add selected suffix */
            if (!result.endsWith("."))
                result.append(".");
            result.append(selectedSuffix);
        }
    }

    /* Return selected suffix if asked to */
    if (selectedSuffixOut) {
        *selectedSuffixOut = selectedSuffix;
    }
    return result;
}

QString getOpenFileName(QWidget* parent, const QString& caption, const QString& dir, const QString& filter, QString* selectedSuffixOut)
{
    QString selectedFilter;
    QString myDir;
    if (dir.isEmpty()) // Default to user documents location
    {
#if QT_VERSION < 0x050000
        myDir = QDesktopServices::storageLocation(QDesktopServices::DocumentsLocation);
#else
        myDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
#endif
    } else {
        myDir = dir;
    }
    /* Directly convert path to native OS path separators */
    QString result = QDir::toNativeSeparators(QFileDialog::getOpenFileName(parent, caption, myDir, filter, &selectedFilter));

    if (selectedSuffixOut) {
        /* Extract first suffix from filter pattern "Description (*.foo)" or "Description (*.foo *.bar ...) */
        QRegExp filter_re(".* \\(\\*\\.(.*)[ \\)]");
        QString selectedSuffix;
        if (filter_re.exactMatch(selectedFilter)) {
            selectedSuffix = filter_re.cap(1);
        }
        *selectedSuffixOut = selectedSuffix;
    }
    return result;
}

Qt::ConnectionType blockingGUIThreadConnection()
{
    if (QThread::currentThread() != qApp->thread()) {
        return Qt::BlockingQueuedConnection;
    } else {
        return Qt::DirectConnection;
    }
}

bool checkPoint(const QPoint& p, const QWidget* w)
{
    QWidget* atW = QApplication::widgetAt(w->mapToGlobal(p));
    if (!atW) return false;
    return atW->topLevelWidget() == w;
}

bool isObscured(QWidget* w)
{
    return !(checkPoint(QPoint(0, 0), w) && checkPoint(QPoint(w->width() - 1, 0), w) && checkPoint(QPoint(0, w->height() - 1), w) && checkPoint(QPoint(w->width() - 1, w->height() - 1), w) && checkPoint(QPoint(w->width() / 2, w->height() / 2), w));
}

void openDebugLogfile()
{
    fs::path pathDebug = GetDataDir() / "debug.log";

    /* Open debug.log with the associated application */
    if (fs::exists(pathDebug))
        QDesktopServices::openUrl(QUrl::fromLocalFile(boostPathToQString(pathDebug)));
}

void openConfigfile()
{
    fs::path pathConfig = GetConfigFile(gArgs.GetArg("-conf", BITCOIN_CONF_FILENAME));

    /* Open memeium.conf with the associated application */
    if (fs::exists(pathConfig))
        QDesktopServices::openUrl(QUrl::fromLocalFile(boostPathToQString(pathConfig)));
}

void showBackups()
{
    fs::path backupsDir = GetBackupsDir();

    /* Open folder with default browser */
    if (fs::exists(backupsDir))
        QDesktopServices::openUrl(QUrl::fromLocalFile(boostPathToQString(backupsDir)));
}

void SubstituteFonts(const QString& language)
{
#if defined(Q_OS_MAC)
// Background:
// OSX's default font changed in 10.9 and Qt is unable to find it with its
// usual fallback methods when building against the 10.7 sdk or lower.
// The 10.8 SDK added a function to let it find the correct fallback font.
// If this fallback is not properly loaded, some characters may fail to
// render correctly.
//
// The same thing happened with 10.10. .Helvetica Neue DeskInterface is now default.
//
// Solution: If building with the 10.7 SDK or lower and the user's platform
// is 10.9 or higher at runtime, substitute the correct font. This needs to
// happen before the QApplication is created.
#if defined(MAC_OS_X_VERSION_MAX_ALLOWED) && MAC_OS_X_VERSION_MAX_ALLOWED < MAC_OS_X_VERSION_10_8
    if (floor(NSAppKitVersionNumber) > NSAppKitVersionNumber10_8) {
        if (floor(NSAppKitVersionNumber) <= NSAppKitVersionNumber10_9)
            /* On a 10.9 - 10.9.x system */
            QFont::insertSubstitution(".Lucida Grande UI", "Lucida Grande");
        else {
            /* 10.10 or later system */
            if (language == "zh_CN" || language == "zh_TW" || language == "zh_HK") // traditional or simplified Chinese
                QFont::insertSubstitution(".Helvetica Neue DeskInterface", "Heiti SC");
            else if (language == "ja") // Japanese
                QFont::insertSubstitution(".Helvetica Neue DeskInterface", "Songti SC");
            else
                QFont::insertSubstitution(".Helvetica Neue DeskInterface", "Lucida Grande");
        }
    }
#endif
#endif
}

ToolTipToRichTextFilter::ToolTipToRichTextFilter(int _size_threshold, QObject* parent) :
    QObject(parent),
    size_threshold(_size_threshold)
{
}

bool ToolTipToRichTextFilter::eventFilter(QObject* obj, QEvent* evt)
{
    if (evt->type() == QEvent::ToolTipChange) {
        QWidget* widget = static_cast<QWidget*>(obj);
        QString tooltip = widget->toolTip();
        if (tooltip.size() > size_threshold && !tooltip.startsWith("<qt")) {
            // Escape the current message as HTML and replace \n by <br> if it's not rich text
            if (!Qt::mightBeRichText(tooltip))
                tooltip = HtmlEscape(tooltip, true);
            // Envelop with <qt></qt> to make sure Qt detects every tooltip as rich text
            // and style='white-space:pre' to preserve line composition
            tooltip = "<qt style='white-space:pre'>" + tooltip + "</qt>";
            widget->setToolTip(tooltip);
            return true;
        }
    }
    return QObject::eventFilter(obj, evt);
}

void TableViewLastColumnResizingFixer::connectViewHeadersSignals()
{
    connect(tableView->horizontalHeader(), SIGNAL(sectionResized(int, int, int)), this, SLOT(on_sectionResized(int, int, int)));
    connect(tableView->horizontalHeader(), SIGNAL(geometriesChanged()), this, SLOT(on_geometriesChanged()));
}

// We need to disconnect these while handling the resize events, otherwise we can enter infinite loops.
void TableViewLastColumnResizingFixer::disconnectViewHeadersSignals()
{
    disconnect(tableView->horizontalHeader(), SIGNAL(sectionResized(int, int, int)), this, SLOT(on_sectionResized(int, int, int)));
    disconnect(tableView->horizontalHeader(), SIGNAL(geometriesChanged()), this, SLOT(on_geometriesChanged()));
}

// Setup the resize mode, handles compatibility for Qt5 and below as the method signatures changed.
// Refactored here for readability.
void TableViewLastColumnResizingFixer::setViewHeaderResizeMode(int logicalIndex, QHeaderView::ResizeMode resizeMode)
{
#if QT_VERSION < 0x050000
    tableView->horizontalHeader()->setResizeMode(logicalIndex, resizeMode);
#else
    tableView->horizontalHeader()->setSectionResizeMode(logicalIndex, resizeMode);
#endif
}

void TableViewLastColumnResizingFixer::resizeColumn(int nColumnIndex, int width)
{
    tableView->setColumnWidth(nColumnIndex, width);
    tableView->horizontalHeader()->resizeSection(nColumnIndex, width);
}

int TableViewLastColumnResizingFixer::getColumnsWidth()
{
    int nColumnsWidthSum = 0;
    for (int i = 0; i < columnCount; i++) {
        nColumnsWidthSum += tableView->horizontalHeader()->sectionSize(i);
    }
    return nColumnsWidthSum;
}

int TableViewLastColumnResizingFixer::getAvailableWidthForColumn(int column)
{
    int nResult = lastColumnMinimumWidth;
    int nTableWidth = tableView->horizontalHeader()->width();

    if (nTableWidth > 0) {
        int nOtherColsWidth = getColumnsWidth() - tableView->horizontalHeader()->sectionSize(column);
        nResult = std::max(nResult, nTableWidth - nOtherColsWidth);
    }

    return nResult;
}

// Make sure we don't make the columns wider than the table's viewport width.
void TableViewLastColumnResizingFixer::adjustTableColumnsWidth()
{
    disconnectViewHeadersSignals();
    resizeColumn(lastColumnIndex, getAvailableWidthForColumn(lastColumnIndex));
    connectViewHeadersSignals();

    int nTableWidth = tableView->horizontalHeader()->width();
    int nColsWidth = getColumnsWidth();
    if (nColsWidth > nTableWidth) {
        resizeColumn(secondToLastColumnIndex, getAvailableWidthForColumn(secondToLastColumnIndex));
    }
}

// Make column use all the space available, useful during window resizing.
void TableViewLastColumnResizingFixer::stretchColumnWidth(int column)
{
    disconnectViewHeadersSignals();
    resizeColumn(column, getAvailableWidthForColumn(column));
    connectViewHeadersSignals();
}

// When a section is resized this is a slot-proxy for ajustAmountColumnWidth().
void TableViewLastColumnResizingFixer::on_sectionResized(int logicalIndex, int oldSize, int newSize)
{
    adjustTableColumnsWidth();
    int remainingWidth = getAvailableWidthForColumn(logicalIndex);
    if (newSize > remainingWidth) {
        resizeColumn(logicalIndex, remainingWidth);
    }
}

// When the table's geometry is ready, we manually perform the stretch of the "Message" column,
// as the "Stretch" resize mode does not allow for interactive resizing.
void TableViewLastColumnResizingFixer::on_geometriesChanged()
{
    if ((getColumnsWidth() - this->tableView->horizontalHeader()->width()) != 0) {
        disconnectViewHeadersSignals();
        resizeColumn(secondToLastColumnIndex, getAvailableWidthForColumn(secondToLastColumnIndex));
        connectViewHeadersSignals();
    }
}

/**
 * Initializes all internal variables and prepares the
 * the resize modes of the last 2 columns of the table and
 */
TableViewLastColumnResizingFixer::TableViewLastColumnResizingFixer(QTableView* table, int lastColMinimumWidth, int allColsMinimumWidth, QObject* parent) :
    QObject(parent),
    tableView(table),
    lastColumnMinimumWidth(lastColMinimumWidth),
    allColumnsMinimumWidth(allColsMinimumWidth)
{
    columnCount = tableView->horizontalHeader()->count();
    lastColumnIndex = columnCount - 1;
    secondToLastColumnIndex = columnCount - 2;
    tableView->horizontalHeader()->setMinimumSectionSize(allColumnsMinimumWidth);
    setViewHeaderResizeMode(secondToLastColumnIndex, QHeaderView::Interactive);
    setViewHeaderResizeMode(lastColumnIndex, QHeaderView::Interactive);
}

#ifdef WIN32
fs::path static StartupShortcutPath()
{
    std::string chain = ChainNameFromCommandLine();
    if (chain == CBaseChainParams::MAIN)
        return GetSpecialFolderPath(CSIDL_STARTUP) / "Memeium Core.lnk";
    if (chain == CBaseChainParams::TESTNET) // Remove this special case when CBaseChainParams::TESTNET = "testnet4"
        return GetSpecialFolderPath(CSIDL_STARTUP) / "Memeium Core (testnet).lnk";
    return GetSpecialFolderPath(CSIDL_STARTUP) / strprintf("Memeium Core (%s).lnk", chain);
}

bool GetStartOnSystemStartup()
{
    // check for "Memeium Core*.lnk"
    return fs::exists(StartupShortcutPath());
}

bool SetStartOnSystemStartup(bool fAutoStart)
{
    // If the shortcut exists already, remove it for updating
    fs::remove(StartupShortcutPath());

    if (fAutoStart) {
        CoInitialize(nullptr);

        // Get a pointer to the IShellLink interface.
        IShellLink* psl = nullptr;
        HRESULT hres = CoCreateInstance(CLSID_ShellLink, nullptr,
            CLSCTX_INPROC_SERVER, IID_IShellLink,
            reinterpret_cast<void**>(&psl));

        if (SUCCEEDED(hres)) {
            // Get the current executable path
            TCHAR pszExePath[MAX_PATH];
            GetModuleFileName(nullptr, pszExePath, sizeof(pszExePath));

            // Start client minimized
            QString strArgs = "-min";
            // Set -testnet /-regtest options
            strArgs += QString::fromStdString(strprintf(" -testnet=%d -regtest=%d", gArgs.GetBoolArg("-testnet", false), gArgs.GetBoolArg("-regtest", false)));

#ifdef UNICODE
            boost::scoped_array<TCHAR> args(new TCHAR[strArgs.length() + 1]);
            // Convert the QString to TCHAR*
            strArgs.toWCharArray(args.get());
            // Add missing '\0'-termination to string
            args[strArgs.length()] = '\0';
#endif

            // Set the path to the shortcut target
            psl->SetPath(pszExePath);
            PathRemoveFileSpec(pszExePath);
            psl->SetWorkingDirectory(pszExePath);
            psl->SetShowCmd(SW_SHOWMINNOACTIVE);
#ifndef UNICODE
            psl->SetArguments(strArgs.toStdString().c_str());
#else
            psl->SetArguments(args.get());
#endif

            // Query IShellLink for the IPersistFile interface for
            // saving the shortcut in persistent storage.
            IPersistFile* ppf = nullptr;
            hres = psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&ppf));
            if (SUCCEEDED(hres)) {
                WCHAR pwsz[MAX_PATH];
                // Ensure that the string is ANSI.
                MultiByteToWideChar(CP_ACP, 0, StartupShortcutPath().string().c_str(), -1, pwsz, MAX_PATH);
                // Save the link by calling IPersistFile::Save.
                hres = ppf->Save(pwsz, TRUE);
                ppf->Release();
                psl->Release();
                CoUninitialize();
                return true;
            }
            psl->Release();
        }
        CoUninitialize();
        return false;
    }
    return true;
}
#elif defined(Q_OS_LINUX)

// Follow the Desktop Application Autostart Spec:
// http://standards.freedesktop.org/autostart-spec/autostart-spec-latest.html

fs::path static GetAutostartDir()
{
    char* pszConfigHome = getenv("XDG_CONFIG_HOME");
    if (pszConfigHome) return fs::path(pszConfigHome) / "autostart";
    char* pszHome = getenv("HOME");
    if (pszHome) return fs::path(pszHome) / ".config" / "autostart";
    return fs::path();
}

fs::path static GetAutostartFilePath()
{
    std::string chain = ChainNameFromCommandLine();
    if (chain == CBaseChainParams::MAIN)
        return GetAutostartDir() / "memeiumcore.desktop";
    return GetAutostartDir() / strprintf("memeiumcore-%s.lnk", chain);
}

bool GetStartOnSystemStartup()
{
    fs::ifstream optionFile(GetAutostartFilePath());
    if (!optionFile.good())
        return false;
    // Scan through file for "Hidden=true":
    std::string line;
    while (!optionFile.eof()) {
        getline(optionFile, line);
        if (line.find("Hidden") != std::string::npos &&
            line.find("true") != std::string::npos)
            return false;
    }
    optionFile.close();

    return true;
}

bool SetStartOnSystemStartup(bool fAutoStart)
{
    if (!fAutoStart)
        fs::remove(GetAutostartFilePath());
    else {
        char pszExePath[MAX_PATH + 1];
        memset(pszExePath, 0, sizeof(pszExePath));
        if (readlink("/proc/self/exe", pszExePath, sizeof(pszExePath) - 1) == -1)
            return false;

        fs::create_directories(GetAutostartDir());

        fs::ofstream optionFile(GetAutostartFilePath(), std::ios_base::out | std::ios_base::trunc);
        if (!optionFile.good())
            return false;
        std::string chain = ChainNameFromCommandLine();
        // Write a memeiumcore.desktop file to the autostart directory:
        optionFile << "[Desktop Entry]\n";
        optionFile << "Type=Application\n";
        if (chain == CBaseChainParams::MAIN)
            optionFile << "Name=Memeium Core\n";
        else
            optionFile << strprintf("Name=Memeium Core (%s)\n", chain);
        optionFile << "Exec=" << pszExePath << strprintf(" -min -testnet=%d -regtest=%d\n", gArgs.GetBoolArg("-testnet", false), gArgs.GetBoolArg("-regtest", false));
        optionFile << "Terminal=false\n";
        optionFile << "Hidden=false\n";
        optionFile.close();
    }
    return true;
}


#elif defined(Q_OS_MAC)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
// based on: https://github.com/Mozketo/LaunchAtLoginController/blob/master/LaunchAtLoginController.m

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>

LSSharedFileListItemRef findStartupItemInList(LSSharedFileListRef list, CFURLRef findUrl);
LSSharedFileListItemRef findStartupItemInList(LSSharedFileListRef list, CFURLRef findUrl)
{
    // loop through the list of startup items and try to find the Memeium Core app
    CFArrayRef listSnapshot = LSSharedFileListCopySnapshot(list, nullptr);
    for (int i = 0; i < CFArrayGetCount(listSnapshot); i++) {
        LSSharedFileListItemRef item = (LSSharedFileListItemRef)CFArrayGetValueAtIndex(listSnapshot, i);
        UInt32 resolutionFlags = kLSSharedFileListNoUserInteraction | kLSSharedFileListDoNotMountVolumes;
        CFURLRef currentItemURL = nullptr;

#if defined(MAC_OS_X_VERSION_MAX_ALLOWED) && MAC_OS_X_VERSION_MAX_ALLOWED >= 10100
        if (&LSSharedFileListItemCopyResolvedURL)
            currentItemURL = LSSharedFileListItemCopyResolvedURL(item, resolutionFlags, nullptr);
#if defined(MAC_OS_X_VERSION_MIN_REQUIRED) && MAC_OS_X_VERSION_MIN_REQUIRED < 10100
        else
            LSSharedFileListItemResolve(item, resolutionFlags, &currentItemURL, nullptr);
#endif
#else
        LSSharedFileListItemResolve(item, resolutionFlags, &currentItemURL, nullptr);
#endif

        if (currentItemURL && CFEqual(currentItemURL, findUrl)) {
            // found
            CFRelease(currentItemURL);
            return item;
        }
        if (currentItemURL) {
            CFRelease(currentItemURL);
        }
    }
    return nullptr;
}

bool GetStartOnSystemStartup()
{
    CFURLRef bitcoinAppUrl = CFBundleCopyBundleURL(CFBundleGetMainBundle());
    LSSharedFileListRef loginItems = LSSharedFileListCreate(nullptr, kLSSharedFileListSessionLoginItems, nullptr);
    LSSharedFileListItemRef foundItem = findStartupItemInList(loginItems, bitcoinAppUrl);
    return !!foundItem; // return boolified object
}

bool SetStartOnSystemStartup(bool fAutoStart)
{
    CFURLRef bitcoinAppUrl = CFBundleCopyBundleURL(CFBundleGetMainBundle());
    LSSharedFileListRef loginItems = LSSharedFileListCreate(nullptr, kLSSharedFileListSessionLoginItems, nullptr);
    LSSharedFileListItemRef foundItem = findStartupItemInList(loginItems, bitcoinAppUrl);

    if (fAutoStart && !foundItem) {
        // add Memeium Core app to startup item list
        LSSharedFileListInsertItemURL(loginItems, kLSSharedFileListItemBeforeFirst, nullptr, nullptr, bitcoinAppUrl, nullptr, nullptr);
    } else if (!fAutoStart && foundItem) {
        // remove item
        LSSharedFileListItemRemove(loginItems, foundItem);
    }
    return true;
}
#pragma GCC diagnostic pop
#else

bool GetStartOnSystemStartup() { return false; }
bool SetStartOnSystemStartup(bool fAutoStart) { return false; }

#endif

void migrateQtSettings()
{
    // Migration (12.1)
    QSettings settings;
    if (!settings.value("fMigrationDone121", false).toBool()) {
        settings.remove("theme");
        settings.remove("nWindowPos");
        settings.remove("nWindowSize");
        settings.setValue("fMigrationDone121", true);
    }
}

// Open CSS when configured
QString loadStyleSheet()
{
    QSettings settings;
    QString theme = settings.value("theme", "").toString();

    QDir themes(":themes");
    // Make sure settings are pointing to an existent theme
    if (theme.isEmpty() || !themes.exists(theme)) {
        theme = defaultTheme;
        settings.setValue("theme", theme);
    }

    QFile qFile(":themes/" + theme);
    if (qFile.open(QFile::ReadOnly)) {
        return QLatin1String(qFile.readAll());
    }

    return QString();
}

void setClipboard(const QString& str)
{
    QApplication::clipboard()->setText(str, QClipboard::Clipboard);
    QApplication::clipboard()->setText(str, QClipboard::Selection);
}

fs::path qstringToBoostPath(const QString& path)
{
    return fs::path(path.toStdString(), utf8);
}

QString boostPathToQString(const fs::path& path)
{
    return QString::fromStdString(path.string(utf8));
}

QString formatDurationStr(int secs)
{
    QStringList strList;
    int days = secs / 86400;
    int hours = (secs % 86400) / 3600;
    int mins = (secs % 3600) / 60;
    int seconds = secs % 60;

    if (days)
        strList.append(QString(QObject::tr("%1 d")).arg(days));
    if (hours)
        strList.append(QString(QObject::tr("%1 h")).arg(hours));
    if (mins)
        strList.append(QString(QObject::tr("%1 m")).arg(mins));
    if (seconds || (!days && !hours && !mins))
        strList.append(QString(QObject::tr("%1 s")).arg(seconds));

    return strList.join(" ");
}

QString formatServicesStr(quint64 mask)
{
    QStringList strList;

    // Just scan the last 8 bits for now.
    for (int i = 0; i < 8; i++) {
        uint64_t check = 1 << i;
        if (mask & check) {
            switch (check) {
            case NODE_NETWORK:
                strList.append("NETWORK");
                break;
            case NODE_GETUTXO:
                strList.append("GETUTXO");
                break;
            case NODE_BLOOM:
                strList.append("BLOOM");
                break;
            case NODE_XTHIN:
                strList.append("XTHIN");
                break;
            default:
                strList.append(QString("%1[%2]").arg("UNKNOWN").arg(check));
            }
        }
    }

    if (strList.size())
        return strList.join(" & ");
    else
        return QObject::tr("None");
}

QString formatPingTime(double dPingTime)
{
    return (dPingTime == std::numeric_limits<int64_t>::max() / 1e6 || dPingTime == 0) ? QObject::tr("N/A") : QString(QObject::tr("%1 ms")).arg(QString::number((int)(dPingTime * 1000), 10));
}

QString formatTimeOffset(int64_t nTimeOffset)
{
    return QString(QObject::tr("%1 s")).arg(QString::number((int)nTimeOffset, 10));
}

QString formatNiceTimeOffset(qint64 secs)
{
    // Represent time from last generated block in human readable text
    QString timeBehindText;
    const int HOUR_IN_SECONDS = 60 * 60;
    const int DAY_IN_SECONDS = 24 * 60 * 60;
    const int WEEK_IN_SECONDS = 7 * 24 * 60 * 60;
    const int YEAR_IN_SECONDS = 31556952; // Average length of year in Gregorian calendar
    if (secs < 60) {
        timeBehindText = QObject::tr("%n second(s)", "", secs);
    } else if (secs < 2 * HOUR_IN_SECONDS) {
        timeBehindText = QObject::tr("%n minute(s)", "", secs / 60);
    } else if (secs < 2 * DAY_IN_SECONDS) {
        timeBehindText = QObject::tr("%n hour(s)", "", secs / HOUR_IN_SECONDS);
    } else if (secs < 2 * WEEK_IN_SECONDS) {
        timeBehindText = QObject::tr("%n day(s)", "", secs / DAY_IN_SECONDS);
    } else if (secs < YEAR_IN_SECONDS) {
        timeBehindText = QObject::tr("%n week(s)", "", secs / WEEK_IN_SECONDS);
    } else {
        qint64 years = secs / YEAR_IN_SECONDS;
        qint64 remainder = secs % YEAR_IN_SECONDS;
        timeBehindText = QObject::tr("%1 and %2").arg(QObject::tr("%n year(s)", "", years)).arg(QObject::tr("%n week(s)", "", remainder / WEEK_IN_SECONDS));
    }
    return timeBehindText;
}

void ClickableLabel::mouseReleaseEvent(QMouseEvent* event)
{
    Q_EMIT clicked(event->pos());
}

void ClickableProgressBar::mouseReleaseEvent(QMouseEvent* event)
{
    Q_EMIT clicked(event->pos());
}

void concatenate(QPainter* painter, QString& catString, int static_width, int left_side, int right_size)
{
    // Starting length of the name
    int start_name_length = catString.size();

    // Get the length of the dots
    // #ifndef QTversionPreFiveEleven
    //	int dots_width = painter->fontMetrics().horizontalAdvance("...");
    // #else
    int dots_width = painter->fontMetrics().width("...");
    // #endif

    // Add the dots width to the amount width
    static_width += dots_width;

    // Start concatenation loop, end loop if name is at three characters
    while (catString.size() > 3) {
        // Get the text width of the current name
        // #ifndef QTversionPreFiveEleven
        //	int text_width = painter->fontMetrics().horizontalAdvance(catString);
        // #else
        int text_width = painter->fontMetrics().width(catString);
        // #endif
        // Check to see if the text width is going to overlap the amount width if it doesn't break the loop
        if (left_side + text_width < right_size - static_width)
            break;

        // substring the name minus the last character of it and continue the loop
        catString = catString.left(catString.size() - 1);
    }

    // Add the ... if the name was concatenated
    if (catString.size() != start_name_length)
        catString.append("...");
}

} // namespace GUIUtil
