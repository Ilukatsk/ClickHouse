#pragma once

#include <Databases/DatabaseOrdinary.h>
#include <Databases/DatabaseFactory.h>
#include <Databases/Git/GitWorker.h>
#include <Common/logger_useful.h>
#include <filesystem>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>

namespace DB
{

class DatabaseGit : public DatabaseOrdinary
{
public:
    DatabaseGit(
        String name_,
        String metadata_path_,
        const String & git_logger_name,
        ContextPtr context_,
        DatabaseMetadataDiskSettings database_metadata_disk_settings_,
        const String & git_repository_path = "",
        const String & git_remote_url = "");

    DatabaseGit(
        String name_, 
        String metadata_path_, 
        ContextPtr context_, 
        DatabaseMetadataDiskSettings database_metadata_disk_settings_,
        const String & git_repository_path = "",
        const String & git_remote_url = "");

    ~DatabaseGit() override;

    void commitCreateTable(const ASTCreateQuery & query, const StoragePtr & table,
                          const String & table_metadata_tmp_path, const String & table_metadata_path,
                          ContextPtr query_context) override;

    void commitAlterTable(const StorageID & table_id, const String & table_metadata_tmp_path,
                        const String & table_metadata_path, const String & statement,
                        ContextPtr query_context) override;

    void dropTable(ContextPtr query_context, const String & table_name, bool sync) override;

    void renameTable(ContextPtr query_context, const String & table_name, IDatabase & to_database,
                    const String & to_table_name, bool exchange, bool dictionary) override;
    
    void startAutoPull(UInt64 interval_seconds);
    void stopAutoPull();
    bool isAutoPullActive() const;

private:
    void commitChanges(const String & message);
    void pullFromRemote();

    // Member variables
    std::unique_ptr<GitWorker> git_worker;
    
    // Threading and synchronization
    mutable std::mutex git_operations_mutex;
    std::atomic<bool> auto_pull_active{false};
    std::atomic<bool> should_stop_auto_pull{false};
    std::atomic<UInt64> pull_interval_seconds{0};
    std::unique_ptr<std::thread> auto_pull_thread;
    
    Poco::Logger * git_log;
};

void registerDatabaseGit(DatabaseFactory & factory);

}
