#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace DB
{

class GitWorker
{
public:
    GitWorker(const std::string & schema_path_param, const std::string & remote_repo_path_param = "");
    
    // High-level operations
    void pull();
    void push();
    void addFiles(const std::vector<std::string> & files = {"."});
    bool createCommit(const std::string & message);
    std::vector<std::string> getFiles();
    
private:
    // Utility functions
    int32_t run(const std::string & cmd);
    std::string runAndCapture(const std::string & cmd);
    bool makeDir(const std::string & local_dir_path, mode_t mode = 0755);
    bool changeDir(const std::string & local_dir_path);
    
    // Git validation functions
    bool isGitRepo(const std::string & local_dir_path);
    bool isGitUrl(const std::string & url);
    bool isEmptyRepository(const std::string & url);
    
    // Git operations
    bool initRepo(const std::string & local_dir_path, const std::string & url);
    bool cloneRepo(const std::string & local_dir_path, const std::string & url);
    std::string getLocalLastCommit(const std::string & local_dir_path, const std::string & branch = "main");
    std::string getRemoteLastCommit(const std::string & url, const std::string & branch = "main");
    
    // Member variables
    std::string schema_path;
    std::string remote_repo_path;
};

}
