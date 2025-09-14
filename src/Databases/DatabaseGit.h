#pragma once

#include <Databases/DatabaseOrdinary.h>
#include <Common/AsyncLoader.h>
#include <Common/logger_useful.h>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>
#include <atomic>
#include <thread>

namespace DB
{

namespace ErrorCodes
{
    extern const int GIT_OPERATION_FAILED;
    extern const int GIT_REPOSITORY_NOT_FOUND;
    extern const int GIT_CONFLICT_DETECTED;
}

/// DatabaseGit engine provides version control for database schema using Git.
/// It inherits from DatabaseOrdinary and adds Git synchronization functionality.
/// All schema changes are automatically committed to Git, and Git pull operations are wrapped in coroutines.
class DatabaseGit : public DatabaseOrdinary
{
public:
    DatabaseGit(
        String name_,
        String metadata_path_,
        const String & logger_name,
        ContextPtr context_,
        DatabaseMetadataDiskSettings database_metadata_disk_settings_ = {});

    DatabaseGit(
        String name_,
        String metadata_path_,
        ContextPtr context_,
        DatabaseMetadataDiskSettings database_metadata_disk_settings_ = {});

    ~DatabaseGit() override;

    String getEngineName() const override { return "Git"; }

    /// Initialize Git repository if it doesn't exist
    void initializeGitRepository(const String & remote_url = "", const String & branch = "main");

    /// Git-specific operations
    void gitPull(ContextPtr query_context);
    void gitPush(ContextPtr query_context, const String & message = "");
    void gitCommit(ContextPtr query_context, const String & message);
    void gitCheckout(ContextPtr query_context, const String & branch);
    void gitStatus(ContextPtr query_context);

    /// Get current Git information
    String getCurrentBranch() const;
    String getCurrentCommitHash() const;
    std::vector<String> getGitLog(ContextPtr query_context, int max_entries = 10);

    /// Schema versioning operations
    void createSchemaSnapshot(ContextPtr query_context, const String & tag_name = "");
    void restoreSchemaSnapshot(ContextPtr query_context, const String & commit_hash);
    std::vector<String> getSchemaHistory(ContextPtr query_context);

    /// Automatic Git pulling with intervals
    void startAutoPull(UInt64 interval_seconds = 300); // Default 5 minutes
    void stopAutoPull();
    bool isAutoPullActive() const;
    
    /// Git configuration setters
    void setGitRepositoryPath(const String & path);
    void setGitRemoteUrl(const String & url);
    void setGitBranch(const String & branch);

protected:
    /// Override key methods to add Git sync functionality
    void commitCreateTable(const ASTCreateQuery & query, const StoragePtr & table,
                          const String & table_metadata_tmp_path, const String & table_metadata_path,
                          ContextPtr query_context) override;

    void commitAlterTable(const StorageID & table_id, const String & table_metadata_tmp_path,
                         const String & table_metadata_path, const String & statement,
                         ContextPtr query_context) override;

    void dropTable(ContextPtr context, const String & table_name, bool sync) override;

    void renameTable(ContextPtr context, const String & table_name, IDatabase & to_database,
                    const String & to_table_name, bool exchange, bool dictionary) override;

private:
    /// Git repository management
    String git_repository_path;
    String git_remote_url;
    String git_branch;
    String current_commit_hash;
    mutable std::mutex git_operations_mutex;

    LoadTaskPtr current_git_task TSA_GUARDED_BY(git_operations_mutex);

    void executeGitCommand(const String & command, bool throw_on_error = true);
    String executeGitCommandWithResult(const String & command);
    bool isGitRepository() const;
    void ensureGitRepository();
    
    LoadJobPtr scheduleGitPull();
    void gitPullFromRemote();
    void gitPullFromRemoteUnlocked(); // Unlocked version for use when already holding mutex
    void reloadModifiedTables();
    
    std::unordered_map<String, String> table_versions TSA_GUARDED_BY(git_operations_mutex);
    
    std::atomic<bool> auto_pull_active{false};
    std::atomic<UInt64> pull_interval_seconds{10};
    std::unique_ptr<std::thread> auto_pull_thread;
    std::atomic<bool> should_stop_auto_pull{false};
    
    Poco::Logger * git_log;
};

}
