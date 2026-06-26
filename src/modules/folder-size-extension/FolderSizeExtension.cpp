/**
 * @file FolderSizeExtension.cpp
 * @brief Core implementations for the PowerToys Folder Size Explorer Extension.
 */

#include "FolderSizeExtension.hpp"
#include <mutex>
#include <iostream>
#include <format>
#include <propkey.h>
#include <propvarutil.h>
#include <pch.h>
// ============================================================================
// SettingsManager Implementation
// ============================================================================

SettingsManager& SettingsManager::GetInstance() {
    static SettingsManager instance;
    return instance;
}

SettingsManager::SettingsManager() {
    ReloadSettings();
}

FolderSizeSettings SettingsManager::GetSettings() const {
    return m_settings;
}

void SettingsManager::ReloadSettings() {
    // TODO [Merge Consideration]: Hook this into PowerToys settings IPC/JSON files.
    m_settings.enabled = true;
    m_settings.useIECUnits = false;
    m_settings.includeHidden = true;
}

// ============================================================================
// CacheManager Implementation
// ============================================================================

CacheManager::CacheManager(const std::wstring& dbPath) : m_dbPath(dbPath) {
    // TODO [Merge Consideration]: Initialize SQLite DB connection here.
}

CacheManager::~CacheManager() {
    // TODO [Merge Consideration]: Close SQLite DB connection here.
}

std::optional<uint64_t> CacheManager::GetSize(const std::wstring& path) {
    // Use shared_lock (read lock) to allow multiple concurrent reads.
    std::shared_lock lock(m_mutex);
    auto it = m_cache.find(path);
    if (it != m_cache.end()) {
        return it->second;
    }
    return std::nullopt;
}

void CacheManager::SetSize(const std::wstring& path, uint64_t size) {
    // Use unique_lock (write lock) as we are mutating the map.
    std::unique_lock lock(m_mutex);
    m_cache[path] = size;
    // TODO [Merge Consideration]: Persist to SQLite.
}

void CacheManager::Invalidate(const std::wstring& path) {
    // Use unique_lock (write lock) as we are mutating the map.
    std::unique_lock lock(m_mutex);
    m_cache.erase(path);
    // TODO [Merge Consideration]: Remove from SQLite.
}

// ============================================================================
// FileSystemWatcher Implementation
// ============================================================================

FileSystemWatcher::FileSystemWatcher() : m_running(false) {}

FileSystemWatcher::~FileSystemWatcher() {
    StopWatching();
}

void FileSystemWatcher::StartWatching(const std::vector<std::wstring>& driveLetters) {
    if (m_running) return;
    m_running = true;
    
    // Spawn a worker thread for each drive we are watching.
    for (const auto& drive : driveLetters) {
        m_workers.emplace_back(&FileSystemWatcher::WorkerThread, this, drive);
    }
}

void FileSystemWatcher::StopWatching() {
    m_running = false;
    // Gracefully wait for all watcher threads to finish.
    for (auto& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    m_workers.clear();
}

void FileSystemWatcher::SetCallback(OnChangeCallback callback) {
    m_callback = callback;
}

void FileSystemWatcher::WorkerThread(std::wstring drive) {
    // Dummy loop. 
    // TODO [Merge Consideration]: Use ReadDirectoryChangesW blocking call here.
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

// ============================================================================
// FolderSizeService Implementation
// ============================================================================

FolderSizeService::FolderSizeService() {
    m_cache = std::make_unique<CacheManager>(L"foldersize_cache.db");
    m_watcher = std::make_unique<FileSystemWatcher>();
    
    // TODO [Merge Consideration]: Enumerate actual logical drives instead of hardcoding C:.
    m_watcher->StartWatching({L"C:\\"}); 
}

FolderSizeService::~FolderSizeService() {
    m_watcher->StopWatching();
}

std::wstring FolderSizeService::GetFormattedFolderSize(const std::wstring& path) {
    auto sizeOpt = m_cache->GetSize(path);
    if (sizeOpt.has_value()) {
        // Return instantly if the cache has the value.
        return FormatSize(sizeOpt.value());
    }
    
    // Otherwise, start the calculation and return a placeholder immediately.
    RequestFolderSizeCalculation(path);
    return L"Calculating...";
}

void FolderSizeService::RequestFolderSizeCalculation(const std::wstring& path) {
    // Spawn an asynchronous detached thread so Explorer is never blocked.
    // TODO [Merge Consideration]: Consider a Thread Pool instead of detached threads if 
    // too many folders are opened simultaneously.
    std::thread([this, path]() {
        uint64_t size = CalculateDirectorySizeImpl(path);
        m_cache->SetSize(path, size);
    }).detach();
}

uint64_t FolderSizeService::CalculateDirectorySizeImpl(const std::wstring& path) {
    uint64_t totalSize = 0;
    WIN32_FIND_DATAW findData;
    std::wstring searchPath = path + L"\\*";
    
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) { return 0; }

    do {
        if (wcscmp(findData.cFileName, L".") != 0 && wcscmp(findData.cFileName, L"..") != 0) {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // Ignore reparse points (symlinks/junctions) to avoid infinite recursive loops.
                if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    totalSize += CalculateDirectorySizeImpl(path + L"\\" + findData.cFileName);
                }
            } else {
                ULARGE_INTEGER fileSize;
                fileSize.LowPart = findData.nFileSizeLow;
                fileSize.HighPart = findData.nFileSizeHigh;
                totalSize += fileSize.QuadPart;
            }
        }
    } while (FindNextFileW(hFind, &findData));

    FindClose(hFind);
    return totalSize;
}

std::wstring FolderSizeService::FormatSize(uint64_t bytes) {
    const auto settings = SettingsManager::GetInstance().GetSettings();
    const double base = settings.useIECUnits ? 1024.0 : 1000.0;
    
    const wchar_t* unitsSI[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    const wchar_t* unitsIEC[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    
    const wchar_t** units = settings.useIECUnits ? unitsIEC : unitsSI;
    
    int unitIndex = 0;
    double size = static_cast<double>(bytes);
    
    while (size >= base && unitIndex < 4) {
        size /= base;
        unitIndex++;
    }
    
    wchar_t buffer[64];
    if (unitIndex == 0) {
        swprintf_s(buffer, L"%llu %s", bytes, units[0]);
    } else {
        swprintf_s(buffer, L"%.2f %s", size, units[unitIndex]);
    }
    
    return std::wstring(buffer);
}
