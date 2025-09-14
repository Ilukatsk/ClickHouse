#include <Databases/DatabaseGit.h>
#include <Databases/DatabaseFactory.h>
#include <Common/AsyncLoader.h>
#include <Common/Stopwatch.h>
#include <Common/ThreadPool.h>
#include <Common/PoolId.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTFunction.h>
#include <Storages/IStorage.h>
#include <filesystem>
#include <sstream>
#include <regex>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

namespace DB
{

namespace ErrorCodes
{
    extern const int GIT_REPOSITORY_NOT_FOUND;
    extern const int GIT_CONFLICT_DETECTED;
}

DatabaseGit::DatabaseGit(
    String name_,
    String metadata_path_,
    const String & git_logger_name,
    ContextPtr context_,
    DatabaseMetadataDiskSettings database_metadata_disk_settings_)
    : DatabaseOrdinary(name_, metadata_path_, "store/", git_logger_name, context_, database_metadata_disk_settings_)
    , git_repository_path(fs::path(metadata_path_).parent_path() / name_)
    , git_branch("main")
    , git_log(&Poco::Logger::get(git_logger_name))
{
}

DatabaseGit::DatabaseGit(
    String name_, String metadata_path_, ContextPtr context_, DatabaseMetadataDiskSettings database_metadata_disk_settings_)
    : DatabaseGit(name_, std::move(metadata_path_), "DatabaseGit (" + name_ + ")", context_, database_metadata_disk_settings_)
{
}

DatabaseGit::~DatabaseGit()
{
    stopAutoPull();
}

void DatabaseGit::initializeGitRepository(const String & remote_url, const String & branch)
{
    String db_name = getDatabaseName();
    
    std::lock_guard lock(git_operations_mutex);
    
    git_remote_url = remote_url;
    git_branch = branch;
    
    try
    {
        auto db_disk = getDisk();
        db_disk->createDirectories(git_repository_path);
        
        if (!isGitRepository())
        {
            executeGitCommand("git init");
            executeGitCommand("git config user.name 'ClickHouse DatabaseGit'");
            executeGitCommand("git config user.email 'clickhouse@localhost'");
            
            if (!remote_url.empty())
            {
                executeGitCommand("git remote add origin " + remote_url);
            }
            
            executeGitCommand("git add .");
            executeGitCommand("git commit -m 'Initial schema commit' || true"); // Allow empty commit
        }
        
        if (branch != "main")
        {
            executeGitCommand("git checkout -b " + branch + " || git checkout " + branch);
        }
        
        current_commit_hash = executeGitCommandWithResult("git rev-parse HEAD");
        
        LOG_INFO(git_log, "Initialized Git repository for database {} at {}", db_name, git_repository_path);
        
        if (!remote_url.empty())
        {
            String connectivity_test = executeGitCommandWithResult("timeout 15 git ls-remote origin 2>/dev/null || echo 'CONNECTION_FAILED'");
            if (!connectivity_test.empty() && connectivity_test.find("CONNECTION_FAILED") == String::npos && !connectivity_test.empty())
            {
                LOG_INFO(git_log, "Remote URL configured with commits. Starting automatic pulling with 5-minute interval");
                startAutoPull(300);
            }
            else
            {
                LOG_INFO(git_log, "Remote URL configured but repository is empty or unreachable. Auto-pull will start when repository has commits.");
            }
        }
    }
    catch (...)
    {
        LOG_ERROR(git_log, "Failed to initialize Git repository: {}", String(getCurrentExceptionMessageAndPattern(true)));
        throw;
    }
}

void DatabaseGit::ensureGitRepository()
{
    if (!isGitRepository())
    {
        String db_name = getDatabaseName();
        LOG_INFO(git_log, "Git repository not found, initializing for database {}", db_name);
        initializeGitRepository();
    }
}

bool DatabaseGit::isGitRepository() const
{
    try
    {
        auto db_disk = getDisk();
        return db_disk->existsFileOrDirectory(fs::path(git_repository_path) / ".git");
    }
    catch (...)
    {
        return false;
    }
}

void DatabaseGit::executeGitCommand(const String & command, bool throw_on_error)
{
    try
    {
        String full_command = "cd " + git_repository_path + " && " + command;
        
        int exit_code = system(full_command.c_str());
        if (exit_code != 0 && throw_on_error)
        {
            throw Exception(ErrorCodes::BAD_ARGUMENTS, 
                          "Git command failed with exit code {}: {}", exit_code, command);
        }
        
        LOG_DEBUG(git_log, "Executed Git command: {}", command);
    }
    catch (...)
    {
        if (throw_on_error)
        {
            LOG_ERROR(git_log, "Git command failed: {}", command);
            throw;
        }
    }
}

String DatabaseGit::executeGitCommandWithResult(const String & command)
{
    try
    {
        String full_command = "cd " + git_repository_path + " && " + command;
        
        FILE* pipe = popen(full_command.c_str(), "r");
        if (!pipe)
        {
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Failed to execute Git command: {}", command);
        }
        
        String result;
        char buffer[128];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
        {
            result += buffer;
        }
        
        int exit_code = pclose(pipe);
        if (exit_code != 0)
        {
            throw Exception(ErrorCodes::BAD_ARGUMENTS, 
                          "Git command failed with exit code {}: {}", exit_code, command);
        }
        
        if (!result.empty() && result.back() == '\n')
        {
            result.pop_back();
        }
        
        return result;
    }
    catch (...)
    {
        LOG_ERROR(git_log, "Git command with result failed: {}", command);
        throw;
    }
}

void DatabaseGit::commitCreateTable(const ASTCreateQuery & query, const StoragePtr & table,
                                   const String & table_metadata_tmp_path, const String & table_metadata_path,
                                   ContextPtr query_context)
{
    std::lock_guard lock(git_operations_mutex);
    
    try
    {
        if (!git_remote_url.empty())
        {
            try
            {
                LOG_INFO(git_log, "Pulling latest changes before creating table {}", query.getTable());
                gitPullFromRemoteUnlocked();
            }
            catch (...)
            {
                LOG_WARNING(git_log, "Failed to pull before creating table {}: {}", 
                           query.getTable(), String(getCurrentExceptionMessageAndPattern(true)));
            }
        }

        DatabaseOrdinary::commitCreateTable(query, table, table_metadata_tmp_path, table_metadata_path, query_context);
        
        if (isGitRepository())
        {
            try
            {
                String commit_message = "CREATE TABLE " + query.getTable();
                
                executeGitCommand("git add .", false);
                
                String commit_cmd = "git commit -m \"" + commit_message + "\" || true";
                executeGitCommand(commit_cmd, false);
                
                current_commit_hash = executeGitCommandWithResult("git rev-parse HEAD");
                
                table_versions[query.getTable()] = current_commit_hash;
                
                LOG_INFO(git_log, "Created table {} and committed to Git", query.getTable());
            }
            catch (...)
            {
                LOG_ERROR(git_log, "Git commit failed for CREATE TABLE {}: {}", query.getTable(), String(getCurrentExceptionMessageAndPattern(true)));
            }
        }
        else
        {
            LOG_INFO(git_log, "Git repository not initialized, skipping Git commit for table {}", query.getTable());
        }
    }
    catch (...)
    {
        LOG_ERROR(git_log, "Failed to create table {}: {}", 
                 query.getTable(), String(getCurrentExceptionMessageAndPattern(true)));
        throw;
    }
}

void DatabaseGit::commitAlterTable(const StorageID & table_id, const String & table_metadata_tmp_path,
                                  const String & table_metadata_path, const String & statement,
                                  ContextPtr query_context)
{
    std::lock_guard lock(git_operations_mutex);
    
    try
    {
        if (!git_remote_url.empty())
        {
            try
            {
                LOG_INFO(git_log, "Pulling latest changes before altering table {}", table_id.table_name);
                gitPullFromRemoteUnlocked();
            }
            catch (...)
            {
                LOG_WARNING(git_log, "Failed to pull before altering table {}: {}", 
                    table_id.table_name, String(getCurrentExceptionMessageAndPattern(true)));
            }
        }

        DatabaseOrdinary::commitAlterTable(table_id, table_metadata_tmp_path, table_metadata_path, statement, query_context);
        
        if (isGitRepository())
        {
            try
            {
                String commit_message = "ALTER TABLE " + table_id.table_name;

                executeGitCommand("git add .", false);
                
                String commit_cmd = "git commit -m \"" + commit_message + "\" || true";
                executeGitCommand(commit_cmd, false);
                
                current_commit_hash = executeGitCommandWithResult("git rev-parse HEAD");
                
                table_versions[table_id.table_name] = current_commit_hash;
                
                LOG_INFO(git_log, "Altered table {} and committed to Git", table_id.table_name);
            }
            catch (...)
            {
                LOG_ERROR(git_log, "Git commit failed for ALTER TABLE {}: {}", table_id.table_name, String(getCurrentExceptionMessageAndPattern(true)));

            }
        }
        else
        {
            LOG_INFO(git_log, "Git repository not initialized, skipping Git commit for table {}", table_id.table_name);
        }
    }
    catch (...)
    {
        LOG_ERROR(git_log, "Failed to alter table {}: {}", 
                 table_id.table_name, String(getCurrentExceptionMessageAndPattern(true)));
        throw;
    }
}

void DatabaseGit::dropTable(ContextPtr query_context, const String & table_name, bool sync)
{
    std::lock_guard lock(git_operations_mutex);
    
    try
    {
        if (!git_remote_url.empty())
        {
            try
            {
                LOG_INFO(git_log, "Pulling latest changes before dropping table {}", table_name);
                gitPullFromRemoteUnlocked();
            }
            catch (...)
            {
                LOG_WARNING(git_log, "Failed to pull before dropping table {}: {}", 
                           table_name, String(getCurrentExceptionMessageAndPattern(true)));
            }
        }

        DatabaseOrdinary::dropTable(query_context, table_name, sync);
        
        if (isGitRepository())
        {
            try
            {
                String commit_message = "DROP TABLE " + table_name;
                
                executeGitCommand("git add .", false); // Don't throw on error
                
                String commit_cmd = "git commit -m \"" + commit_message + "\" || true";
                executeGitCommand(commit_cmd, false);
                
                current_commit_hash = executeGitCommandWithResult("git rev-parse HEAD");
                
                table_versions.erase(table_name);
                
                LOG_INFO(git_log, "Dropped table {} and committed to Git", table_name);
            }
            catch (...)
            {
                LOG_ERROR(git_log, "Git commit failed for DROP TABLE {}: {}", table_name, String(getCurrentExceptionMessageAndPattern(true)));
            }
        }
        else
        {
            LOG_INFO(git_log, "Git repository not initialized, skipping Git commit for table {}", table_name);
            table_versions.erase(table_name);
        }
    }
    catch (...)
    {
        LOG_ERROR(git_log, "Failed to drop table {}: {}", 
                 table_name, String(getCurrentExceptionMessageAndPattern(true)));
        throw;
    }
}

void DatabaseGit::renameTable(ContextPtr query_context, const String & table_name, IDatabase & to_database,
                             const String & to_table_name, bool exchange, bool dictionary)
{
    std::lock_guard lock(git_operations_mutex);
    try
    {
        if (!git_remote_url.empty())
        {
            try
            {
                LOG_INFO(git_log, "Pulling latest changes before renaming table {} to {}", table_name, to_table_name);
                gitPullFromRemote();
            }
            catch (...)
            {
                LOG_WARNING(git_log, "Failed to pull before renaming table {}: {}", 
                        table_name, String(getCurrentExceptionMessageAndPattern(true)));
            }
        }


        DatabaseOrdinary::renameTable(query_context, table_name, to_database, to_table_name, exchange, dictionary);

        ensureGitRepository();
        
        String commit_message = "RENAME TABLE " + table_name + " TO " + to_table_name;

        executeGitCommand("git add .");
        
        String commit_cmd = "git commit -m \"" + commit_message + "\" || true";
        executeGitCommand(commit_cmd, false);
        
        current_commit_hash = executeGitCommandWithResult("git rev-parse HEAD");

        auto it = table_versions.find(table_name);
        if (it != table_versions.end())
        {
            table_versions[to_table_name] = it->second;
            table_versions.erase(it);
        }
        
        LOG_INFO(git_log, "Renamed table {} to {} and committed to Git", table_name, to_table_name);
    }
    catch (...)
    {
        LOG_ERROR(git_log, "Git commit failed for RENAME TABLE {} to {}: {}", table_name, to_table_name, String(getCurrentExceptionMessageAndPattern(true)));
    }
}


LoadJobPtr DatabaseGit::scheduleGitPull()
{
    String db_name = getDatabaseName();
    
    return makeLoadJob(
        LoadJobSet{},
        TablesLoaderBackgroundStartupPoolId,
        "git_pull_" + db_name,
        [this](AsyncLoader &, const LoadJobPtr &)
        {
            gitPullFromRemote();
        }
    );
}

void DatabaseGit::gitPullFromRemote()
{
    std::lock_guard lock(git_operations_mutex);
    gitPullFromRemoteUnlocked();
}

void DatabaseGit::gitPullFromRemoteUnlocked()
{
    try
    {
        if (git_remote_url.empty())
        {
            LOG_INFO(git_log, "No remote URL configured, skipping Git pull");
            return;
        }
        

        ensureGitRepository();

        String remote_check = executeGitCommandWithResult("timeout 10 git remote get-url origin 2>/dev/null || echo ''");
        if (remote_check.empty())
        {
            LOG_INFO(git_log, "No remote origin configured, skipping Git pull");
            return;
        }
        
        LOG_INFO(git_log, "Testing remote repository connectivity...");
        String connectivity_test = executeGitCommandWithResult("timeout 15 git ls-remote origin 2>/dev/null || echo 'CONNECTION_FAILED'");
        if (connectivity_test.empty() || connectivity_test.find("CONNECTION_FAILED") != String::npos)
        {
            LOG_WARNING(git_log, "Remote repository is not reachable, skipping Git pull");
            return;
        }
        
        String trimmed_test = connectivity_test;
        while (!trimmed_test.empty() && (trimmed_test[0] == ' ' || trimmed_test[0] == '\t' || trimmed_test[0] == '\n' || trimmed_test[0] == '\r'))
            trimmed_test = trimmed_test.substr(1);
        while (!trimmed_test.empty() && (trimmed_test.back() == ' ' || trimmed_test.back() == '\t' || trimmed_test.back() == '\n' || trimmed_test.back() == '\r'))
            trimmed_test.pop_back();
            
        if (trimmed_test.empty())
        {
            LOG_INFO(git_log, "Remote repository is empty (no commits), skipping Git pull");
            return;
        }
        
        LOG_INFO(git_log, "Fetching latest changes from remote...");
        executeGitCommand("timeout 30 git fetch origin", false);
        
        // Check for conflicts before pulling
        String status = executeGitCommandWithResult("git status --porcelain");
        if (!status.empty())
        {
            LOG_WARNING(git_log, "Local changes detected, cannot pull safely");
            return;
        }
        
        LOG_INFO(git_log, "Pulling changes from remote...");
        executeGitCommand("timeout 30 git pull origin " + git_branch, false);
        
        reloadModifiedTables();
        
        current_commit_hash = executeGitCommandWithResult("git rev-parse HEAD");
        
        LOG_INFO(git_log, "Git pull completed successfully");
    }
    catch (...)
    {
        LOG_ERROR(git_log, "Git pull failed: {}", String(getCurrentExceptionMessageAndPattern(true)));
    }
}

void DatabaseGit::reloadModifiedTables()
{
    try
    {
        String modified_files = executeGitCommandWithResult("git diff --name-only HEAD~1 HEAD");
        
        if (modified_files.empty())
        {
            LOG_DEBUG(git_log, "No modified tables to reload");
            return;
        }
        
        std::istringstream stream(modified_files);
        String line;
        while (std::getline(stream, line))
        {
            if (line.ends_with(".sql"))
            {
                // Extract table name from file path
                fs::path file_path(line);
                String table_name = file_path.stem().string();
                
                LOG_INFO(git_log, "Reloading modified table: {}", table_name);
                
            }
        }
    }
    catch (...)
    {
        LOG_ERROR(git_log, "Failed to reload modified tables: {}", String(getCurrentExceptionMessageAndPattern(true)));
    }
}

String DatabaseGit::getCurrentCommitHash() const
{
    std::lock_guard lock(git_operations_mutex);
    return current_commit_hash;
}

std::vector<String> DatabaseGit::getGitLog(ContextPtr /* query_context */, int max_entries)
{
    std::lock_guard lock(git_operations_mutex);
    
    String command = "git log --oneline -n " + std::to_string(max_entries);
    String log_output = executeGitCommandWithResult(command);
    
    std::vector<String> commits;
    std::istringstream stream(log_output);
    String line;
    while (std::getline(stream, line))
    {
        if (!line.empty())
        {
            commits.push_back(line);
        }
    }
    
    return commits;
}

void DatabaseGit::createSchemaSnapshot(ContextPtr /* query_context */, const String & tag_name)
{
    std::lock_guard lock(git_operations_mutex);
    
    String tag = tag_name.empty() ? "snapshot_" + std::to_string(std::time(nullptr)) : tag_name;
    executeGitCommand("git tag " + tag);
    LOG_INFO(git_log, "Created schema snapshot: {}", tag);
}

void DatabaseGit::restoreSchemaSnapshot(ContextPtr /* query_context */, const String & commit_hash)
{
    std::lock_guard lock(git_operations_mutex);
    
    executeGitCommand("git checkout " + commit_hash);
    current_commit_hash = commit_hash;
    
    reloadModifiedTables();
    
    LOG_INFO(git_log, "Restored schema snapshot: {}", commit_hash);
}

std::vector<String> DatabaseGit::getSchemaHistory(ContextPtr query_context)
{
    return getGitLog(query_context, 50);
}

void registerDatabaseGit(DatabaseFactory & factory)
{
    auto create_fn = [](const DatabaseFactory::Arguments & args)
    {
        DatabaseMetadataDiskSettings database_metadata_disk_settings;
        auto * engine_define = args.create_query.storage;
        chassert(engine_define);
        database_metadata_disk_settings.loadFromQuery(*engine_define, args.context, args.create_query.attach);

        String git_repository_path;
        String git_remote_url;
        String git_branch = "main";
        
        auto * engine = engine_define->engine;
        if (engine && engine->arguments && !engine->arguments->children.empty())
        {
            ASTs & engine_args = engine->arguments->children;
            if (engine_args.size() >= 1)
            {
                auto * path_ast = engine_args[0]->as<ASTLiteral>();
                if (path_ast && path_ast->value.getType() == Field::Types::String)
                    git_repository_path = path_ast->value.safeGet<String>();
                
                if (engine_args.size() >= 2)
                {
                    auto * remote_ast = engine_args[1]->as<ASTLiteral>();
                    if (remote_ast && remote_ast->value.getType() == Field::Types::String)
                        git_remote_url = remote_ast->value.safeGet<String>();
                    
                    if (engine_args.size() >= 3)
                    {
                        auto * branch_ast = engine_args[2]->as<ASTLiteral>();
                        if (branch_ast && branch_ast->value.getType() == Field::Types::String)
                            git_branch = branch_ast->value.safeGet<String>();
                    }
                }
            }
        }

        auto db = make_shared<DatabaseGit>(
            args.database_name, args.metadata_path, args.context, database_metadata_disk_settings);
        
        if (!git_repository_path.empty())
            db->setGitRepositoryPath(git_repository_path);
        if (!git_remote_url.empty())
            db->setGitRemoteUrl(git_remote_url);
        db->setGitBranch(git_branch);
        
        return db;
    };
    factory.registerDatabase("Git", create_fn, /*features=*/{.supports_arguments = true, .supports_settings = true});
}

void DatabaseGit::setGitRepositoryPath(const String & path)
{
    std::lock_guard lock(git_operations_mutex);
    git_repository_path = path;
}

void DatabaseGit::setGitRemoteUrl(const String & url)
{
    std::lock_guard lock(git_operations_mutex);
    git_remote_url = url;
}

void DatabaseGit::setGitBranch(const String & branch)
{
    std::lock_guard lock(git_operations_mutex);
    git_branch = branch;
}

void DatabaseGit::startAutoPull(UInt64 interval_seconds)
{
    if (auto_pull_active.load())
    {
        LOG_WARNING(git_log, "Auto-pull is already active");
        return;
    }

    if (git_remote_url.empty())
    {
        LOG_WARNING(git_log, "Cannot start auto-pull: no remote URL configured");
        return;
    }

    pull_interval_seconds.store(interval_seconds);
    should_stop_auto_pull.store(false);
    auto_pull_active.store(true);

    // Start background thread for auto-pull
    auto_pull_thread = std::make_unique<std::thread>([this]()
    {
        String db_name = getDatabaseName();
        LOG_INFO(git_log, "Auto-pull started for database {} with interval {} seconds", db_name, pull_interval_seconds.load());

        while (!should_stop_auto_pull.load())
        {
            try
            {
                std::this_thread::sleep_for(std::chrono::seconds(pull_interval_seconds.load()));

                if (should_stop_auto_pull.load())
                    break;

                LOG_DEBUG(git_log, "Auto-pull triggered for database {}", db_name);
                
                gitPullFromRemote();
                
                LOG_DEBUG(git_log, "Auto-pull completed for database {}", db_name);
            }
            catch (...)
            {
                LOG_ERROR(git_log, "Auto-pull failed for database {}: {}", 
                         db_name, String(getCurrentExceptionMessageAndPattern(true)));
                
                std::this_thread::sleep_for(std::chrono::seconds(60));
            }
        }

        LOG_INFO(git_log, "Auto-pull stopped for database {}", db_name);
        auto_pull_active.store(false);
    });

    LOG_INFO(git_log, "Auto-pull started with interval {} seconds", interval_seconds);
}

void DatabaseGit::stopAutoPull()
{
    if (!auto_pull_active.load())
    {
        return;
    }

    LOG_INFO(git_log, "Stopping auto-pull...");
    should_stop_auto_pull.store(true);

    if (auto_pull_thread && auto_pull_thread->joinable())
    {
        auto_pull_thread->join();
        auto_pull_thread.reset();
    }

    auto_pull_active.store(false);
    LOG_INFO(git_log, "Auto-pull stopped");
}

bool DatabaseGit::isAutoPullActive() const
{
    return auto_pull_active.load();
}

}
