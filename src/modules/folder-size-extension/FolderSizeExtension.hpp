#pragma once
/**
 * @file FolderSizeExtension.hpp
 * @brief Core declarations for the PowerToys Folder Size Explorer Extension.
 * 
 * This file contains the primary components for calculating folder sizes asynchronously
 * in the background. It is designed to be easily integrated into the main PowerToys codebase.
 * It includes settings management, a caching layer, a file system watcher to invalidate 
 * stale cache entries, and the main service orchestrator.
 */

#include <string>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <shobjidl.h>
#include <vector>
#include <windows.h>
#include <thread>
#include <atomic>
#include <memory>
#include <future>

/**
 * @struct FolderSizeSettings
 * @brief Represents the configuration settings for the extension.
 */
struct FolderSizeSettings {
    bool enabled = true;          // Master toggle for the extension
    bool useIECUnits = false;     // true = KiB/MiB, false = KB/MB (standard Windows behavior)
    bool includeHidden = true;    // Whether to include hidden/system files in size calculations
};

/**
 * @class SettingsManager
 * @brief Singleton class to load and manage extension settings.
 * 
 * TODO [Merge Consideration]: Integrate this with the PowerToys Settings JSON 
 * infrastructure (e.g., %localappdata%\Microsoft\PowerToys\FolderSize\settings.json).
 */
class SettingsManager {
public:
    static SettingsManager& GetInstance();
    FolderSizeSettings GetSettings() const;
    void ReloadSettings();

private:
    SettingsManager();
    ~SettingsManager() = default;
    
    FolderSizeSettings m_settings;
};

/**
 * @class CacheManager
 * @brief Thread-safe cache layer for storing folder sizes.
 * 
 * TODO [Merge Consideration]: This is currently an in-memory map. Replace it 
 * with a SQLite database backend to persist sizes across Explorer restarts.
 */
class CacheManager {
public:
    CacheManager(const std::wstring& dbPath);
    ~CacheManager();

    std::optional<uint64_t> GetSize(const std::wstring& path);
    void SetSize(const std::wstring& path, uint64_t size);
    void Invalidate(const std::wstring& path);

private:
    std::unordered_map<std::wstring, uint64_t> m_cache;
    std::shared_mutex m_mutex; // Using shared_mutex for reader-writer locking
    std::wstring m_dbPath;
};

/**
 * @class FileSystemWatcher
 * @brief Watches for file system changes to invalidate stale folder sizes.
 * 
 * TODO [Merge Consideration]: Replace the dummy loop in WorkerThread with 
 * ReadDirectoryChangesW or the NTFS USN Journal for efficient file system monitoring.
 */
class FileSystemWatcher {
public:
    FileSystemWatcher();
    ~FileSystemWatcher();

    void StartWatching(const std::vector<std::wstring>& driveLetters);
    void StopWatching();

    using OnChangeCallback = void(*)(const std::wstring& path);
    void SetCallback(OnChangeCallback callback);

private:
    void WorkerThread(std::wstring drive);
    
    OnChangeCallback m_callback = nullptr;
    std::atomic<bool> m_running;
    std::vector<std::thread> m_workers;
};

/**
 * @class FolderSizeService
 * @brief The main orchestrator connecting the UI/Column to the background thread.
 * 
 * This service receives requests for folder sizes. If the size is cached, it 
 * returns immediately. Otherwise, it spawns a detached background thread to compute 
 * the size and returns a placeholder string.
 */
class FolderSizeService {
public:
    FolderSizeService();
    ~FolderSizeService();

    // Returns a formatted size (e.g. "1.5 MB") or "Calculating..." if not ready.
    std::wstring GetFormattedFolderSize(const std::wstring& path);
    void RequestFolderSizeCalculation(const std::wstring& path);

private:
    uint64_t CalculateDirectorySizeImpl(const std::wstring& path);
    std::wstring FormatSize(uint64_t bytes);

    std::unique_ptr<CacheManager> m_cache;
    std::unique_ptr<FileSystemWatcher> m_watcher;
};
