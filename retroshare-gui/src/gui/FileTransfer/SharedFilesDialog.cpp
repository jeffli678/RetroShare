/****************************************************************
 *  RShare is distributed under the following license:
 *
 *  Copyright (C) 2006, crypton
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA  02110-1301, USA.
 ****************************************************************/

#include <set>

#include <QString>
#include <QTreeView>
#include <QClipboard>
#include <QMenu>
#include <QPainter>
#include <QProcess>
#include <QMessageBox>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>

#include "rshare.h"
#include "SharedFilesDialog.h"
#include "gui/notifyqt.h"
#include "gui/MainWindow.h"
#include "gui/RemoteDirModel.h"
#include "gui/RetroShareLink.h"
#include "gui/ShareManager.h"
#include "gui/common/PeerDefs.h"
#include "gui/common/RsCollection.h"
#include "gui/msgs/MessageComposer.h"
#include "gui/settings/AddFileAssociationDialog.h"
#include "gui/gxschannels/GxsChannelDialog.h"
#include "gui/gxsforums/GxsForumsDialog.h"
#include "gui/settings/rsharesettings.h"
#include "util/QtVersion.h"
#include "util/RsAction.h"

#include <retroshare/rspeers.h>
#include <retroshare/rsfiles.h>
#include <retroshare/rsexpr.h>


/* Images for context menu icons */
#define IMAGE_DOWNLOAD       ":/images/download16.png"
#define IMAGE_PLAY           ":/images/start.png"
#define IMAGE_HASH_BUSY      ":/images/settings.png"
#define IMAGE_HASH_DONE      ":/images/accepted16.png"
#define IMAGE_MSG            ":/images/message-mail.png"
#define IMAGE_ATTACHMENT     ":/images/attachment.png"
#define IMAGE_FRIEND         ":/images/peers_16x16.png"
#define IMAGE_PROGRESS       ":/images/browse-looking.gif"
#define IMAGE_COPYLINK       ":/images/copyrslink.png"
#define IMAGE_OPENFOLDER     ":/images/folderopen.png"
#define IMAGE_OPENFILE       ":/images/fileopen.png"
#define IMAGE_LIBRARY        ":/images/library.png"
#define IMAGE_CHANNEL        ":/images/channels32.png"
#define IMAGE_FORUMS         ":/icons/png/forums.png"
#define IMAGE_COLLCREATE     ":/images/library_add.png"
#define IMAGE_COLLMODIF      ":/images/library_edit.png"
#define IMAGE_COLLVIEW       ":/images/library_view.png"
#define IMAGE_COLLOPEN       ":/images/library.png"
#define IMAGE_EDITSHARE      ":/images/edit_16.png"
#define IMAGE_MYFILES        ":/icons/svg/folders1.svg"

/*define dirTreeView Column */
#define COLUMN_NAME          0
#define COLUMN_SIZE          1
#define COLUMN_AGE           2
#define COLUMN_FRIEND        3
#define COLUMN_DIR           4
#define COLUMN_COUNT         5

/*define viewType_CB value */
#define VIEW_TYPE_TREE       0
#define VIEW_TYPE_FLAT       1

#define MAX_SEARCH_RESULTS   3000

// Define to avoid using the search in treeview, because it is really slow for now.
//
//#define DONT_USE_SEARCH_IN_TREE_VIEW 1

//#define DEBUG_SHARED_FILES_DIALOG 1

const QString Image_AddNewAssotiationForFile = ":/images/kcmsystem24.png";

class SFDSortFilterProxyModel : public QSortFilterProxyModel
{
public:
    SFDSortFilterProxyModel(RetroshareDirModel *dirModel, QObject *parent) : QSortFilterProxyModel(parent)
    {
        m_dirModel = dirModel;
    }

protected:
    virtual bool lessThan(const QModelIndex &left, const QModelIndex &right) const
    {
        bool dirLeft = (m_dirModel->getType(left) == DIR_TYPE_DIR);
        bool dirRight = (m_dirModel->getType(right) == DIR_TYPE_DIR);

        if (dirLeft ^ dirRight) {
            return dirLeft;
        }

        return QSortFilterProxyModel::lessThan(left, right);
    }

private:
    RetroshareDirModel *m_dirModel;
};

// This class allows to draw the item in the share flags column using an appropriate size

class ShareFlagsItemDelegate: public QStyledItemDelegate
{
public:
    ShareFlagsItemDelegate() {}

    virtual void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        Q_ASSERT(index.isValid());

        QStyleOptionViewItemV4 opt = option;
        initStyleOption(&opt, index);
        // disable default icon
        opt.icon = QIcon();
        // draw default item
        QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter, 0);

        const QRect r = option.rect;

        // get pixmap
        QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        QPixmap pix = icon.pixmap(r.size());

        // draw pixmap at center of item
        const QPoint p = QPoint((r.width() - pix.width())/2, (r.height() - pix.height())/2);
        painter->drawPixmap(r.topLeft() + p, pix);
    }
};

/** Constructor */
SharedFilesDialog::SharedFilesDialog(RetroshareDirModel *_tree_model,RetroshareDirModel *_flat_model,QWidget *parent)
: RsAutoUpdatePage(1000,parent),model(NULL)
{
	/* Invoke the Qt Designer generated object setup routine */
	ui.setupUi(this);

	NotifyQt *notify = NotifyQt::getInstance();
	connect(notify, SIGNAL(filesPreModChanged(bool)), this, SLOT(preModDirectories(bool)));
	connect(notify, SIGNAL(filesPostModChanged(bool)), this, SLOT(postModDirectories(bool)));

	connect(ui.viewType_CB, SIGNAL(currentIndexChanged(int)), this, SLOT(changeCurrentViewModel(int)));

	connect( ui.dirTreeView, SIGNAL( customContextMenuRequested( QPoint ) ), this,  SLOT( spawnCustomPopupMenu( QPoint ) ) );

	connect(ui.indicatorCBox, SIGNAL(currentIndexChanged(int)), this, SLOT(indicatorChanged(int)));

	tree_model = _tree_model ;
	flat_model = _flat_model ;

	tree_proxyModel = new SFDSortFilterProxyModel(tree_model, this);
	tree_proxyModel->setSourceModel(tree_model);
	tree_proxyModel->setSortCaseSensitivity(Qt::CaseInsensitive);
	tree_proxyModel->setSortRole(RetroshareDirModel::SortRole);
	tree_proxyModel->sort(COLUMN_NAME);

    flat_proxyModel = new SFDSortFilterProxyModel(flat_model, this);
    flat_proxyModel->setSourceModel(flat_model);
    flat_proxyModel->setSortCaseSensitivity(Qt::CaseInsensitive);
    flat_proxyModel->setSortRole(RetroshareDirModel::SortRole);
    flat_proxyModel->sort(COLUMN_NAME);

    // Mr.Alice: I removed this because it causes a crash for some obscur reason. Apparently when the model is changed, the proxy model cannot
    // deal with the change by itself. Should I call something specific? I've no idea. Removing this does not seem to cause any harm either.
    //Ghibli: set false because by default in qt5 is true and makes rs crash when sorting, all this decided by Cyril not me :D it works
    tree_proxyModel->setDynamicSortFilter(false);
    flat_proxyModel->setDynamicSortFilter(false);

    connect(ui.filterClearButton, SIGNAL(clicked()), this, SLOT(clearFilter()));
	connect(ui.filterStartButton, SIGNAL(clicked()), this, SLOT(startFilter()));
	connect(ui.filterPatternLineEdit, SIGNAL(returnPressed()), this, SLOT(startFilter()));
	connect(ui.filterPatternLineEdit, SIGNAL(textChanged(const QString &)), this, SLOT(filterRegExpChanged()));

	/* Set header resize modes and initial section sizes  */
	QHeaderView * header = ui.dirTreeView->header () ;

	header->resizeSection ( COLUMN_NAME, 490 );
    header->resizeSection ( COLUMN_SIZE, 70  );
    header->resizeSection ( COLUMN_AGE, 100  );
    header->resizeSection ( COLUMN_FRIEND,100);
    header->resizeSection ( COLUMN_DIR, 100  );

	header->setStretchLastSection(false);

	/* Set Multi Selection */
	ui.dirTreeView->setSelectionMode(QAbstractItemView::ExtendedSelection);

  /* Hide platform specific features */
  copylinkAct = new QAction(QIcon(IMAGE_COPYLINK), tr( "Copy retroshare Links to Clipboard" ), this );
  connect( copylinkAct , SIGNAL( triggered() ), this, SLOT( copyLink() ) );
  copylinkhtmlAct = new QAction(QIcon(IMAGE_COPYLINK), tr( "Copy retroshare Links to Clipboard (HTML)" ), this );
  connect( copylinkhtmlAct , SIGNAL( triggered() ), this, SLOT( copyLinkhtml() ) );
  sendlinkAct = new QAction(QIcon(IMAGE_COPYLINK), tr( "Send retroshare Links" ), this );
  connect( sendlinkAct , SIGNAL( triggered() ), this, SLOT( sendLinkTo( ) ) );

	collCreateAct= new QAction(QIcon(IMAGE_COLLCREATE), tr("Create Collection..."), this) ;
	connect(collCreateAct,SIGNAL(triggered()),this,SLOT(collCreate())) ;
	collModifAct= new QAction(QIcon(IMAGE_COLLMODIF), tr("Modify Collection..."), this) ;
	connect(collModifAct,SIGNAL(triggered()),this,SLOT(collModif())) ;
	collViewAct= new QAction(QIcon(IMAGE_COLLVIEW), tr("View Collection..."), this) ;
	connect(collViewAct,SIGNAL(triggered()),this,SLOT(collView())) ;
	collOpenAct = new QAction(QIcon(IMAGE_COLLOPEN), tr( "Download from collection file..." ), this ) ;
	connect(collOpenAct, SIGNAL(triggered()), this, SLOT(collOpen())) ;
}

LocalSharedFilesDialog::LocalSharedFilesDialog(QWidget *parent)
	: SharedFilesDialog(new TreeStyle_RDM(false),new FlatStyle_RDM(false),parent)
{
	// Hide columns after loading the settings
	ui.dirTreeView->setColumnHidden(COLUMN_DIR, false) ;
	ui.downloadButton->hide() ;

	// load settings
	processSettings(true);
	// Setup the current view model.
	//
	changeCurrentViewModel(ui.viewType_CB->currentIndex()) ;

	connect(ui.addShares_PB, SIGNAL(clicked()), this, SLOT(addShares())) ;
	connect(ui.checkButton, SIGNAL(clicked()), this, SLOT(forceCheck())) ;

	openfileAct = new QAction(QIcon(IMAGE_OPENFILE), tr("Open File"), this) ;
	connect(openfileAct, SIGNAL(triggered()), this, SLOT(openfile())) ;
	openfolderAct = new QAction(QIcon(IMAGE_OPENFOLDER), tr("Open Folder"), this) ;
	connect(openfolderAct, SIGNAL(triggered()), this, SLOT(openfolder())) ;

    ui.titleBarPixmap->setPixmap(QPixmap(IMAGE_MYFILES)) ;

    ui.dirTreeView->setItemDelegateForColumn(COLUMN_FRIEND,new ShareFlagsItemDelegate()) ;
}

RemoteSharedFilesDialog::RemoteSharedFilesDialog(QWidget *parent)
	: SharedFilesDialog(new TreeStyle_RDM(true),new FlatStyle_RDM(true),parent)
{
	ui.dirTreeView->setColumnHidden(COLUMN_FRIEND, false) ;
	ui.dirTreeView->setColumnHidden(COLUMN_DIR, true) ;
	ui.checkButton->hide() ;

	connect(ui.downloadButton, SIGNAL(clicked()), this, SLOT(downloadRemoteSelected()));
    connect(ui.dirTreeView, SIGNAL(  expanded(const QModelIndex & ) ), this, SLOT(   expanded(const QModelIndex & ) ) );
    connect(ui.dirTreeView, SIGNAL(  doubleClicked(const QModelIndex & ) ), this, SLOT(   expanded(const QModelIndex & ) ) );

	// load settings
	processSettings(true);
	// Setup the current view model.
	//
	changeCurrentViewModel(ui.viewType_CB->currentIndex()) ;

	ui.addShares_PB->hide() ;
}

void LocalSharedFilesDialog::addShares()
{
	ShareManager::showYourself();
}

void SharedFilesDialog::hideEvent(QHideEvent *)
{
	if(model!=NULL)
		model->setVisible(false) ;
}
void SharedFilesDialog::showEvent(QShowEvent *)
{
	if(model!=NULL)
	{
        std::set<std::string> expanded_indexes,hidden_indexes,selected_indexes ;

        saveExpandedPathsAndSelection(expanded_indexes,hidden_indexes,selected_indexes);

        model->setVisible(true) ;
		model->update() ;

        restoreExpandedPathsAndSelection(expanded_indexes,hidden_indexes,selected_indexes);
    }
}
RemoteSharedFilesDialog::~RemoteSharedFilesDialog()
{
    // save settings
    processSettings(false);
}

LocalSharedFilesDialog::~LocalSharedFilesDialog()
{
    // save settings
    processSettings(false);
}

void LocalSharedFilesDialog::processSettings(bool bLoad)
{
	Settings->beginGroup("LocalSharedFilesDialog");

	if (bLoad) {
		// load settings

		// state of the trees
		ui.dirTreeView->header()->restoreState(Settings->value("LocalDirTreeView").toByteArray());

		// state of splitter
		ui.splitter->restoreState(Settings->value("LocalSplitter").toByteArray());

		// view type
		ui.viewType_CB->setCurrentIndex(Settings->value("LocalViewType").toInt());
	} else {
		// save settings

		// state of trees
		Settings->setValue("LocalDirTreeView", ui.dirTreeView->header()->saveState());

		// state of splitter
		Settings->setValue("LocalSplitter", ui.splitter->saveState());

		// view type
		Settings->setValue("LocalViewType", ui.viewType_CB->currentIndex());
	}

	Settings->endGroup();
}
void RemoteSharedFilesDialog::processSettings(bool bLoad)
{
	Settings->beginGroup("SharedFilesDialog");

	if (bLoad) {
		// load settings

		// state of the trees
		ui.dirTreeView->header()->restoreState(Settings->value("RemoteDirTreeView").toByteArray());

		// state of splitter
		ui.splitter->restoreState(Settings->value("RemoteSplitter").toByteArray());

		// view type
		ui.viewType_CB->setCurrentIndex(Settings->value("RemoteViewType").toInt());
	} else {
		// save settings

		// state of trees
		Settings->setValue("RemoteDirTreeView", ui.dirTreeView->header()->saveState());

		// state of splitter
		Settings->setValue("RemoteSplitter", ui.splitter->saveState());

		// view type
		Settings->setValue("RemoteViewType", ui.viewType_CB->currentIndex());
	}

	Settings->endGroup();
}

void SharedFilesDialog::changeCurrentViewModel(int viewTypeIndex)
{
//    disconnect( ui.dirTreeView, SIGNAL( collapsed(const QModelIndex & ) ), NULL, NULL );
//    disconnect( ui.dirTreeView, SIGNAL(  expanded(const QModelIndex & ) ), NULL, NULL );

	if(model!=NULL)
		model->setVisible(false) ;

	if(viewTypeIndex==VIEW_TYPE_TREE)
	{
		model = tree_model ;
        proxyModel = tree_proxyModel ;
	}
	else
	{
		model = flat_model ;
        proxyModel = flat_proxyModel ;
    }

	showProperColumns() ;

    std::set<std::string> expanded_indexes,hidden_indexes,selected_indexes ;

    saveExpandedPathsAndSelection(expanded_indexes,hidden_indexes,selected_indexes);

    if(isVisible())
	{
        model->setVisible(true) ;

		model->update() ;
	}

//    connect( ui.dirTreeView, SIGNAL( collapsed(const QModelIndex & ) ), this, SLOT(  collapsed(const QModelIndex & ) ) );

	ui.dirTreeView->setModel(proxyModel);
	ui.dirTreeView->update();

    restoreExpandedPathsAndSelection(expanded_indexes,hidden_indexes,selected_indexes);

    QHeaderView * header = ui.dirTreeView->header () ;
	QHeaderView_setSectionResizeModeColumn(header, COLUMN_NAME, QHeaderView::Interactive);

	ui.dirTreeView->header()->headerDataChanged(Qt::Horizontal, COLUMN_NAME, COLUMN_DIR) ;

//    recursRestoreExpandedItems(ui.dirTreeView->rootIndex(),expanded_indexes);
    FilterItems();
}

void LocalSharedFilesDialog::showProperColumns()
{
	if(model == tree_model)
	{
		ui.dirTreeView->setColumnHidden(COLUMN_FRIEND, false) ;
		ui.dirTreeView->setColumnHidden(COLUMN_DIR, false) ;
#ifdef DONT_USE_SEARCH_IN_TREE_VIEW
		ui.filterLabel->hide();
		ui.filterPatternLineEdit->hide();
		ui.filterStartButton->hide();
		ui.filterClearButton->hide();
#endif
	}
	else
	{
		ui.dirTreeView->setColumnHidden(COLUMN_FRIEND, true) ;
		ui.dirTreeView->setColumnHidden(COLUMN_DIR, false) ;
#ifdef DONT_USE_SEARCH_IN_TREE_VIEW
		ui.filterLabel->show();
		ui.filterPatternLineEdit->show();
#endif
	}
}
void RemoteSharedFilesDialog::showProperColumns()
{
	if(model == tree_model)
	{
		ui.dirTreeView->setColumnHidden(COLUMN_FRIEND, true) ;
		ui.dirTreeView->setColumnHidden(COLUMN_DIR, true) ;
#ifdef DONT_USE_SEARCH_IN_TREE_VIEW
		ui.filterLabel->hide();
		ui.filterPatternLineEdit->hide();
		ui.filterStartButton->hide();
		ui.filterClearButton->hide();
#endif
	}
	else
	{
		ui.dirTreeView->setColumnHidden(COLUMN_FRIEND, false) ;
		ui.dirTreeView->setColumnHidden(COLUMN_DIR, false) ;
#ifdef DONT_USE_SEARCH_IN_TREE_VIEW
		ui.filterLabel->show();
		ui.filterPatternLineEdit->show();
#endif
	}
}

void LocalSharedFilesDialog::checkUpdate()
{
	/* update */
	if (rsFiles->InDirectoryCheck())
	{
		ui.checkButton->setText(tr("Checking..."));
	}
	else
	{
		ui.checkButton->setText(tr("Check files"));
		ui.hashLabel->setPixmap(QPixmap(IMAGE_HASH_DONE));
		ui.hashLabel->setToolTip("") ;
	}

	return;
}

void LocalSharedFilesDialog::forceCheck()
{
	rsFiles->ForceDirectoryCheck();
	return;
}

void RemoteSharedFilesDialog::spawnCustomPopupMenu( QPoint point )
{
	if (!rsPeers) return; /* not ready yet! */

	QModelIndex idx = ui.dirTreeView->indexAt(point) ;
	if (!idx.isValid()) return;

	QModelIndex midx = proxyModel->mapToSource(idx) ;
	if (!midx.isValid()) return;

	currentFile = model->data(midx, RetroshareDirModel::FileNameRole).toString() ;
	int type = model->getType(midx) ;
	if (type != DIR_TYPE_DIR && type != DIR_TYPE_FILE) return;


	QMenu contextMnu( this ) ;

	collCreateAct->setEnabled(true);
	collOpenAct->setEnabled(true);

	QMenu collectionMenu(tr("Collection"), this);
	collectionMenu.setIcon(QIcon(IMAGE_LIBRARY));
	collectionMenu.addAction(collCreateAct);
	collectionMenu.addAction(collOpenAct);

	QModelIndexList list = ui.dirTreeView->selectionModel()->selectedRows() ;

	if(type == DIR_TYPE_DIR || list.size() > 1)
	{
		QAction *downloadActI = new QAction(QIcon(IMAGE_DOWNLOAD), tr( "Download..." ), &contextMnu ) ;
		connect( downloadActI , SIGNAL( triggered() ), this, SLOT( downloadRemoteSelectedInteractive() ) ) ;
		contextMnu.addAction( downloadActI) ;
	}
	else
	{
		QAction *downloadAct = new QAction(QIcon(IMAGE_DOWNLOAD), tr( "Download" ), &contextMnu ) ;
		connect( downloadAct , SIGNAL( triggered() ), this, SLOT( downloadRemoteSelected() ) ) ;
		contextMnu.addAction( downloadAct) ;
	}

	contextMnu.addSeparator() ;//------------------------------------
	contextMnu.addAction( copylinkAct) ;
	contextMnu.addAction( sendlinkAct) ;
	contextMnu.addSeparator() ;//------------------------------------
	contextMnu.addAction(QIcon(IMAGE_MSG), tr("Recommend in a message to..."), this, SLOT(recommendFilesToMsg())) ;


	contextMnu.addSeparator() ;//------------------------------------
	contextMnu.addMenu(&collectionMenu) ;

	contextMnu.exec(QCursor::pos()) ;
}

QModelIndexList SharedFilesDialog::getSelected()
{
	QModelIndexList list = ui.dirTreeView->selectionModel()->selectedIndexes() ;
	QModelIndexList proxyList ;
	for (QModelIndexList::iterator index = list.begin(); index != list.end(); ++index ) {
		proxyList.append(proxyModel->mapToSource(*index)) ;
	}

	return proxyList ;
}

void RemoteSharedFilesDialog::expanded(const QModelIndex& indx)
{
#ifdef DEBUG_SHARED_FILES_DIALOG
    std::cerr << "Expanding at " << indx.row() << " and " << indx.column() << " ref=" << indx.internalPointer() << ", pointer at 1: " << proxyModel->mapToSource(indx).internalPointer() << std::endl;
#endif

    model->updateRef(proxyModel->mapToSource(indx)) ;
}
void RemoteSharedFilesDialog::downloadRemoteSelectedInteractive()
{
	/* call back to the model (which does all the interfacing?  */

	std::cerr << "Downloading Files" ;
	std::cerr << std::endl ;

	QModelIndexList lst = getSelected() ;
	model -> downloadSelected(lst,true) ;
}
void RemoteSharedFilesDialog::downloadRemoteSelected()
{
	/* call back to the model (which does all the interfacing?  */

	std::cerr << "Downloading Files" ;
	std::cerr << std::endl ;

	QModelIndexList lst = getSelected() ;
	model -> downloadSelected(lst,false) ;
}

void SharedFilesDialog::copyLinks(const QModelIndexList& lst, bool remote,QList<RetroShareLink>& urls,bool& has_unhashed_files)
{
	std::vector<DirDetails> dirVec;

	model->getDirDetailsFromSelect(lst, dirVec);

	has_unhashed_files = false;

	for (int i = 0, n = dirVec.size(); i < n; ++i)
	{
		const DirDetails& details = dirVec[i];

		if (details.type == DIR_TYPE_DIR)
		{
			FileTree *ft = FileTree::create(details,remote) ;

			std::cerr << "Created collection file tree:" << std::endl;
			ft->print();

			QString dir_name = QDir(QString::fromUtf8(details.name.c_str())).dirName();

			RetroShareLink link = RetroShareLink::createCollection(dir_name,ft->mTotalSize,ft->mTotalFiles,QString::fromStdString(ft->toRadix64())) ;

			if(link.valid())
				urls.push_back(link) ;

			delete ft ;
		}
		else
		{
			if(details.hash.isNull())
			{
				has_unhashed_files = true;
				continue;
			}
			RetroShareLink link = RetroShareLink::createFile(QString::fromUtf8(details.name.c_str()), details.count, details.hash.toStdString().c_str());
			if (link.valid()) {
				urls.push_back(link) ;
			}
		}
	}
}

void SharedFilesDialog::copyLink (const QModelIndexList& lst, bool remote)
{
	QList<RetroShareLink> urls ;
	bool has_unhashed_files = false;

	copyLinks(lst,remote,urls,has_unhashed_files) ;
    RSLinkClipboard::copyLinks(urls) ;

    if(has_unhashed_files)
        QMessageBox::warning(NULL,tr("Some files have been omitted"),tr("Some files have been omitted because they have not been indexed yet.")) ;
}

void SharedFilesDialog::copyLink()
{
    copyLink ( getSelected() , isRemote());
}

void SharedFilesDialog::copyLinkhtml( )
{
    copyLink();

    QString link = QApplication::clipboard()->text();

    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText("<a href='" +  link + "'> " + link + "</a>");
}

void SharedFilesDialog::sendLinkTo()
{
    copyLink();

    /* create a message */
    MessageComposer *nMsgDialog = MessageComposer::newMsg();
    if (nMsgDialog == NULL) {
        return;
    }

    /* fill it in
    * files are receommended already
    * just need to set peers
    */
    std::cerr << "SharedFilesDialog::sendLinkTo()" << std::endl;
    nMsgDialog->setTitleText(tr("RetroShare Link"));
    nMsgDialog->setMsgText(RSLinkClipboard::toHtml(), true);

    nMsgDialog->show();

    /* window will destroy itself! */
}

void SharedFilesDialog::collCreate()
{
	QModelIndexList lst = getSelected();
	model->createCollectionFile(this, lst);
}

void SharedFilesDialog::collModif()
{
	std::list<DirDetails> files_info ;

	model->getFileInfoFromIndexList(getSelected(),files_info);

	if(files_info.size() != 1) return ;

	/* make path for downloaded files */
	std::list<DirDetails>::iterator it = files_info.begin();
	DirDetails details = (*it);
	FileInfo info;
	if (!rsFiles->FileDetails(details.hash, RS_FILE_HINTS_EXTRA | RS_FILE_HINTS_LOCAL
	                                        | RS_FILE_HINTS_BROWSABLE | RS_FILE_HINTS_NETWORK_WIDE
	                                        | RS_FILE_HINTS_SPEC_ONLY, info)) return;

	std::string path;
	path = info.path;

	/* open file with a suitable application */
	QFileInfo qinfo;
	qinfo.setFile(QString::fromUtf8(path.c_str()));
	if (qinfo.exists()) {
		if (qinfo.absoluteFilePath().endsWith(RsCollection::ExtensionString)) {
			RsCollection collection;
			collection.openColl(qinfo.absoluteFilePath());
		}
	}
}

void SharedFilesDialog::collView()
{
	std::list<DirDetails> files_info ;

	model->getFileInfoFromIndexList(getSelected(),files_info);

	if(files_info.size() != 1) return ;

	/* make path for downloaded files */
	std::list<DirDetails>::iterator it = files_info.begin();
	DirDetails details = (*it);
	FileInfo info;
	if (!rsFiles->FileDetails(details.hash, RS_FILE_HINTS_EXTRA | RS_FILE_HINTS_LOCAL
	                                        | RS_FILE_HINTS_BROWSABLE | RS_FILE_HINTS_NETWORK_WIDE
	                                        | RS_FILE_HINTS_SPEC_ONLY, info)) return;

	std::string path;
	path = info.path;

	/* open file with a suitable application */
	QFileInfo qinfo;
	qinfo.setFile(QString::fromUtf8(path.c_str()));
	if (qinfo.exists()) {
		if (qinfo.absoluteFilePath().endsWith(RsCollection::ExtensionString)) {
			RsCollection collection;
			collection.openColl(qinfo.absoluteFilePath(), true);
		}
	}
}

void SharedFilesDialog::collOpen()
{
	std::list<DirDetails> files_info ;

	model->getFileInfoFromIndexList(getSelected(),files_info);

	if(files_info.size() == 1) {

		/* make path for downloaded files */
		std::list<DirDetails>::iterator it = files_info.begin();
		DirDetails details = (*it);
		FileInfo info;
		if (rsFiles->FileDetails(details.hash, RS_FILE_HINTS_EXTRA | RS_FILE_HINTS_LOCAL
		                          | RS_FILE_HINTS_BROWSABLE | RS_FILE_HINTS_NETWORK_WIDE
		                          | RS_FILE_HINTS_SPEC_ONLY, info)) {

			std::string path;
			path = info.path;

			/* open file with a suitable application */
			QFileInfo qinfo;
			qinfo.setFile(QString::fromUtf8(path.c_str()));
			if (qinfo.exists()) {
				if (qinfo.absoluteFilePath().endsWith(RsCollection::ExtensionString)) {
					RsCollection collection;
					if (collection.load(qinfo.absoluteFilePath())) {
						collection.downloadFiles();
						return;
					}
				}
			}
		}
	}

	RsCollection collection;
	if (collection.load(this)) {
		collection.downloadFiles();
	}
}

void LocalSharedFilesDialog::playselectedfiles()
{
  /* call back to the model (which does all the interfacing? */

  std::cerr << "SharedFilesDialog::playselectedfiles()";
  std::cerr << std::endl;

  std::list<std::string> paths;
  model -> getFilePaths(getSelected(), paths);

  std::list<std::string>::iterator it;
  QStringList fullpaths;
  for(it = paths.begin(); it != paths.end(); ++it)
  {
	  std::string fullpath;
	  rsFiles->ConvertSharedFilePath(*it, fullpath);
	  fullpaths.push_back(QString::fromStdString(fullpath));

	  std::cerr << "Playing: " << fullpath;
	  std::cerr << std::endl;
  }

  playFiles(fullpaths);

  std::cerr << "SharedFilesDialog::playselectedfiles() Completed";
  std::cerr << std::endl;
}

void SharedFilesDialog::recommendFilesToMsg()
{
    std::list<DirDetails> files_info ;

    model->getFileInfoFromIndexList(getSelected(),files_info);

    if(files_info.empty())
        return ;

    /* create a message */

    MessageComposer *nMsgDialog = MessageComposer::newMsg();
    if (nMsgDialog == NULL) {
        return;
    }

    nMsgDialog->setFileList(files_info) ;
    nMsgDialog->setTitleText(tr("Recommendation(s)"));
    nMsgDialog->setMsgText(tr("Recommendation(s)"));
    nMsgDialog->show();

    /* window will destroy itself! */
}

void LocalSharedFilesDialog::openfile()
{
	/* call back to the model (which does all the interfacing? */

	std::cerr << "SharedFilesDialog::openfile" << std::endl;

	QModelIndexList qmil = getSelected();
	model->openSelected(qmil);
}


void LocalSharedFilesDialog::openfolder()
{
	std::cerr << "SharedFilesDialog::openfolder" << std::endl;

	QModelIndexList qmil = getSelected();
	model->openSelected(qmil);
}

void  SharedFilesDialog::preModDirectories(bool local)
{
	if (isRemote() == local)
		return;

#ifdef DEBUG_SHARED_FILES_DIALOG
    std::cerr << "About to modify directories. Local=" << local << ". Temporarily disabling sorting" << std::endl;
#endif

    ui.dirTreeView->setSortingEnabled(false);

    std::set<std::string> expanded_indexes,hidden_indexes,selected_indexes;
    saveExpandedPathsAndSelection(expanded_indexes,hidden_indexes,selected_indexes) ;

	/* Notify both models, only one is visible */
	tree_model->preMods();
	flat_model->preMods();

    restoreExpandedPathsAndSelection(expanded_indexes,hidden_indexes,selected_indexes) ;
}

void SharedFilesDialog::saveExpandedPathsAndSelection(std::set<std::string>& expanded_indexes,
                                                      std::set<std::string>& hidden_indexes,
                                                      std::set<std::string>& selected_indexes)
{
    if(ui.dirTreeView->model() == NULL)
        return ;

#ifdef DEBUG_SHARED_FILES_DIALOG
    std::cerr << "Saving expanded items. " << std::endl;
#endif
    for(int row = 0; row < ui.dirTreeView->model()->rowCount(); ++row)
    {
        std::string path = ui.dirTreeView->model()->index(row,0).data(Qt::DisplayRole).toString().toStdString();

        recursSaveExpandedItems(ui.dirTreeView->model()->index(row,0),path,expanded_indexes,hidden_indexes,selected_indexes);
    }
}

void SharedFilesDialog::restoreExpandedPathsAndSelection(const std::set<std::string>& expanded_indexes,
                                                         const std::set<std::string>& hidden_indexes,
                                                         const std::set<std::string>& selected_indexes)
{
    if(ui.dirTreeView->model() == NULL)
        return ;

    // we need to disable this, because the signal will trigger unnecessary update at the friend.

    ui.dirTreeView->blockSignals(true) ;

#ifdef DEBUG_SHARED_FILES_DIALOG
    std::cerr << "Restoring expanded items. " << std::endl;
#endif
    for(int row = 0; row < ui.dirTreeView->model()->rowCount(); ++row)
    {
        std::string path = ui.dirTreeView->model()->index(row,0).data(Qt::DisplayRole).toString().toStdString();
        recursRestoreExpandedItems(ui.dirTreeView->model()->index(row,0),path,expanded_indexes,hidden_indexes,selected_indexes);
    }
    QItemSelection selection ;

    ui.dirTreeView->blockSignals(false) ;
}

void SharedFilesDialog::recursSaveExpandedItems(const QModelIndex& index,const std::string& path,std::set<std::string>& exp,
                                                std::set<std::string>& vis,
                                                std::set<std::string>& sel
                                                )
{
    std::string local_path = path+"/"+index.data(Qt::DisplayRole).toString().toStdString();
#ifdef DEBUG_SHARED_FILES_DIALOG
    std::cerr << "at index " << index.row() << ". data[1]=" << local_path << std::endl;
#endif

    if(ui.dirTreeView->selectionModel()->selection().contains(index))
        sel.insert(local_path) ;

    if(ui.dirTreeView->isRowHidden(index.row(),index.parent()))
        vis.insert(local_path) ;

    if(ui.dirTreeView->isExpanded(index))
    {
#ifdef DEBUG_SHARED_FILES_DIALOG
        std::cerr << "Index " << local_path << " is expanded." << std::endl;
#endif
        if(index.isValid())
            exp.insert(local_path) ;

        for(int row=0;row<ui.dirTreeView->model()->rowCount(index);++row)
            recursSaveExpandedItems(index.child(row,0),local_path,exp,vis,sel) ;
    }
#ifdef DEBUG_SHARED_FILES_DIALOG
    else
        std::cerr << "Index is not expanded." << std::endl;
#endif
}

void SharedFilesDialog::recursRestoreExpandedItems(const QModelIndex& index, const std::string &path,
                                                   const std::set<std::string>& exp,
                                                   const std::set<std::string>& vis,
                                                   const std::set<std::string> &sel)
{
    std::string local_path = path+"/"+index.data(Qt::DisplayRole).toString().toStdString();
#ifdef DEBUG_SHARED_FILES_DIALOG
    std::cerr << "at index " << index.row() << ". data[1]=" << local_path << std::endl;
#endif
    if(sel.find(local_path) != sel.end())
        ui.dirTreeView->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);

	ui.dirTreeView->setRowHidden(index.row(),index.parent(), vis.find(local_path) != vis.end()) ;

    if(exp.find(local_path) != exp.end())
    {
#ifdef DEBUG_SHARED_FILES_DIALOG
        std::cerr << "re expanding index " << local_path << std::endl;
#endif
        ui.dirTreeView->setExpanded(index,true) ;

        for(int row=0;row<ui.dirTreeView->model()->rowCount(index);++row)
            recursRestoreExpandedItems(index.child(row,0),local_path,exp,vis,sel) ;
    }
}


void  SharedFilesDialog::postModDirectories(bool local)
{
	if (isRemote() == local)
		return;

    std::set<std::string> expanded_indexes,selected_indexes,hidden_indexes;

    saveExpandedPathsAndSelection(expanded_indexes,hidden_indexes,selected_indexes) ;
#ifdef DEBUG_SHARED_FILES_DIALOG
    std::cerr << "Saving expanded items. " << expanded_indexes.size() << " items found" << std::endl;
#endif

    /* Notify both models, only one is visible */
	tree_model->postMods();
	flat_model->postMods();
	ui.dirTreeView->update() ;

    if (ui.filterPatternLineEdit->text().isEmpty() == false)
		FilterItems();

    ui.dirTreeView->setSortingEnabled(true);

    restoreExpandedPathsAndSelection(expanded_indexes,hidden_indexes,selected_indexes) ;

#ifdef DEBUG_SHARED_FILES_DIALOG
    std::cerr << "****** updated directories! Re-enabling sorting ******" << std::endl;
#endif
	QCoreApplication::flush();
}

class ChannelCompare
{
public:
	bool operator()(const std::pair<std::string,RsGxsGroupId>& id1,const std::pair<std::string,RsGxsGroupId>& id2) const
	{
		return id1.first < id2.first ;
	}
};

void LocalSharedFilesDialog::spawnCustomPopupMenu( QPoint point )
{
	if (!rsPeers) return; /* not ready yet! */

	QModelIndex idx = ui.dirTreeView->indexAt(point) ;
	if (!idx.isValid()) return ;

	QModelIndex midx = proxyModel->mapToSource(idx) ;
	if (!midx.isValid()) return ;


	currentFile = model->data(midx, RetroshareDirModel::FileNameRole).toString() ;
	int type = model->getType(midx) ;
	if (type != DIR_TYPE_DIR && type != DIR_TYPE_FILE) return;

	QMenu contextMnu(this) ;

	bool bIsRsColl = currentFile.endsWith(RsCollection::ExtensionString);
	collCreateAct->setEnabled(true);
	collModifAct->setEnabled(bIsRsColl);
	collViewAct->setEnabled(bIsRsColl);
	collOpenAct->setEnabled(true);

	QMenu collectionMenu(tr("Collection"), this);
	collectionMenu.setIcon(QIcon(IMAGE_LIBRARY));
	collectionMenu.addAction(collCreateAct);
	collectionMenu.addAction(collModifAct);
	collectionMenu.addAction(collViewAct);
	collectionMenu.addAction(collOpenAct);

	switch (type) {
		case DIR_TYPE_DIR :
			contextMnu.addAction(openfolderAct) ;
			contextMnu.addAction(copylinkAct) ;
			contextMnu.addSeparator() ;//------------------------------------
			contextMnu.addMenu(&collectionMenu) ;
		break ;

		case DIR_TYPE_FILE :
			contextMnu.addAction(openfileAct) ;
			contextMnu.addSeparator() ;//------------------------------------
			contextMnu.addAction(copylinkAct) ;
			contextMnu.addAction(sendlinkAct) ;
			contextMnu.addSeparator() ;//------------------------------------
			contextMnu.addMenu(&collectionMenu) ;
			contextMnu.addSeparator() ;//------------------------------------
			contextMnu.addAction(QIcon(IMAGE_MSG), tr("Recommend in a message to..."), this, SLOT(recommendFilesToMsg())) ;
		break ;

		default :
		return ;
	}

	GxsChannelDialog *channelDialog = dynamic_cast<GxsChannelDialog*>(MainWindow::getPage(MainWindow::Channels));
	QMenu shareChannelMenu(tr("Share on channel...")) ; // added here because the shareChannelMenu QMenu object is deleted afterwards

	if(channelDialog != NULL)
	{
		shareChannelMenu.setIcon(QIcon(IMAGE_CHANNEL));

		std::list<RsGroupMetaData> grp_metas ;
		channelDialog->getGroupList(grp_metas) ;

		std::vector<std::pair<std::string,RsGxsGroupId> > grplist ; // I dont use a std::map because two or more channels may have the same name.

		for(auto it(grp_metas.begin());it!=grp_metas.end();++it)
			if(IS_GROUP_PUBLISHER((*it).mSubscribeFlags) && IS_GROUP_SUBSCRIBED((*it).mSubscribeFlags))
				grplist.push_back(std::make_pair((*it).mGroupName, (*it).mGroupId));

		std::sort(grplist.begin(),grplist.end(),ChannelCompare()) ;

		for(auto it(grplist.begin());it!=grplist.end();++it)
				shareChannelMenu.addAction(QString::fromUtf8((*it).first.c_str()), this, SLOT(shareOnChannel()))->setData(QString::fromStdString((*it).second.toStdString())) ;

		contextMnu.addMenu(&shareChannelMenu) ;
	}

	GxsForumsDialog *forumsDialog = dynamic_cast<GxsForumsDialog*>(MainWindow::getPage(MainWindow::Forums));
	QMenu shareForumMenu(tr("Share on forum...")) ; // added here because the shareChannelMenu QMenu object is deleted afterwards

	if(forumsDialog != NULL)
	{
		shareForumMenu.setIcon(QIcon(IMAGE_FORUMS));

		std::list<RsGroupMetaData> grp_metas ;
		forumsDialog->getGroupList(grp_metas) ;

		std::vector<std::pair<std::string,RsGxsGroupId> > grplist ; // I dont use a std::map because two or more channels may have the same name.

		for(auto it(grp_metas.begin());it!=grp_metas.end();++it)
			if(IS_GROUP_SUBSCRIBED((*it).mSubscribeFlags))
				grplist.push_back(std::make_pair((*it).mGroupName, (*it).mGroupId));

		std::sort(grplist.begin(),grplist.end(),ChannelCompare()) ;

		for(auto it(grplist.begin());it!=grplist.end();++it)
			shareForumMenu.addAction(QString::fromUtf8((*it).first.c_str()), this, SLOT(shareInForum()))->setData(QString::fromStdString((*it).second.toStdString())) ;

		contextMnu.addMenu(&shareForumMenu) ;
	}

	contextMnu.exec(QCursor::pos()) ;
}
void LocalSharedFilesDialog::shareOnChannel()
{
	RsGxsGroupId groupId(qobject_cast<QAction*>(sender())->data().toString().toStdString());

	GxsChannelDialog *channelDialog = dynamic_cast<GxsChannelDialog*>(MainWindow::getPage(MainWindow::Channels));

	if(channelDialog == NULL)
		return ;

	std::list<DirDetails> files_info ;

	QList<RetroShareLink> file_links_list ;
	bool has_unhashed_files ;

	copyLinks(getSelected(),false,file_links_list,has_unhashed_files) ;

	channelDialog->shareOnChannel(groupId,file_links_list) ;
}
void LocalSharedFilesDialog::shareInForum()
{
	RsGxsGroupId groupId(qobject_cast<QAction*>(sender())->data().toString().toStdString());

	GxsForumsDialog *forumsDialog = dynamic_cast<GxsForumsDialog*>(MainWindow::getPage(MainWindow::Forums));

	if(forumsDialog == NULL)
		return ;

	std::list<DirDetails> files_info ;

	QList<RetroShareLink> file_links_list ;
	bool has_unhashed_files ;

	copyLinks(getSelected(),false,file_links_list,has_unhashed_files) ;

	forumsDialog->shareInMessage(groupId,file_links_list) ;
}

//============================================================================

QAction*
LocalSharedFilesDialog::fileAssotiationAction(const QString /*fileName*/)
{
    QAction* result = NULL;

    Settings->beginGroup("FileAssotiations");

    QString key = AddFileAssociationDialog::cleanFileType(currentFile) ;
    if ( Settings->contains(key) )
    {
        result = new QAction(QIcon(IMAGE_PLAY), tr( "Open File" ), this );
        connect( result , SIGNAL( triggered() ),
                 this, SLOT( runCommandForFile() ) );

        currentCommand = (Settings->value( key )).toString();
    }
    else
    {
        result = new QAction(QIcon(Image_AddNewAssotiationForFile),
                             tr( "Set command for opening this file"), this );
        connect( result , SIGNAL( triggered() ),
                 this,    SLOT(   tryToAddNewAssotiation() ) );
    }

    Settings->endGroup();

    return result;
}

//============================================================================

void
LocalSharedFilesDialog::runCommandForFile()
{
    QStringList tsl;
    tsl.append( currentFile );
    QProcess::execute( currentCommand, tsl);
}

//============================================================================

void
LocalSharedFilesDialog::tryToAddNewAssotiation()
{
    AddFileAssociationDialog afad(true, this);//'add file assotiations' dialog

    afad.setFileType(AddFileAssociationDialog::cleanFileType(currentFile));

    int ti = afad.exec();

    if (ti==QDialog::Accepted)
    {
        QString currType = afad.resultFileType() ;
        QString currCmd = afad.resultCommand() ;

        Settings->setValueToGroup("FileAssotiations", currType, currCmd);
    }
}

void SharedFilesDialog::indicatorChanged(int index)
{
	static uint32_t correct_indicator[4] = { IND_ALWAYS,IND_LAST_DAY,IND_LAST_WEEK,IND_LAST_MONTH } ;

	model->changeAgeIndicator(correct_indicator[index]);

	ui.dirTreeView->update(ui.dirTreeView->rootIndex());

	if (correct_indicator[index] != IND_ALWAYS)
		ui.dirTreeView->sortByColumn(COLUMN_AGE, Qt::AscendingOrder);
	else
		ui.dirTreeView->sortByColumn(COLUMN_NAME, Qt::AscendingOrder);

	updateDisplay() ;
}

void SharedFilesDialog::filterRegExpChanged()
{
    QString text = ui.filterPatternLineEdit->text();

    if (text.isEmpty()) {
        ui.filterClearButton->hide();
    } else {
        ui.filterClearButton->show();
    }

    if (text == lastFilterString) {
        ui.filterStartButton->hide();
    } else {
        ui.filterStartButton->show();
    }

	bool valid = false ;
	QColor color ;

	if(text.length() > 0 && text.length() < 3)
	{
		valid = false;

		ui.filterStartButton->setEnabled(false) ;
		ui.filterPatternFrame->setToolTip(tr("Search string should be at least 3 characters long.")) ;
		return ;
	}

	if(text.length() > 0 && proxyModel == tree_proxyModel)
	{
		std::list<DirDetails> result_list ;
		std::list<std::string> keywords;

		QStringList lst = text.split(" ",QString::SkipEmptyParts) ;

		for(auto it(lst.begin());it!=lst.end();++it)
			keywords.push_back((*it).toStdString());

		FileSearchFlags flags = isRemote()?RS_FILE_HINTS_REMOTE:RS_FILE_HINTS_LOCAL;

		if(keywords.size() > 1)
		{
			RsRegularExpression::NameExpression exp(RsRegularExpression::ContainsAllStrings,keywords,true);
			rsFiles->SearchBoolExp(&exp,result_list, flags) ;
		}
		else
			rsFiles->SearchKeywords(keywords,result_list, flags) ;

		uint32_t nb_results = result_list.size();

		if(nb_results > MAX_SEARCH_RESULTS)
		{
			ui.filterStartButton->setEnabled(false) ;
			ui.filterPatternFrame->setToolTip(tr("More than 3000 results. Add more/longer search words to select less.")) ;
			return ;
		}
	}

	ui.filterStartButton->setEnabled(true) ;
	ui.filterPatternFrame->setToolTip(QString());

	/* unpolish widget to clear the stylesheet's palette cache */
	// ui.filterPatternFrame->style()->unpolish(ui.filterPatternFrame);

	// QPalette palette = ui.filterPatternLineEdit->palette();
	// palette.setColor(ui.filterPatternLineEdit->backgroundRole(), color);
	// ui.filterPatternLineEdit->setPalette(palette);

	// //ui.searchLineFrame->setProperty("valid", valid);
	// Rshare::refreshStyleSheet(ui.filterPatternFrame, false);
}

/* clear Filter */
void SharedFilesDialog::clearFilter()
{
    ui.filterPatternLineEdit->clear();
    ui.filterPatternLineEdit->setFocus();

    startFilter();
}

/* clear Filter */
void SharedFilesDialog::startFilter()
{
    ui.filterStartButton->hide();
    lastFilterString = ui.filterPatternLineEdit->text();

    FilterItems();
}

// This macro make the search expand all items that contain the searched text.
// A bug however, makes RS expand everything when nothing is selected, which is a pain.

#define EXPAND_WHILE_SEARCHING 1

static void recursMakeAllVisible(QTreeView *tree,const QModelIndex& indx)
{
	tree->setRowHidden(indx.row(), indx.parent(), false) ;

	int rowCount = tree->model()->rowCount(indx);

	for (int row = 0; row < rowCount; ++row)
	{
		QModelIndex child_index = indx.child(row,0);

		recursMakeAllVisible(tree,child_index);
	}
#ifdef EXPAND_WHILE_SEARCHING
	tree->setExpanded(indx,false) ;
#endif
}

//#define DEBUG_SHARED_FILES_DIALOG

static void recursMakeVisible(QTreeView *tree,const QSortFilterProxyModel *proxyModel,const QModelIndex& indx,uint32_t depth,const std::vector<std::set<void*> >& pointers)
{
#ifdef DEBUG_SHARED_FILES_DIALOG
	for(uint32_t i=0;i<depth;++i) std::cerr << "  " ; std::cerr << "depth " << depth << ": current ref=" << proxyModel->mapToSource(indx).internalPointer() << std::endl;
#endif
	int rowCount = tree->model()->rowCount(indx);
	const std::set<void*>& ptrs(pointers[depth+1]) ;

#ifdef DEBUG_SHARED_FILES_DIALOG
	std::cerr << "Pointers are: " << std::endl;
	for(auto it(ptrs.begin());it!=ptrs.end();++it)
		std::cerr << *it << std::endl;
#endif
	tree->setRowHidden(indx.row(), indx.parent(), false) ;
#ifdef EXPAND_WHILE_SEARCHING
	tree->setExpanded(indx,true) ;
#endif

	bool found = false ;

    for (int row = 0; row < rowCount; ++row)
	{
		QModelIndex child_index = indx.child(row,0);

		if(ptrs.find(proxyModel->mapToSource(child_index).internalPointer()) != ptrs.end())
		{
#ifdef DEBUG_SHARED_FILES_DIALOG
			for(uint32_t i=0;i<depth+1;++i) std::cerr << "  " ;	std::cerr << "object " << proxyModel->mapToSource(child_index).internalPointer() << " visible" << std::endl;
#endif
			recursMakeVisible(tree,proxyModel,child_index,depth+1,pointers) ;
			found = true ;
		}
		else
		{
			tree->setRowHidden(child_index.row(), indx, true) ;
#ifdef EXPAND_WHILE_SEARCHING
			tree->setExpanded(child_index,false) ;
#endif

#ifdef DEBUG_SHARED_FILES_DIALOG
			for(uint32_t i=0;i<depth+1;++i) std::cerr << "  " ;	std::cerr << "object " << proxyModel->mapToSource(child_index).internalPointer() << " hidden" << std::endl;
#endif
		}
	}

	if(!found && depth == 0)
		tree->setRowHidden(indx.row(), indx.parent(), true) ;
}

void SharedFilesDialog::FilterItems()
{
#ifdef DONT_USE_SEARCH_IN_TREE_VIEW
    if(proxyModel == tree_proxyModel)
        return;
#endif

	setCursor(Qt::WaitCursor);
	ui.dirTreeView->blockSignals(true) ;

    QString text = ui.filterPatternLineEdit->text();

	if(proxyModel == tree_proxyModel)
	{
		QCoreApplication::processEvents() ;

		std::list<std::string> keywords ;
		std::list<DirDetails> result_list ;

		if(text == "")
		{
			int rowCount = ui.dirTreeView->model()->rowCount();

			for (int row = 0; row < rowCount; ++row)
				recursMakeAllVisible(ui.dirTreeView,ui.dirTreeView->model()->index(row, COLUMN_NAME)) ;

			setCursor(Qt::ArrowCursor);
			return ;
		}

		if(text.length() < 3)
			return ;

		FileSearchFlags flags = isRemote()?RS_FILE_HINTS_REMOTE:RS_FILE_HINTS_LOCAL;
		QStringList lst = text.split(" ",QString::SkipEmptyParts) ;

		for(auto it(lst.begin());it!=lst.end();++it)
			keywords.push_back((*it).toStdString());

		if(keywords.size() > 1)
		{
			RsRegularExpression::NameExpression exp(RsRegularExpression::ContainsAllStrings,keywords,true);
			rsFiles->SearchBoolExp(&exp,result_list, flags) ;
		}
		else
			rsFiles->SearchKeywords(keywords,result_list, flags) ;

#ifdef DEBUG_SHARED_FILES_DIALOG
		std::cerr << "Found " << result_list.size() << " results" << std::endl;
#endif

		if(result_list.size() > MAX_SEARCH_RESULTS)
			return ;
#ifdef DEBUG_SHARED_FILES_DIALOG
		std::cerr << "Found this result: " << std::endl;
#endif
		std::vector<std::set<void*> > pointers(2,std::set<void*>());	// at least two levels need to be here.

		// Then show only the ones we need
		for(auto it(result_list.begin());it!=result_list.end();++it)
		{
#ifdef DEBUG_SHARED_FILES_DIALOG
			std::cerr << (void*)(*it).ref << "  parents: " ;
#endif

			DirDetails& det(*it) ;
			void *p = NULL;
			std::list<void*> lst ;

			lst.push_back(det.ref) ;

			while(det.type == DIR_TYPE_FILE || det.type == DIR_TYPE_DIR)
			{
				p = det.parent ;
				rsFiles->RequestDirDetails( p, det, flags);

#ifdef DEBUG_SHARED_FILES_DIALOG
				std::cerr << " " << (void*)p << "(" << (int)det.type << ")";
#endif

				lst.push_front(p) ;
			}

#ifdef DEBUG_SHARED_FILES_DIALOG
			std::cerr << std::endl;
#endif

			uint32_t u=0;
			for(auto it2(lst.begin());it2!=lst.end();++it2,++u)
			{
				if(pointers.size() <= u)
					pointers.resize(u+5) ;

				pointers[u].insert(*it2) ;
			}
		}

		int rowCount = ui.dirTreeView->model()->rowCount();
		for (int row = 0; row < rowCount; ++row)
			recursMakeVisible(ui.dirTreeView,proxyModel,ui.dirTreeView->model()->index(row, COLUMN_NAME),0,pointers);
	}
	else
	{
		int rowCount = ui.dirTreeView->model()->rowCount();
		for (int row = 0; row < rowCount; ++row)
			flat_FilterItem(ui.dirTreeView->model()->index(row, COLUMN_NAME), text, 0);
	}

#ifdef DEPRECATED_CODE
    int rowCount = ui.dirTreeView->model()->rowCount();
    for (int row = 0; row < rowCount; ++row)
		 if(proxyModel == tree_proxyModel)
			 tree_FilterItem(ui.dirTreeView->model()->index(row, COLUMN_NAME), text, 0);
		 else
			 flat_FilterItem(ui.dirTreeView->model()->index(row, COLUMN_NAME), text, 0);
#endif

	ui.dirTreeView->blockSignals(false) ;
    setCursor(Qt::ArrowCursor);
}

bool SharedFilesDialog::flat_FilterItem(const QModelIndex &index, const QString &text, int /*level*/)
{
	if(index.data(RetroshareDirModel::FileNameRole).toString().contains(text, Qt::CaseInsensitive)) 
	{
		ui.dirTreeView->setRowHidden(index.row(), index.parent(), false);
		return false ;
	}
	else 
	{
		ui.dirTreeView->setRowHidden(index.row(), index.parent(), true);
		return true ;
	}
}

bool SharedFilesDialog::tree_FilterItem(const QModelIndex &index, const QString &text, int level)
{
    bool visible = true;

    if (text.isEmpty() == false) {
        // better use RetroshareDirModel::getType, but its slow enough
        if (/*index.parent().isValid()*/ level >= 1) {
            if (index.data(RetroshareDirModel::FileNameRole).toString().contains(text, Qt::CaseInsensitive) == false) {
                visible = false;
            }
        } else {
            visible = false;
        }
    }

    int visibleChildCount = 0;
    int rowCount = ui.dirTreeView->model()->rowCount(index);
    for (int row = 0; row < rowCount; ++row) {
        if (tree_FilterItem(ui.dirTreeView->model()->index(row, index.column(), index), text, level + 1)) {
            ++visibleChildCount;
        }
    }

    if (visible || visibleChildCount) {
        ui.dirTreeView->setRowHidden(index.row(), index.parent(), false);
    } else {
        ui.dirTreeView->setRowHidden(index.row(), index.parent(), true);
    }

    return (visible || visibleChildCount);
}

