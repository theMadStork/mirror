// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <QList>
#include <QObject>
#include <QRunnable>
#include <QString>

#include "common/thread.h"
#include "core/file_sys/registered_cache.h"
#include "frontend_common/play_time_manager.h"
#include "qt_common/config/uisettings.h"

namespace Core {
class System;
}

class GameListDir;
class GameListModel;
class QStandardItem;

namespace FileSys {
class NCA;
class VfsFilesystem;
} // namespace FileSys

/**
 * Asynchronous worker object for populating the game list.
 * Communicates with other threads through Qt's signal/slot system.
 */
class GameListWorker : public QObject, public QRunnable {
    Q_OBJECT

public:
    explicit GameListWorker(std::shared_ptr<FileSys::VfsFilesystem> vfs_,
                            FileSys::ManualContentProvider* provider_,
                            QVector<UISettings::GameDir>& game_dirs_,
                            const PlayTime::PlayTimeManager& play_time_manager_,
                            Core::System& system_);
    ~GameListWorker() override;

    /// Starts the processing of directory tree information.
    void run() override;

public:
    /**
     * Synchronously processes any events queued by the worker.
     *
     * AddDirEntry is called on the model for every discovered directory.
     * AddEntry is called on the model for every discovered program.
     * DonePopulating is called on the model when processing completes.
     */
    void ProcessEvents(GameListModel* model);

signals:
    void DataAvailable();

private:
    template <typename F>
    void RecordEvent(F&& func);

private:
    void AddTitlesToGameList(GameListDir* parent_dir);

    enum class ScanTarget {
        FillManualContentProvider,
        PopulateGameList,
    };

    void ScanFileSystem(ScanTarget target, const std::string& dir_path, bool deep_scan,
                        GameListDir* parent_dir);

    std::shared_ptr<FileSys::VfsFilesystem> vfs;
    FileSys::ManualContentProvider* provider;
    QVector<UISettings::GameDir>& game_dirs;
    const PlayTime::PlayTimeManager& play_time_manager;

    QStringList watch_list;

    std::mutex lock;
    std::condition_variable cv;
    std::deque<std::function<void(GameListModel*)>> queued_events;
    std::atomic_bool stop_requested = false;
    Common::Event processing_completed;

    Core::System& system;

    // Path-based metadata cache keyed on file path + mtime + size. Unchanged files are
    // served from the cache without opening the ROM — important for game libraries on
    // network drives or sync-managed folders, where every file open transfers header
    // data or triggers sync activity. Gated on the cache_game_list setting.
    struct PathCacheGame {
        std::uint64_t program_id{};
        std::string name;
        std::string file_type;
        std::vector<std::uint8_t> icon;
    };

    struct PathCacheEntry {
        std::int64_t mtime{};
        std::uint64_t size{};
        std::vector<PathCacheGame> games;
    };

    void LoadPathCache();
    void SavePathCache();

    std::unordered_map<std::string, PathCacheEntry> path_cache;
    bool path_cache_dirty{false};
};
