#include "cpanel.h"
#include "settings/csettings.h"
#include "settings.h"
#include "filesystemhelperfunctions.h"
#include "directoryscanner.h"
#include "assert/advanced_assert.h"
#include "filesystemwatcher/cfilesystemwatcher.h"

DISABLE_COMPILER_WARNINGS
#include <QDebug>
#include <QVector>
RESTORE_COMPILER_WARNINGS

#include <time.h>
#include <limits>

#define exec_on_UI_thread _uiThreadQueue.enqueue

enum {
	ContentsChangedNotificationTag,
	ItemDiscoveryProgressNotificationTag
};

CPanel::CPanel(Panel position) :
	_watcher(std::make_shared<CFileSystemWatcher>()),
	_panelPosition(position),
	_workerThreadPool(4, std::string(position == LeftPanel ? "Left panel" : "Right panel") + " file list refresh thread pool")
{
	// The list of items in the current folder is being refreshed asynchronously, not every time a change is detected, to avoid refresh tasks queuing up out of control
	_fileListRefreshTimer.start(200);
	connect(&_fileListRefreshTimer, &QTimer::timeout, this, &CPanel::processContentsChangedEvent);

	_watcher->addCallback([this](const transparent_set<QFileInfo>&, const transparent_set<QFileInfo>&, const transparent_set<QFileInfo>&) {
		contentsChanged();
	});
}

void CPanel::restoreFromSettings()
{
	CSettings s;
	const QStringList historyList = s.value(_panelPosition == RightPanel ? KEY_HISTORY_R : KEY_HISTORY_L).toStringList();
	_history.addLatest(historyList.toVector().toStdVector());
	setPath(s.value(_panelPosition == LeftPanel ? KEY_LPANEL_PATH : KEY_RPANEL_PATH, QDir::homePath()).toString(), refreshCauseOther);
}

FileOperationResultCode CPanel::setPath(const QString &path, FileListRefreshCause operation)
{
#if defined __linux__ || defined __APPLE__
	const QString posixPath = path.contains("~") ? QString(path).replace("~", getenv("HOME")) : path;
#elif defined _WIN32
	assert(!path.contains('\\'));
	const QString posixPath = path;
#else
#error "Not implemented"
#endif

	_currentDisplayMode = NormalMode;

	std::unique_lock<std::recursive_mutex> locker(_fileListAndCurrentDirMutex);

	const QString oldPath = _currentDirObject.fullAbsolutePath();
	const auto pathGraph = CFileSystemObject::pathHierarchy(posixPath);
	bool pathSet = false;
	for (const auto& candidatePath: pathGraph)
	{
		if (pathIsAccessible(candidatePath))
		{
			_currentDirObject.setPath(candidatePath);
			pathSet = true;
			break;
		}
	}

	if (!pathSet)
	{
		if (pathIsAccessible(oldPath))
			_currentDirObject.setPath(oldPath);
		else
		{
			QString pathToSet;
			for (auto it = history().rbegin() + (history().size() - 1 - history().currentIndex()); it != history().rend(); ++it)
			{
				if (pathIsAccessible(*it))
				{
					pathToSet = *it;
					break;
				}
			}

			if (pathToSet.isEmpty())
				pathToSet = QDir::homePath();
			_currentDirObject.setPath(pathToSet);
		}
	}

	const QString newPath = _currentDirObject.fullAbsolutePath();

	// History management
	assert(!_history.currentItem().contains('\\') && !newPath.contains('\\'));
	if (_history.empty())
		// The current folder does not automatically make it into history on program startup, but it should (#103)
		_history.addLatest(oldPath);

	if (_history.currentItem() != newPath)
	{
		_history.addLatest(newPath);
		CSettings().setValue(_panelPosition == RightPanel ? KEY_HISTORY_R : KEY_HISTORY_L, QVariant(QStringList::fromVector(QVector<QString>::fromStdVector(_history.list()))));
	}

	CSettings().setValue(_panelPosition == LeftPanel ? KEY_LPANEL_PATH : KEY_RPANEL_PATH, newPath);

	if (_watcher->setPathToWatch(newPath) == false)
		qInfo() << __FUNCTION__ << "Error setting path" << newPath << "to CFileSystemWatcher";

	// If the new folder is one of the subfolders of the previous folder, mark it as the current for that previous folder
	// We're using the fact that _currentDirObject is already updated, but the _items list is not as it still corresponds to the previous location
	const auto newItemInPreviousFolder = _items.find(_currentDirObject.hash());
	if (operation != refreshCauseCdUp && newItemInPreviousFolder != _items.end() && newItemInPreviousFolder->second.parentDirPath() != newItemInPreviousFolder->second.fullAbsolutePath())
		// Updating the cursor when navigating downwards
		setCurrentItemForFolder(newItemInPreviousFolder->second.parentDirPath(), _currentDirObject.hash());
	else if (operation == refreshCauseCdUp)
		// Updating the cursor when navigating upwards
		setCurrentItemForFolder(_currentDirObject.fullAbsolutePath() /* where we are */, CFileSystemObject(oldPath).hash() /* where we were */);

	locker.unlock();

	refreshFileList(pathSet ? operation : refreshCauseOther);
	return pathSet ? rcOk : rcDirNotAccessible;
}

// Navigates up the directory tree
void CPanel::navigateUp()
{
	if (_currentDisplayMode != NormalMode)
		setPath(currentDirPathPosix(), refreshCauseOther);
	else
	{
		QDir tmpDir(currentDirPathPosix());
		if (tmpDir.cdUp())
			setPath(tmpDir.absolutePath(), refreshCauseCdUp);
		else
			sendContentsChangedNotification(refreshCauseOther);
	}
}

// Go to the previous location from history
bool CPanel::navigateBack()
{
	if (_currentDisplayMode != NormalMode)
		return setPath(currentDirPathPosix(), refreshCauseOther) == rcOk;
	else if (!_history.empty())
		return setPath(_history.navigateBack(), refreshCauseOther) == rcOk;
	else
		return false;
}

// Go to the next location from history, if any
bool CPanel::navigateForward()
{
	if (_currentDisplayMode == NormalMode && !_history.empty())
		return setPath(_history.navigateForward(), refreshCauseOther) == rcOk;
	return false;
}

const CHistoryList<QString>& CPanel::history() const
{
	return _history;
}

void CPanel::showAllFilesFromCurrentFolderAndBelow()
{
	_currentDisplayMode = AllObjectsMode;
	_watcher.reset();

	_workerThreadPool.enqueue([this]() {
		std::unique_lock<std::recursive_mutex> locker(_fileListAndCurrentDirMutex);
		const QString path = _currentDirObject.fullAbsolutePath();

		_items.clear();

		//locker.unlock();
		// TODO: synchronization and lock-ups
		const bool showHiddenFiles = CSettings().value(KEY_INTERFACE_SHOW_HIDDEN_FILES, true).toBool();
		scanDirectory(CFileSystemObject(path), [showHiddenFiles, this](const CFileSystemObject& item) {
			if (item.isFile() && item.exists() && (showHiddenFiles || !item.isHidden()))
				_items[item.hash()] = item;
		});
		//locker.lock();

		sendContentsChangedNotification(refreshCauseOther);
	});
}

// Switches to the appropriate directory and sets the cursor to the specified item
bool CPanel::goToItem(const CFileSystemObject& item)
{
	if (!item.exists())
		return false;

	const QString dir = item.parentDirPath();
	setCurrentItemForFolder(dir, item.hash());
	return setPath(dir, refreshCauseOther) == rcOk;
}

CFileSystemObject CPanel::currentDirObject() const
{
	return _currentDirObject;
}

// Info on the dir this panel is currently set to
QString CPanel::currentDirPathNative() const
{
	std::lock_guard<std::recursive_mutex> locker(_fileListAndCurrentDirMutex);
	return toNativeSeparators(_currentDirObject.fullAbsolutePath());
}

QString CPanel::currentDirPathPosix() const
{
	std::lock_guard<std::recursive_mutex> locker(_fileListAndCurrentDirMutex);
	return _currentDirObject.fullAbsolutePath();
}

QString CPanel::currentDirName() const
{
	std::lock_guard<std::recursive_mutex> locker(_fileListAndCurrentDirMutex);
	const QString name = _currentDirObject.fullName();
	return !name.isEmpty() ? name : _currentDirObject.fullAbsolutePath();
}

inline QString normalizeFolderPath(const QString& path)
{
	const QString posixPath = cleanPath(path);
	return posixPath.endsWith('/') ? posixPath : (posixPath + '/');
}

void CPanel::setCurrentItemForFolder(const QString& dir, qulonglong currentItemHash)
{
	assert(!dir.contains('\\'));
	_cursorPosForFolder[normalizeFolderPath(dir)] = currentItemHash;
}

qulonglong CPanel::currentItemForFolder(const QString &dir) const
{
	assert(!dir.contains('\\'));
	const auto it = _cursorPosForFolder.find(normalizeFolderPath(dir));
	return it == _cursorPosForFolder.end() ? 0 : it->second;
}

// Enumerates objects in the current directory
void CPanel::refreshFileList(FileListRefreshCause operation)
{
	_workerThreadPool.enqueue([this, operation]() {
		QFileInfoList list;

		{
			std::lock_guard<std::recursive_mutex> locker(_fileListAndCurrentDirMutex);

			if (!pathIsAccessible(_currentDirObject.fullAbsolutePath()))
			{
				setPath(_currentDirObject.fullAbsolutePath(), operation); // setPath will itself find the closest best folder to set instead
				return;
			}

			list = _currentDirObject.qDir().entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDot | QDir::Hidden | QDir::System);
			_items.clear();
		}

		const bool showHiddenFiles = CSettings().value(KEY_INTERFACE_SHOW_HIDDEN_FILES, true).toBool();
		std::vector<CFileSystemObject> objectsList;

		const size_t numItemsFound = (size_t)list.size();
		objectsList.reserve(numItemsFound);

		for (size_t i = 0; i < numItemsFound; ++i)
		{
#ifndef _WIN32
			if (list[(int)i].absoluteFilePath() == QLatin1String("/.."))
				continue;
#endif
			objectsList.emplace_back(list[(int)i]);
			sendItemDiscoveryProgressNotification(_currentDirObject.hash(), 20 + 80 * i / numItemsFound, _currentDirObject.fullAbsolutePath());
		}

		{
			std::lock_guard<std::recursive_mutex> locker(_fileListAndCurrentDirMutex);

			for (const auto& object : objectsList)
			{
				if (object.exists() && (showHiddenFiles || !object.isHidden()))
					_items[object.hash()] = object;
			}
		}

		sendContentsChangedNotification(operation);
	});
}

// Returns the current list of objects on this panel
std::map<qulonglong, CFileSystemObject> CPanel::list() const
{
	std::lock_guard<std::recursive_mutex> locker(_fileListAndCurrentDirMutex);
	return _items;
}

bool CPanel::itemHashExists(const qulonglong hash) const
{
	std::lock_guard<std::recursive_mutex> locker(_fileListAndCurrentDirMutex);
	return _items.count(hash) > 0;
}

CFileSystemObject CPanel::itemByHash(qulonglong hash) const
{
	std::lock_guard<std::recursive_mutex> locker(_fileListAndCurrentDirMutex);

	const auto it = _items.find(hash);
	return it != _items.end() ? it->second : CFileSystemObject();
}

// Calculates total size for the specified objects
FilesystemObjectsStatistics CPanel::calculateStatistics(const std::vector<qulonglong>& hashes)
{
	if (hashes.empty())
		return FilesystemObjectsStatistics();

	FilesystemObjectsStatistics stats;
	for(const auto hash: hashes)
	{
		const CFileSystemObject rootItem = itemByHash(hash);
		if (rootItem.isDir())
		{
			++stats.folders;
			scanDirectory(rootItem, [this, &stats](const CFileSystemObject& discoveredItem) {
				if (discoveredItem.isFile())
				{
					stats.occupiedSpace += discoveredItem.size();
					++stats.files;
				}
				else if (discoveredItem.isDir())
					++stats.folders;

				sendItemDiscoveryProgressNotification(0, std::numeric_limits<size_t>::max(), discoveredItem.fullAbsolutePath());
			});
		}
		else if (rootItem.isFile())
		{
			++stats.files;
			stats.occupiedSpace += rootItem.size();
		}
	}

	return stats;
}

// Calculates directory size, stores it in the corresponding CFileSystemObject and sends data change notification
void CPanel::displayDirSize(qulonglong dirHash)
{
	_workerThreadPool.enqueue([this, dirHash] {
		std::unique_lock<std::recursive_mutex> locker(_fileListAndCurrentDirMutex);

		auto it = _items.find(dirHash);
		assert_and_return_r(it != _items.end(), );

		if (it->second.isDir())
		{
			locker.unlock(); // Without this .unlock() the UI thread will get blocked very easily
			const FilesystemObjectsStatistics stats = calculateStatistics(std::vector<qulonglong>(1, dirHash));
			locker.lock();
			// Since we unlocked the mutex, the item we were working on may well be out of the _items list by now
			// So we find it again and see if it's still there
			it = _items.find(dirHash);
			if (it == _items.end())
				return;

			it->second.setDirSize(stats.occupiedSpace);
			sendContentsChangedNotification(refreshCauseOther);
		}
	});
}

void CPanel::sendContentsChangedNotification(FileListRefreshCause operation) const
{
	exec_on_UI_thread([this, operation]() {
		for (auto listener : _panelContentsChangedListeners)
			listener->panelContentsChanged(_panelPosition, operation);
	}, ContentsChangedNotificationTag);
}

// progress > 100 means indefinite
void CPanel::sendItemDiscoveryProgressNotification(qulonglong itemHash, size_t progress, const QString& currentDir) const
{
	exec_on_UI_thread([=]() {
		for (auto listener : _panelContentsChangedListeners)
			listener->itemDiscoveryInProgress(_panelPosition, itemHash, progress, currentDir);
	}, ItemDiscoveryProgressNotificationTag);
}

void CPanel::volumesChanged(const std::deque<VolumeInfo>& volumes)
{
	_volumes = volumes;

	// Handling an unplugged device
	if (_currentDirObject.isValid() && !volumeInfoForObject(_currentDirObject).isReady)
		setPath(_currentDirObject.fullAbsolutePath(), refreshCauseOther);
}

// Settings have changed
void CPanel::settingsChanged()
{
}

void CPanel::uiThreadTimerTick()
{
	_uiThreadQueue.exec();
}

void CPanel::contentsChanged()
{
	// The list of items in the current folder is being refreshed asynchronously, not every time a change is detected, to avoid refresh tasks queuing up out of control
	_bContentsChangedEventPending = true;
}

void CPanel::addPanelContentsChangedListener(PanelContentsChangedListener *listener)
{
	assert_r(std::find(_panelContentsChangedListeners.begin(), _panelContentsChangedListeners.end(), listener) == _panelContentsChangedListeners.end()); // Why would we want to set the same listener twice? That'd probably be a mistake.
	_panelContentsChangedListeners.push_back(listener);
}

const VolumeInfo& CPanel::volumeInfoForObject(const CFileSystemObject& object) const
{
	static const VolumeInfo dummy;

	const auto storage = std::find_if(_volumes.cbegin(), _volumes.cend(), [&object](const VolumeInfo& item) {return item.rootObjectInfo.rootFileSystemId() == object.rootFileSystemId();});
	return storage != _volumes.cend() ? *storage : dummy;
}

bool CPanel::pathIsAccessible(const QString& path) const
{
	const CFileSystemObject pathObject(path);
	const auto storageInfo = volumeInfoForObject(pathObject);
	if (!pathObject.exists() || !pathObject.isReadable() || (!pathObject.isNetworkObject() && !storageInfo.isReady))
		return false;

#ifdef _WIN32
	if (storageInfo.rootObjectInfo.fullAbsolutePath() == pathObject.fullAbsolutePath())
		return true; // On Windows, a drive root (e. g. C:\) doesn't produce '.' in the entryList, so the list is empty, but it's not an error
#endif // _WIN32

	return !pathObject.qDir().entryList().empty();
}

void CPanel::processContentsChangedEvent()
{
	if (_bContentsChangedEventPending)
	{
		refreshFileList(refreshCauseOther);
		_bContentsChangedEventPending = false;
	}
}
