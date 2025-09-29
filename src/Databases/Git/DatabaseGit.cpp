#include <Databases/Git/DatabaseGit.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTLiteral.h>
#include <Interpreters/Context.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>

namespace DB
{

DatabaseGit::DatabaseGit(
    String name_,
    String metadata_path_,
    const String & git_logger_name,
    ContextPtr context_,
    DatabaseMetadataDiskSettings database_metadata_disk_settings_,
    const String & git_repository_path,
    const String & git_remote_url)
    : DatabaseOrdinary(name_, metadata_path_, "store/", git_logger_name, context_, database_metadata_disk_settings_)
    , git_log(&Poco::Logger::get(git_logger_name))
{
    String repo_path;
    if (git_repository_path.empty())
    {
        repo_path = (fs::path(metadata_path_).parent_path() / name_).string();
    }
    else
    {
        repo_path = git_repository_path;
    }
    
    git_worker = std::make_unique<GitWorker>(repo_path, git_remote_url);
}

DatabaseGit::DatabaseGit(
    String name_, String metadata_path_, ContextPtr context_, DatabaseMetadataDiskSettings database_metadata_disk_settings_, const String & git_repository_path, const String & git_remote_url)
    : DatabaseGit(name_, std::move(metadata_path_), "DatabaseGit (" + name_ + ")", context_, database_metadata_disk_settings_, git_repository_path, git_remote_url)
{
}

DatabaseGit::~DatabaseGit()
{
    stopAutoPull();
}

void DatabaseGit::commitCreateTable(const ASTCreateQuery & query, const StoragePtr & table,
                                   const String & table_metadata_tmp_path, const String & table_metadata_path,
                                   ContextPtr query_context)
{
    std::lock_guard lock(git_operations_mutex);
    
    try
    {
        try
        {
            LOG_INFO(git_log, "Pulling latest changes before creating table {}", query.getTable());
            pullFromRemote();
        }
        catch (...)
        {
            LOG_WARNING(git_log, "Failed to pull before creating table {}: {}", 
                        query.getTable(), String(getCurrentExceptionMessageAndPattern(true)));
        }

        DatabaseOrdinary::commitCreateTable(query, table, table_metadata_tmp_path, table_metadata_path, query_context);
        
        try
        {
            String commit_message = "CREATE TABLE " + query.getTable();
            commitChanges(commit_message);
            
            LOG_INFO(git_log, "Created table {} and committed to Git", query.getTable());
        }
        catch (...)
        {
            LOG_ERROR(git_log, "Git commit failed for CREATE TABLE {}: {}", query.getTable(), String(getCurrentExceptionMessageAndPattern(true)));
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
        try
        {
            LOG_INFO(git_log, "Pulling latest changes before altering table {}", table_id.table_name);
            pullFromRemote();
        }
        catch (...)
        {
            LOG_WARNING(git_log, "Failed to pull before altering table {}: {}", 
                table_id.table_name, String(getCurrentExceptionMessageAndPattern(true)));
        }

        DatabaseOrdinary::commitAlterTable(table_id, table_metadata_tmp_path, table_metadata_path, statement, query_context);
        
        try
        {
            String commit_message = "ALTER TABLE " + table_id.table_name;
            commitChanges(commit_message);
            
            LOG_INFO(git_log, "Altered table {} and committed to Git", table_id.table_name);
        }
        catch (...)
        {
            LOG_ERROR(git_log, "Git commit failed for ALTER TABLE {}: {}", table_id.table_name, String(getCurrentExceptionMessageAndPattern(true)));
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
        try
        {
            LOG_INFO(git_log, "Pulling latest changes before dropping table {}", table_name);
            pullFromRemote();
        }
        catch (...)
        {
            LOG_WARNING(git_log, "Failed to pull before dropping table {}: {}", 
                        table_name, String(getCurrentExceptionMessageAndPattern(true)));
        }

        DatabaseOrdinary::dropTable(query_context, table_name, sync);
        
        try
        {
            String commit_message = "DROP TABLE " + table_name;
            commitChanges(commit_message);
            
            LOG_INFO(git_log, "Dropped table {} and committed to Git", table_name);
        }
        catch (...)
        {
            LOG_ERROR(git_log, "Git commit failed for DROP TABLE {}: {}", table_name, String(getCurrentExceptionMessageAndPattern(true)));
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
        try
        {
            LOG_INFO(git_log, "Pulling latest changes before renaming table {} to {}", table_name, to_table_name);
            pullFromRemote();
        }
        catch (...)
        {
            LOG_WARNING(git_log, "Failed to pull before renaming table {}: {}", 
                    table_name, String(getCurrentExceptionMessageAndPattern(true)));
        }

        DatabaseOrdinary::renameTable(query_context, table_name, to_database, to_table_name, exchange, dictionary);
        
        String commit_message = "RENAME TABLE " + table_name + " TO " + to_table_name;
        commitChanges(commit_message);
        
        LOG_INFO(git_log, "Renamed table {} to {} and committed to Git", table_name, to_table_name);
    }
    catch (...)
    {
        LOG_ERROR(git_log, "Git commit failed for RENAME TABLE {} to {}: {}", table_name, to_table_name, String(getCurrentExceptionMessageAndPattern(true)));
    }
}

void DatabaseGit::startAutoPull(UInt64 interval_seconds)
{
    if (auto_pull_active.load())
    {
        LOG_WARNING(git_log, "Auto-pull is already active");
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
                
                std::lock_guard lock(git_operations_mutex);
                pullFromRemote();
                
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

void DatabaseGit::commitChanges(const String & message)
{
    if (!git_worker)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "GitWorker not initialized");
    
    // Get all files and filter for .sql files only
    std::vector<std::string> all_files = git_worker->getFiles();
    std::vector<std::string> sql_files;
    
    for (const auto & file : all_files)
    {
        if (file.length() >= 4 && file.substr(file.length() - 4) == ".sql")
        {
            sql_files.push_back(file);
        }
    }
    
    // Add only SQL files
    git_worker->addFiles(sql_files);
    git_worker->createCommit(message);
    git_worker->push();
}

void DatabaseGit::pullFromRemote()
{
    if (!git_worker)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "GitWorker not initialized");
    
    git_worker->pull();
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
                }
            }
        }

        auto db = make_shared<DatabaseGit>(
            args.database_name, args.metadata_path, args.context, database_metadata_disk_settings, git_repository_path, git_remote_url);
        
        return db;
    };
    factory.registerDatabase("Git", create_fn, /*features=*/{.supports_arguments = true, .supports_settings = true});
}

}
