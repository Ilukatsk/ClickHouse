#include <Databases/Git/GitWorker.h>

#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <array>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <sys/stat.h>
#include <regex>

#include <Common/Exception.h>
#include <Common/ErrorCodes.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int CANNOT_OPEN_FILE;
    extern const int CANNOT_PIPE;
}

static std::string convertHttpToSshUrl(const std::string & url)
{
    if (url.empty())
        return url;
    
    std::regex http_regex(R"(https?://github\.com/([^/]+)/([^/]+?)(?:\.git)?$)");
    std::smatch matches;
    
    if (std::regex_match(url, matches, http_regex))
    {
        std::string user = matches[1].str();
        std::string repo = matches[2].str();
        
        // Ensure .git extension
        if (!repo.ends_with(".git"))
            repo += ".git";
            
        return "git@github.com:" + user + "/" + repo;
    }
    
    std::regex generic_regex(R"(https?://([^/]+)/([^/]+)/([^/]+?)(?:\.git)?$)");
    if (std::regex_match(url, matches, generic_regex))
    {
        std::string host = matches[1].str();
        std::string user = matches[2].str();
        std::string repo = matches[3].str();
        
        // Ensure .git extension
        if (!repo.ends_with(".git"))
            repo += ".git";
            
        return "git@" + host + ":" + user + "/" + repo;
    }
    
    return url;
}

 GitWorker::GitWorker(const std::string& schema_path_param, const std::string& remote_repo_path_param)
    : schema_path(schema_path_param), remote_repo_path(convertHttpToSshUrl(remote_repo_path_param))
{
    if (schema_path_param.empty()) {
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Schema path must be non-empty");
    }
    if (!makeDir(schema_path_param)) {
        throw Exception(ErrorCodes::CANNOT_OPEN_FILE, "Failed to access schema path");
    }
    if (!isGitRepo(schema_path_param)) {
        if (remote_repo_path_param.empty() || !isGitUrl(remote_repo_path_param)) {
            if (!initRepo(schema_path_param, remote_repo_path_param)) {
                throw Exception(ErrorCodes::CANNOT_OPEN_FILE, "Failed to initialize repository");

            }
        } else if (!cloneRepo(schema_path_param, remote_repo_path_param)) {
            throw Exception(ErrorCodes::CANNOT_OPEN_FILE, "Failed to clone repository");
        }
    }
}

int32_t GitWorker::run(const std::string & cmd)
{
    return std::system(cmd.c_str());
}

std::string GitWorker::runAndCapture(const std::string & cmd)
{
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe)
        throw Exception(ErrorCodes::CANNOT_PIPE, "popen() failed!");
    
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
        result += buffer.data();
    
    if (!result.empty() && result.back() == '\n')
        result.pop_back();
    
    return result;
}

bool GitWorker::makeDir(const std::string & local_dir_path, mode_t mode)
{
    if (mkdir(local_dir_path.c_str(), mode) == -1)
    {
        if (errno == EEXIST)
            return true;
        else
            return false;
    }
    return true;
}

bool GitWorker::changeDir(const std::string & local_dir_path)
{
    if (chdir(local_dir_path.c_str()) == -1)
        return false;
    return true;
}

bool GitWorker::isGitRepo(const std::string & local_dir_path)
{
    int status = run("git -C " + local_dir_path + " rev-parse --is-inside-work-tree > /dev/null 2>&1");
    return status == 0;
}

bool GitWorker::isGitUrl(const std::string & url)
{
    int is_remote = run("git ls-remote " + url + " > /dev/null 2>&1");
    return is_remote == 0;
}

bool GitWorker::isEmptyRepository(const std::string & url)
{
    std::string output = runAndCapture("git ls-remote " + url);
    return output.empty();
}

bool GitWorker::initRepo(const std::string & local_dir_path, const std::string & url)
{
    if (run("git init " + local_dir_path) != 0)
        return false;
    
    if (makeDir(local_dir_path))
        changeDir(local_dir_path);
    else
        return false;

    run("echo \"# New Repo\" > README.md");
    if (run("git add README.md") != 0
        || run("git commit -m \"Initial commit\"") != 0
        || run("git branch -M main") != 0
        || run("git remote add origin " + url) != 0)
    {
        return false;
    }

    run("git push -u origin main");
    
    return true;
}

bool GitWorker::cloneRepo(const std::string & local_dir_path, const std::string & url)
{
    // Clone the repository
    if (run("git clone " + url + " " + local_dir_path) != 0)
        return false;
    
    return true;
}

std::string GitWorker::getLocalLastCommit(const std::string & local_dir_path, const std::string & branch)
{
    if (makeDir(local_dir_path))
        changeDir(local_dir_path);
    else
        throw Exception(ErrorCodes::CANNOT_OPEN_FILE, "Failed to access directory");
    
    std::string cmd = "git rev-parse " + branch;
    std::string hash = runAndCapture(cmd);
    return hash;
}

std::string GitWorker::getRemoteLastCommit(const std::string & url, const std::string & branch)
{
    std::string cmd = "git ls-remote " + url + " " + branch;
    std::string output = runAndCapture(cmd);
    std::istringstream iss(output);
    std::string hash;
    iss >> hash;
    return hash;
}

bool GitWorker::createCommit(const std::string & message)
{
    if (makeDir(schema_path))
        changeDir(schema_path);

    std::string cmd = "git commit -m \"" + message + "\"";
    int status = run(cmd);
    if (status != 0)
        throw Exception(ErrorCodes::CANNOT_OPEN_FILE, "Failed to create commit (maybe no changes to commit?)");
    
    return true;
}

void GitWorker::addFiles(const std::vector<std::string> & files)
{
    if (!changeDir(schema_path))
        throw Exception(ErrorCodes::CANNOT_OPEN_FILE, "Failed to change to repository directory");
    
    std::string files_str;
    for (const auto & file : files)
    {
        if (!files_str.empty())
            files_str += " ";
        files_str += file;
    }
    
    if (run("git add " + files_str) != 0)
        throw Exception(ErrorCodes::CANNOT_OPEN_FILE, "Failed to stage files");
}

std::vector<std::string> GitWorker::getFiles()
{
    if (makeDir(schema_path))
        changeDir(schema_path);
    
    std::vector<std::string> files;
    std::string cmd = "git ls-files";
    std::string output = runAndCapture(cmd);
    std::istringstream iss(output);
    std::string file;
    while (iss >> file)
    {
        files.push_back(file);
        file.clear();
    }
    return files;
}

void GitWorker::pull()
{
    try
    {
        std::string local_commit = getLocalLastCommit(schema_path);
        std::string remote_commit = getRemoteLastCommit(remote_repo_path, "main");
        
        if (local_commit.empty() && remote_commit.empty())
        {
            return;
        }
        
        if (local_commit != remote_commit)
        {
            run("git pull origin main");
        }
    }
    catch (...)
    {
        return;
    }
}

void GitWorker::push()
{
    if (run("git push origin main") != 0)
        throw Exception(ErrorCodes::CANNOT_OPEN_FILE, "Failed to push repository");
}

}
