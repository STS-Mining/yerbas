// Copyright (c) 2011-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "transactionview.h"

#include "addresstablemodel.h"
#include "bitcoinunits.h"
#include "csvmodelwriter.h"
#include "editaddressdialog.h"
#include "guiutil.h"
#include "optionsmodel.h"
#include "platformstyle.h"
#include "qrdialog.h"
#include "transactiondescdialog.h"
#include "transactionfilterproxy.h"
#include "transactionrecord.h"
#include "transactiontablemodel.h"
#include "walletmodel.h"
#include "validation.h"

#include "ui_interface.h"

#include <QComboBox>
#include <QDateTimeEdit>
#include <QDesktopServices>
#include <QDoubleValidator>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPoint>
#include <QScrollBar>
#include <QSettings>
#include <QSignalMapper>
#include <QTableView>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

/** Date format for persistence */
static const char* PERSISTENCE_DATE_FORMAT = "yyyy-MM-dd";

TransactionView::TransactionView(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent), model(0), transactionProxyModel(0),
    transactionView(0), abandonAction(0), columnResizingFixer(0)
{
    QSettings settings;
    // Build filter row
    setContentsMargins(0,0,0,0);

    QHBoxLayout *hlayout = new QHBoxLayout();
    hlayout->setContentsMargins(0,0,0,0);
    if (platformStyle->getUseExtraSpacing()) {
        hlayout->setSpacing(0);
        hlayout->addSpacing(STATUS_COLUMN_WIDTH - 1);
    } else {
        hlayout->setSpacing(1);
        hlayout->addSpacing(STATUS_COLUMN_WIDTH - 2);
    }
    watchOnlyWidget = new QComboBox(this);
    watchOnlyWidget->setFixedWidth(24);
    watchOnlyWidget->addItem("", TransactionFilterProxy::WatchOnlyFilter_All);
    watchOnlyWidget->addItem(QIcon(":/icons/eye_plus"), "", TransactionFilterProxy::WatchOnlyFilter_Yes);
    watchOnlyWidget->addItem(QIcon(":/icons/eye_minus"), "", TransactionFilterProxy::WatchOnlyFilter_No);
    hlayout->addWidget(watchOnlyWidget);

    instantsendWidget = new QComboBox(this);
    instantsendWidget->setFixedWidth(24);
    instantsendWidget->addItem(tr("All"), TransactionFilterProxy::InstantSendFilter_All);
    instantsendWidget->addItem(tr("Locked by InstantSend"), TransactionFilterProxy::InstantSendFilter_Yes);
    instantsendWidget->addItem(tr("Not locked by InstantSend"), TransactionFilterProxy::InstantSendFilter_No);
    hlayout->addWidget(instantsendWidget);

    dateWidget = new QComboBox(this);
    if (platformStyle->getUseExtraSpacing()) {
        dateWidget->setFixedWidth(120);
    } else {
        dateWidget->setFixedWidth(120);
    }
    dateWidget->addItem(tr("All"), All);
    dateWidget->addItem(tr("Today"), Today);
    dateWidget->addItem(tr("This week"), ThisWeek);
    dateWidget->addItem(tr("This month"), ThisMonth);
    dateWidget->addItem(tr("Last month"), LastMonth);
    dateWidget->addItem(tr("This year"), ThisYear);
    dateWidget->addItem(tr("Range..."), Range);
    dateWidget->setCurrentIndex(settings.value("transactionDate").toInt());
    hlayout->addWidget(dateWidget);

    typeWidget = new QComboBox(this);
    if (platformStyle->getUseExtraSpacing()) {
        typeWidget->setFixedWidth(TYPE_COLUMN_WIDTH);
    } else {
        typeWidget->setFixedWidth(TYPE_COLUMN_WIDTH-1);
    }

    typeWidget->addItem(tr("All"), TransactionFilterProxy::ALL_TYPES);
    typeWidget->addItem(tr("Most Common"), TransactionFilterProxy::COMMON_TYPES);
    typeWidget->addItem(tr("Received with"), TransactionFilterProxy::TYPE(TransactionRecord::RecvWithAddress) |
                                        TransactionFilterProxy::TYPE(TransactionRecord::RecvFromOther));
    typeWidget->addItem(tr("Sent to"), TransactionFilterProxy::TYPE(TransactionRecord::SendToAddress) |
                                  TransactionFilterProxy::TYPE(TransactionRecord::SendToOther));
    typeWidget->addItem(tr("PrivateSend"), TransactionFilterProxy::TYPE(TransactionRecord::PrivateSend));
    typeWidget->addItem(tr("PrivateSend Make Collateral Inputs"), TransactionFilterProxy::TYPE(TransactionRecord::PrivateSendMakeCollaterals));
    typeWidget->addItem(tr("PrivateSend Create Denominations"), TransactionFilterProxy::TYPE(TransactionRecord::PrivateSendCreateDenominations));
    typeWidget->addItem(tr("PrivateSend Denominate"), TransactionFilterProxy::TYPE(TransactionRecord::PrivateSendDenominate));
    typeWidget->addItem(tr("PrivateSend Collateral Payment"), TransactionFilterProxy::TYPE(TransactionRecord::PrivateSendCollateralPayment));
    typeWidget->addItem(tr("To yourself"), TransactionFilterProxy::TYPE(TransactionRecord::SendToSelf));
    typeWidget->addItem(tr("Mined"), TransactionFilterProxy::TYPE(TransactionRecord::Generated));
    typeWidget->addItem(tr("Other"), TransactionFilterProxy::TYPE(TransactionRecord::Other));
    typeWidget->setCurrentIndex(settings.value("transactionType").toInt());

    hlayout->addWidget(typeWidget);

    addressWidget = new QLineEdit(this);
#if QT_VERSION >= 0x040700
    addressWidget->setPlaceholderText(tr("Enter address or label to search"));
#endif
    addressWidget->setObjectName("addressWidget");
    hlayout->addWidget(addressWidget);

    amountWidget = new QLineEdit(this);
#if QT_VERSION >= 0x040700
    amountWidget->setPlaceholderText(tr("Min amount"));
#endif
    if (platformStyle->getUseExtraSpacing()) {
        amountWidget->setFixedWidth(118);
    } else {
        amountWidget->setFixedWidth(125);
    }  
    amountWidget->setValidator(new QDoubleValidator(0, 1e20, 8, this));
    amountWidget->setObjectName("amountWidget");
    hlayout->addWidget(amountWidget);

    assetNameWidget = new QLineEdit(this);
#if QT_VERSION >= 0x040700
    assetNameWidget->setPlaceholderText(tr("Asset name"));
#endif
    assetNameWidget->setFixedWidth(200);
    hlayout->addWidget(assetNameWidget);
    assetNameWidget->hide();

    // Delay before filtering transactions in ms
    static const int input_filter_delay = 200;

    QTimer* amount_typing_delay = new QTimer(this);
    amount_typing_delay->setSingleShot(true);
    amount_typing_delay->setInterval(input_filter_delay);

    QTimer* prefix_typing_delay = new QTimer(this);
    prefix_typing_delay->setSingleShot(true);
    prefix_typing_delay->setInterval(input_filter_delay);

    QTimer* asset_typing_delay = new QTimer(this);
    asset_typing_delay->setSingleShot(true);
    asset_typing_delay->setInterval(input_filter_delay);

    QVBoxLayout *vlayout = new QVBoxLayout(this);
    vlayout->setContentsMargins(0,0,0,0);
    vlayout->setSpacing(0);

    QTableView *view = new QTableView(this);
    vlayout->addLayout(hlayout);
    vlayout->addWidget(createDateRangeWidget());
    vlayout->addWidget(view);
    vlayout->setSpacing(0);
#ifndef Q_OS_MAC
    int width = view->verticalScrollBar()->sizeHint().width();
    // Cover scroll bar width with spacing
    if (platformStyle->getUseExtraSpacing()) {
        hlayout->addSpacing(width+2);
    } else {
        hlayout->addSpacing(width);
    }
    // Always show scroll bar
    view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
#endif
    view->setTabKeyNavigation(false);
    view->setContextMenuPolicy(Qt::CustomContextMenu);

    view->installEventFilter(this);

    transactionView = view;

    // Actions
    abandonAction = new QAction(tr("Abandon transaction"), this);
    QAction *copyAddressAction = new QAction(tr("Copy address"), this);
    QAction *copyLabelAction = new QAction(tr("Copy label"), this);
    QAction *copyAmountAction = new QAction(tr("Copy amount"), this);
    QAction *copyTxIDAction = new QAction(tr("Copy transaction ID"), this);
    QAction *copyTxHexAction = new QAction(tr("Copy raw transaction"), this);
    QAction *copyTxPlainText = new QAction(tr("Copy full transaction details"), this);
    QAction *editLabelAction = new QAction(tr("Edit label"), this);
    QAction *showDetailsAction = new QAction(tr("Show transaction details"), this);
    QAction *showAddressQRCodeAction = new QAction(tr("Show address QR code"), this);

    contextMenu = new QMenu(this);
    contextMenu->addAction(copyAddressAction);
    contextMenu->addAction(copyLabelAction);
    contextMenu->addAction(copyAmountAction);
    contextMenu->addAction(copyTxIDAction);
    contextMenu->addAction(copyTxHexAction);
    contextMenu->addAction(copyTxPlainText);
    contextMenu->addAction(showDetailsAction);
    contextMenu->addAction(showAddressQRCodeAction);
    contextMenu->addSeparator();
    contextMenu->addAction(abandonAction);
    contextMenu->addAction(editLabelAction);

    mapperThirdPartyTxUrls = new QSignalMapper(this);

    // Connect actions
    connect(mapperThirdPartyTxUrls, SIGNAL(mapped(QString)), this, SLOT(openThirdPartyTxUrl(QString)));

    connect(dateWidget, SIGNAL(activated(int)), this, SLOT(chooseDate(int)));
    connect(typeWidget, SIGNAL(activated(int)), this, SLOT(chooseType(int)));
    connect(watchOnlyWidget, SIGNAL(activated(int)), this, SLOT(chooseWatchonly(int)));
    connect(instantsendWidget, SIGNAL(activated(int)), this, SLOT(chooseInstantSend(int)));
    connect(amountWidget, SIGNAL(textChanged(QString)), amount_typing_delay, SLOT(start()));
    connect(assetNameWidget, SIGNAL(textChanged(QString)), asset_typing_delay, SLOT(start()));
    connect(asset_typing_delay, SIGNAL(timeout()), this, SLOT(changedAssetName()));
    connect(amount_typing_delay, SIGNAL(timeout()), this, SLOT(changedAmount()));
    connect(addressWidget, SIGNAL(textChanged(QString)), prefix_typing_delay, SLOT(start()));
    connect(prefix_typing_delay, SIGNAL(timeout()), this, SLOT(changedPrefix()));

    connect(view, SIGNAL(doubleClicked(QModelIndex)), this, SIGNAL(doubleClicked(QModelIndex)));
    connect(view, SIGNAL(clicked(QModelIndex)), this, SLOT(computeSum()));
    connect(view, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(contextualMenu(QPoint)));

    connect(abandonAction, SIGNAL(triggered()), this, SLOT(abandonTx()));
    connect(copyAddressAction, SIGNAL(triggered()), this, SLOT(copyAddress()));
    connect(copyLabelAction, SIGNAL(triggered()), this, SLOT(copyLabel()));
    connect(copyAmountAction, SIGNAL(triggered()), this, SLOT(copyAmount()));
    connect(copyTxIDAction, SIGNAL(triggered()), this, SLOT(copyTxID()));
    connect(copyTxHexAction, SIGNAL(triggered()), this, SLOT(copyTxHex()));
    connect(copyTxPlainText, SIGNAL(triggered()), this, SLOT(copyTxPlainText()));
    connect(editLabelAction, SIGNAL(triggered()), this, SLOT(editLabel()));
    connect(showDetailsAction, SIGNAL(triggered()), this, SLOT(showDetails()));
    connect(showAddressQRCodeAction, SIGNAL(triggered()), this, SLOT(showAddressQRCode()));
     
    // Trigger the call to show the assets table if assets are active
    showingAssets = false;
    showAssets();
}

void TransactionView::setModel(WalletModel *_model)
{
    QSettings settings;
    this->model = _model;
    if(_model)
    {
        transactionProxyModel = new TransactionFilterProxy(this);
        transactionProxyModel->setSourceModel(_model->getTransactionTableModel());
        transactionProxyModel->setDynamicSortFilter(true);
        transactionProxyModel->setSortCaseSensitivity(Qt::CaseInsensitive);
        transactionProxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);

        transactionProxyModel->setSortRole(Qt::EditRole);

        transactionView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        transactionView->setModel(transactionProxyModel);
        transactionView->setAlternatingRowColors(true);
        transactionView->setSelectionBehavior(QAbstractItemView::SelectRows);
        transactionView->setSelectionMode(QAbstractItemView::ExtendedSelection);
        transactionView->setSortingEnabled(true);
        transactionView->sortByColumn(TransactionTableModel::Date, Qt::DescendingOrder);
        transactionView->verticalHeader()->hide();

        transactionView->setColumnWidth(TransactionTableModel::Status, STATUS_COLUMN_WIDTH);
        transactionView->setColumnWidth(TransactionTableModel::Watchonly, WATCHONLY_COLUMN_WIDTH);
        transactionView->setColumnWidth(TransactionTableModel::InstantSend, INSTANTSEND_COLUMN_WIDTH);
        transactionView->setColumnWidth(TransactionTableModel::Date, DATE_COLUMN_WIDTH);
        transactionView->setColumnWidth(TransactionTableModel::Type, TYPE_COLUMN_WIDTH);
        transactionView->setColumnWidth(TransactionTableModel::Amount, AMOUNT_MINIMUM_COLUMN_WIDTH);
        transactionView->setColumnWidth(TransactionTableModel::AssetName, TYPE_COLUMN_WIDTH);

        // Note: it's a good idea to connect this signal AFTER the model is set
        connect(transactionView->selectionModel(), SIGNAL(selectionChanged(QItemSelection, QItemSelection)), this, SLOT(computeSum()));

        columnResizingFixer = new GUIUtil::TableViewLastColumnResizingFixer(transactionView, AMOUNT_MINIMUM_COLUMN_WIDTH, MINIMUM_COLUMN_WIDTH, this);

        if (_model->getOptionsModel())
        {
            // Add third party transaction URLs to context menu
            QStringList listUrls = _model->getOptionsModel()->getThirdPartyTxUrls().split("|", QString::SkipEmptyParts);
            for (int i = 0; i < listUrls.size(); ++i)
            {
                QString host = QUrl(listUrls[i].trimmed(), QUrl::StrictMode).host();
                if (!host.isEmpty())
                {
                    QAction *thirdPartyTxUrlAction = new QAction(host, this); // use host as menu item label
                    if (i == 0)
                        contextMenu->addSeparator();
                    contextMenu->addAction(thirdPartyTxUrlAction);
                    connect(thirdPartyTxUrlAction, SIGNAL(triggered()), mapperThirdPartyTxUrls, SLOT(map()));
                    mapperThirdPartyTxUrls->setMapping(thirdPartyTxUrlAction, listUrls[i].trimmed());
                }
            }
        }

        // show/hide column Watch-only
        updateWatchOnlyColumn(_model->haveWatchOnly());

        // Watch-only signal
        connect(_model, SIGNAL(notifyWatchonlyChanged(bool)), this, SLOT(updateWatchOnlyColumn(bool)));
        
        // Update transaction list with persisted settings
        chooseType(settings.value("transactionType").toInt());
        chooseDate(settings.value("transactionDate").toInt());
    }
}

void TransactionView::chooseDate(int idx)
{
    if(!transactionProxyModel)
        return;
    
    QSettings settings;
    QDate current = QDate::currentDate();
    dateRangeWidget->setVisible(false);
    switch(dateWidget->itemData(idx).toInt())
    {
    case All:
        transactionProxyModel->setDateRange(
                TransactionFilterProxy::MIN_DATE,
                TransactionFilterProxy::MAX_DATE);
        break;
    case Today:
        transactionProxyModel->setDateRange(
                QDateTime(current),
                TransactionFilterProxy::MAX_DATE);
        break;
    case ThisWeek: {
        // Find last Monday
        QDate startOfWeek = current.addDays(-(current.dayOfWeek()-1));
        transactionProxyModel->setDateRange(
                QDateTime(startOfWeek),
                TransactionFilterProxy::MAX_DATE);

        } break;
    case ThisMonth:
        transactionProxyModel->setDateRange(
                QDateTime(QDate(current.year(), current.month(), 1)),
                TransactionFilterProxy::MAX_DATE);
        break;
    case LastMonth:
        transactionProxyModel->setDateRange(
                QDateTime(QDate(current.year(), current.month(), 1).addMonths(-1)),
                QDateTime(QDate(current.year(), current.month(), 1)));
        break;
    case ThisYear:
        transactionProxyModel->setDateRange(
                QDateTime(QDate(current.year(), 1, 1)),
                TransactionFilterProxy::MAX_DATE);
        break;
    case Range:
        dateRangeWidget->setVisible(true);
        dateRangeChanged();
        break;
    }
    // Persist new date settings
    settings.setValue("transactionDate", idx);
    if (dateWidget->itemData(idx).toInt() == Range){
        settings.setValue("transactionDateFrom", dateFrom->date().toString(PERSISTENCE_DATE_FORMAT));
        settings.setValue("transactionDateTo", dateTo->date().toString(PERSISTENCE_DATE_FORMAT));
    }
}

void TransactionView::chooseType(int idx)
{
    if(!transactionProxyModel)
        return;
    transactionProxyModel->setTypeFilter(
        typeWidget->itemData(idx).toInt());
    // Persist settings
    QSettings settings;
    settings.setValue("transactionType", idx);
}

void TransactionView::chooseWatchonly(int idx)
{
    if(!transactionProxyModel)
        return;
    transactionProxyModel->setWatchOnlyFilter(
        (TransactionFilterProxy::WatchOnlyFilter)watchOnlyWidget->itemData(idx).toInt());
}

void TransactionView::chooseInstantSend(int idx)
{
    if(!transactionProxyModel)
        return;
    transactionProxyModel->setInstantSendFilter(
        (TransactionFilterProxy::InstantSendFilter)instantsendWidget->itemData(idx).toInt());
}

void TransactionView::changedPrefix()
{
    if(!transactionProxyModel)
        return;
    transactionProxyModel->setAddressPrefix(addressWidget->text());
}

void TransactionView::changedAmount()
{
    if(!transactionProxyModel)
        return;
    CAmount amount_parsed = 0;

    // Replace "," by "." so BitcoinUnits::parse will not fail for users entering "," as decimal separator
    QString newAmount = amountWidget->text();
    newAmount.replace(QString(","), QString("."));

    if (BitcoinUnits::parse(model->getOptionsModel()->getDisplayUnit(), newAmount, &amount_parsed)) {
        transactionProxyModel->setMinAmount(amount_parsed);
    }
    else
    {
        transactionProxyModel->setMinAmount(0);
    }
}

void TransactionView::changedAssetName()
{
    if (!transactionProxyModel)
        return;
    transactionProxyModel->setAssetNamePrefix(assetNameWidget->text());
}

void TransactionView::exportClicked()
{
    if (!model || !model->getOptionsModel()) {
        return;
    }

    // CSV is currently the only supported format
    QString filename = GUIUtil::getSaveFileName(this,
        tr("Export Transaction History"), QString(),
        tr("Comma separated file (*.csv)"), nullptr);

    if (filename.isNull())
        return;

    CSVModelWriter writer(filename);

    // name, column, role
    writer.setModel(transactionProxyModel);
    writer.addColumn(tr("Confirmed"), 0, TransactionTableModel::ConfirmedRole);
    if (model && model->haveWatchOnly())
        writer.addColumn(tr("Watch-only"), TransactionTableModel::Watchonly);
    writer.addColumn(tr("Date"), 0, TransactionTableModel::DateRole);
    writer.addColumn(tr("Type"), TransactionTableModel::Type, Qt::EditRole);
    writer.addColumn(tr("Label"), 0, TransactionTableModel::LabelRole);
    writer.addColumn(tr("Address"), 0, TransactionTableModel::AddressRole);
    writer.addColumn(BitcoinUnits::getAmountColumnTitle(model->getOptionsModel()->getDisplayUnit()), 0, TransactionTableModel::FormattedAmountRole);
    if (AreAssetsDeployed()) {
        writer.addColumn(tr("Asset"), 0, TransactionTableModel::AssetNameRole);
    }
    writer.addColumn(tr("ID"), 0, TransactionTableModel::TxIDRole);

    if(!writer.write()) {
        Q_EMIT message(tr("Exporting Failed"), tr("There was an error trying to save the transaction history to %1.").arg(filename),
            CClientUIInterface::MSG_ERROR);
    }
    else {
        Q_EMIT message(tr("Exporting Successful"), tr("The transaction history was successfully saved to %1.").arg(filename),
            CClientUIInterface::MSG_INFORMATION);
    }
}

void TransactionView::contextualMenu(const QPoint &point)
{
    if (!transactionView || !transactionView->selectionModel())
        return;
    QModelIndex index = transactionView->indexAt(point);
    QModelIndexList selection = transactionView->selectionModel()->selectedRows(0);
    if (selection.empty())
        return;

    // check if transaction can be abandoned, disable context menu action in case it doesn't
    uint256 hash;
    hash.SetHex(selection.at(0).data(TransactionTableModel::TxHashRole).toString().toStdString());
    abandonAction->setEnabled(model->transactionCanBeAbandoned(hash));

    if(index.isValid())
    {
        contextMenu->exec(QCursor::pos());
    }
}

void TransactionView::abandonTx()
{
    if(!transactionView || !transactionView->selectionModel())
        return;
    QModelIndexList selection = transactionView->selectionModel()->selectedRows(0);

    // get the hash from the TxHashRole (QVariant / QString)
    uint256 hash;
    QString hashQStr = selection.at(0).data(TransactionTableModel::TxHashRole).toString();
    hash.SetHex(hashQStr.toStdString());

    // Abandon the wallet transaction over the walletModel
    model->abandonTransaction(hash);

    // Update the table
    model->getTransactionTableModel()->updateTransaction(hashQStr, CT_UPDATED, false);
}

void TransactionView::copyAddress()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::AddressRole);
}

void TransactionView::copyLabel()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::LabelRole);
}

void TransactionView::copyAmount()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::FormattedAmountRole);
}

void TransactionView::copyTxID()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::TxIDRole);
}

void TransactionView::copyTxHex()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::TxHexRole);
}

void TransactionView::copyTxPlainText()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::TxPlainTextRole);
}

void TransactionView::editLabel()
{
    if(!transactionView->selectionModel() ||!model)
        return;
    QModelIndexList selection = transactionView->selectionModel()->selectedRows();
    if(!selection.isEmpty())
    {
        AddressTableModel *addressBook = model->getAddressTableModel();
        if(!addressBook)
            return;
        QString address = selection.at(0).data(TransactionTableModel::AddressRole).toString();
        if(address.isEmpty())
        {
            // If this transaction has no associated address, exit
            return;
        }
        // Is address in address book? Address book can miss address when a transaction is
        // sent from outside the UI.
        int idx = addressBook->lookupAddress(address);
        if(idx != -1)
        {
            // Edit sending / receiving address
            QModelIndex modelIdx = addressBook->index(idx, 0, QModelIndex());
            // Determine type of address, launch appropriate editor dialog type
            QString type = modelIdx.data(AddressTableModel::TypeRole).toString();

            EditAddressDialog dlg(
                type == AddressTableModel::Receive
                ? EditAddressDialog::EditReceivingAddress
                : EditAddressDialog::EditSendingAddress, this);
            dlg.setModel(addressBook);
            dlg.loadRow(idx);
            dlg.exec();
        }
        else
        {
            // Add sending address
            EditAddressDialog dlg(EditAddressDialog::NewSendingAddress,
                this);
            dlg.setModel(addressBook);
            dlg.setAddress(address);
            dlg.exec();
        }
    }
}

void TransactionView::showDetails()
{
    if(!transactionView->selectionModel())
        return;
    QModelIndexList selection = transactionView->selectionModel()->selectedRows();
    if(!selection.isEmpty())
    {
        TransactionDescDialog *dlg = new TransactionDescDialog(selection.at(0));
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
    }
}

void TransactionView::showAddressQRCode()
{
    QList<QModelIndex> entries = GUIUtil::getEntryData(transactionView, 0);
    if (entries.empty()) {
        return;
    }

    QString strAddress = entries.at(0).data(TransactionTableModel::AddressRole).toString();
    QRDialog* dialog = new QRDialog(this);
    OptionsModel *model = new OptionsModel(nullptr, false);

    dialog->setModel(model);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setInfo(tr("QR code"), "memeium:"+strAddress, "", strAddress);
    dialog->show();
}

/** Compute sum of all selected transactions */
void TransactionView::computeSum()
{
    qint64 amount = 0;
    int nDisplayUnit = model->getOptionsModel()->getDisplayUnit();
    if(!transactionView->selectionModel())
        return;
    QModelIndexList selection = transactionView->selectionModel()->selectedRows();

    for (QModelIndex index : selection){
        amount += index.data(TransactionTableModel::AmountRole).toLongLong();
    }
    QString strAmount(BitcoinUnits::formatWithUnit(nDisplayUnit, amount, true, BitcoinUnits::separatorAlways));
    if (amount < 0) strAmount = "<span style='" + GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR) + "'>" + strAmount + "</span>";
    Q_EMIT trxAmount(strAmount);
}

void TransactionView::openThirdPartyTxUrl(QString url)
{
    if(!transactionView || !transactionView->selectionModel())
        return;
    QModelIndexList selection = transactionView->selectionModel()->selectedRows(0);
    if(!selection.isEmpty())
         QDesktopServices::openUrl(QUrl::fromUserInput(url.replace("%s", selection.at(0).data(TransactionTableModel::TxHashRole).toString())));
}

QWidget *TransactionView::createDateRangeWidget()
{
    // Create default dates in case nothing is persisted
    QString defaultDateFrom = QDate::currentDate().toString(PERSISTENCE_DATE_FORMAT);
    QString defaultDateTo = QDate::currentDate().addDays(1).toString(PERSISTENCE_DATE_FORMAT);
    QSettings settings;
 
    dateRangeWidget = new QFrame();
    dateRangeWidget->setFrameStyle(QFrame::Panel | QFrame::Raised);
    dateRangeWidget->setContentsMargins(1,1,1,1);
    QHBoxLayout *layout = new QHBoxLayout(dateRangeWidget);
    layout->setContentsMargins(0,0,0,0);
    layout->addSpacing(23);
    layout->addWidget(new QLabel(tr("Range:")));

    dateFrom = new QDateTimeEdit(this);
    dateFrom->setCalendarPopup(true);
    dateFrom->setMinimumWidth(100);
    // Load persisted FROM date
    dateFrom->setDate(QDate::fromString(settings.value("transactionDateFrom", defaultDateFrom).toString(), PERSISTENCE_DATE_FORMAT));

    layout->addWidget(dateFrom);
    layout->addWidget(new QLabel(tr("to")));

    dateTo = new QDateTimeEdit(this);
    dateTo->setCalendarPopup(true);
    dateTo->setMinimumWidth(100);
    // Load persisted TO date
    dateTo->setDate(QDate::fromString(settings.value("transactionDateTo", defaultDateTo).toString(), PERSISTENCE_DATE_FORMAT));

    layout->addWidget(dateTo);
    layout->addStretch();

    // Hide by default
    dateRangeWidget->setVisible(false);

    // Notify on change
    connect(dateFrom, SIGNAL(dateChanged(QDate)), this, SLOT(dateRangeChanged()));
    connect(dateTo, SIGNAL(dateChanged(QDate)), this, SLOT(dateRangeChanged()));

    return dateRangeWidget;
}

void TransactionView::dateRangeChanged()
{
    if(!transactionProxyModel)
        return;
    
    // Persist new date range
    QSettings settings;
    settings.setValue("transactionDateFrom", dateFrom->date().toString(PERSISTENCE_DATE_FORMAT));
    settings.setValue("transactionDateTo", dateTo->date().toString(PERSISTENCE_DATE_FORMAT));
    
    transactionProxyModel->setDateRange(
            QDateTime(dateFrom->date()),
            QDateTime(dateTo->date()));
}

void TransactionView::focusTransaction(const QModelIndex &idx)
{
    if(!transactionProxyModel)
        return;
    QModelIndex targetIdx = transactionProxyModel->mapFromSource(idx);
    transactionView->selectRow(targetIdx.row());
    computeSum();
    transactionView->scrollTo(targetIdx);
    transactionView->setCurrentIndex(targetIdx);
    transactionView->setFocus();
}

// We override the virtual resizeEvent of the QWidget to adjust tables column
// sizes as the tables width is proportional to the dialogs width.
void TransactionView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    columnResizingFixer->stretchColumnWidth(TransactionTableModel::ToAddress);
}

// Need to override default Ctrl+C action for amount as default behaviour is just to copy DisplayRole text
bool TransactionView::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyPress)
    {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_C && ke->modifiers().testFlag(Qt::ControlModifier))
        {
             GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::TxPlainTextRole);
             return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

// show/hide column Watch-only
void TransactionView::updateWatchOnlyColumn(bool fHaveWatchOnly)
{
    watchOnlyWidget->setVisible(fHaveWatchOnly);
    transactionView->setColumnHidden(TransactionTableModel::Watchonly, !fHaveWatchOnly);
}

void TransactionView::copyAssetName()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::AssetNameRole);
}

void TransactionView::showAssets()
{
    if (AreAssetsDeployed() && !showingAssets) {
        typeWidget->addItem(tr("Asset Issued"), TransactionFilterProxy::TYPE(TransactionRecord::Issue));
        typeWidget->addItem(tr("Asset Reissued"), TransactionFilterProxy::TYPE(TransactionRecord::Reissue));
        typeWidget->addItem(tr("Asset Received"), TransactionFilterProxy::TYPE(TransactionRecord::TransferFrom));
        typeWidget->addItem(tr("Asset Sent"), TransactionFilterProxy::TYPE(TransactionRecord::TransferTo));

        assetNameWidget->show();

        QAction *copyAssetNameAction = new QAction(tr("Copy asset name"), this);
        contextMenu->addAction(copyAssetNameAction);
        connect(copyAssetNameAction, SIGNAL(triggered()), this, SLOT(copyAssetName()));

        showingAssets = true;
    }

    if (!AreAssetsDeployed())
    {
        assetNameWidget->hide();
    }
}