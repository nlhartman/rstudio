/*
 * SessionSourceControl.cpp
 *
 * Copyright (C) 2009-11 by RStudio, Inc.
 *
 * This program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */
#include "SessionSourceControl.hpp"

#include <signal.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#endif

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/foreach.hpp>
#include <boost/function.hpp>
#include <boost/lambda/lambda.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/optional.hpp>
#include <boost/regex.hpp>

#include <core/json/JsonRpc.hpp>
#include <core/system/Crypto.hpp>
#include <core/system/ShellUtils.hpp>
#include <core/system/System.hpp>
#include <core/system/Process.hpp>
#include <core/system/Environment.hpp>
#include <core/Exec.hpp>
#include <core/FileSerializer.hpp>
#include <core/GitGraph.hpp>
#include <core/Scope.hpp>
#include <core/StringUtils.hpp>

#include <r/RExec.hpp>

#include <session/SessionUserSettings.hpp>
#include <session/SessionModuleContext.hpp>
#include <session/projects/SessionProjects.hpp>

#include "SessionConsoleProcess.hpp"

#include "config.h"

#ifdef RSTUDIO_SERVER
#include <sys/stat.h>
#endif

// TODO: It's actually pretty easy to look in an id_rsa file and see if it's encrypted,
//       use that to see if we even need to do ssh-agent stuff at all
// TODO: We could use the "right" ssh key if we wanted to, by reading ~/.ssh/config:
//       http://technosophos.com/content/ssh-host-configuration

using namespace core;
using namespace core::shell_utils;
using session::modules::console_process::ConsoleProcess;

namespace session {
namespace modules {
namespace source_control {

namespace {

const size_t WARN_SIZE = 200 * 1024;

// git bin dir which we detect at startup. note that if the git bin
// is already in the path then this will be empty
std::string s_gitBinDir;
uint64_t s_gitVersion;
const uint64_t GIT_1_7_2 = ((uint64_t)1 << 48) |
                           ((uint64_t)7 << 32) |
                           ((uint64_t)2 << 16);

core::system::ProcessOptions procOptions()
{
   core::system::ProcessOptions options;

   // detach the session so there is no terminal
   options.detachSession = true;

   // get current environment for modification prior to passing to child
   core::system::Options childEnv;
   core::system::environment(&childEnv);

   // add git bin dir to PATH if necessary
   if (!s_gitBinDir.empty())
      core::system::addToPath(&childEnv, s_gitBinDir);

   // add postback directory to PATH
   FilePath postbackDir = session::options().rpostbackPath().parent();
   core::system::addToPath(&childEnv, postbackDir.absolutePath());

   // on windows set HOME to USERPROFILE
#ifdef _WIN32
   std::string userProfile = core::system::getenv(childEnv, "USERPROFILE");
   core::system::setenv(&childEnv, "HOME", userProfile);
#endif

   // set custom environment
   options.environment = childEnv;

   return options;
}

enum PatchMode
{
   PatchModeWorking = 0,
   PatchModeStage = 1
};

struct CommitInfo
{
   std::string id;
   std::string author;
   std::string subject;
   std::string description;
   std::string parent;
   boost::int64_t date; // millis since epoch, UTC
   std::vector<std::string> refs;
   std::vector<std::string> tags;
   std::string graph;
};

void enqueueRefreshEvent()
{
   module_context::enqueClientEvent(ClientEvent(client_events::kVcsRefresh));
}

class VCSImpl : boost::noncopyable
{
public:
   VCSImpl(FilePath rootDir)
   {
      root_ = rootDir;
   }

   virtual ~VCSImpl()
   {
   }

   virtual VCS id() { return VCSNone; }
   virtual std::string name() { return std::string(); }
   FilePath root() { return root_; }

   virtual core::Error status(const FilePath&,
                              StatusResult* pStatusResult)
   {
      *pStatusResult = StatusResult();
      return Success();
   }

   virtual core::Error add(const std::vector<FilePath>& filePaths,
                           std::string* pStdErr)
   {
      return Success();
   }

   virtual core::Error remove(const std::vector<FilePath>& filePaths,
                              std::string* pStdErr)
   {
      return Success();
   }

   virtual core::Error discard(const std::vector<FilePath>& filePaths,
                               std::string* pStdErr)
   {
      return Success();
   }

   virtual core::Error stage(const std::vector<FilePath>& filePaths,
                             std::string* pStdErr)
   {
      return Success();
   }

   virtual core::Error unstage(const std::vector<FilePath>& filePaths,
                               std::string* pStdErr)
   {
      return Success();
   }

   virtual core::Error listBranches(std::vector<std::string>* pBranches,
                                    boost::optional<size_t>* pActiveBranchIndex)
   {
      return Success();
   }

   virtual core::Error checkout(const std::string& id,
                                std::string* pHandle)
   {
      return Success();
   }

   virtual core::Error hasRemote(bool* pHasRemote)
   {
      *pHasRemote = false;
      return Success();
   }

   core::Error runCommand(const ShellCommand& command,
                          std::vector<std::string>* pOutputLines=NULL)
   {
      std::string output;
      Error error = runCommand(command, &output, NULL);
      if (error)
         return error;

      if (pOutputLines)
      {
         boost::algorithm::split(*pOutputLines, output,
                                 boost::algorithm::is_any_of("\r\n"));
      }

      return Success();
   }

   core::Error runCommand(const ShellCommand& command,
                          std::string* pOutput,
                          std::string* pStdErr)
   {
      return runCommand(command.string(), pOutput, pStdErr);
   }

   std::string wrapWithCd(const ShellCommand& command)
   {
      return shell_utils::join(
               ShellCommand("cd") << root_,
               command);
   }

   core::Error runCommand(const std::string& command,
                          std::string* pOutput,
                          std::string* pStdErr)
   {
      std::string cmd = shell_utils::join(
            ShellCommand(std::string("cd")) << root_,
            command);

      core::system::ProcessResult result;
      Error error = core::system::runCommand(cmd,
                                             procOptions(),
                                             &result);
      if (error)
         return error;

      if (pOutput)
         *pOutput = result.stdOut;
      if (pStdErr)
         *pStdErr = result.stdErr;

      return Success();
   }

   virtual core::Error diffFile(const FilePath& filePath,
                                PatchMode mode,
                                int contextLines,
                                std::string* pOutput)
   {
      return Success();
   }

   virtual core::Error applyPatch(const FilePath& patchFile,
                                  PatchMode patchMode,
                                  std::string* pStdErr)
   {
      return Success();
   }

   virtual core::Error log(const std::string& rev,
                           int skip,
                           int maxentries,
                           const std::string& filterText,
                           std::vector<CommitInfo>* pOutput)
   {
      return Success();
   }

   virtual core::Error logLength(const std::string& rev,
                                 const std::string& filterText,
                                 int* pLength)
   {
      *pLength = 0;
      return Success();
   }

   virtual core::Error show(const std::string& rev,
                            std::string* pOutput)
   {
      return Success();
   }

protected:
   FilePath root_;
};

namespace {

boost::scoped_ptr<VCSImpl> s_pVcsImpl_;
std::vector<PidType> s_pidsToTerminate_;

ShellCommand git()
{
   if (!s_gitBinDir.empty())
   {
      FilePath fullPath = FilePath(s_gitBinDir).childPath("git");
      return ShellCommand(fullPath);
   }
   else
      return ShellCommand("git");
}

ShellCommand svn()
{
   return ShellCommand("svn");
}

void afterCommit(const FilePath& tempFile)
{
   Error removeError = tempFile.remove();
   if (removeError)
      LOG_ERROR(removeError);
   enqueueRefreshEvent();
}

bool commitIsMatch(const std::vector<std::string>& patterns,
                   const CommitInfo& commit)
{
   BOOST_FOREACH(std::string pattern, patterns)
   {
      if (!boost::algorithm::ifind_first(commit.author, pattern)
          && !boost::algorithm::ifind_first(commit.description, pattern)
          && !boost::algorithm::ifind_first(commit.id, pattern))
      {
         return false;
      }
   }

   return true;
}

boost::function<bool(CommitInfo)> createFilterPredicate(
      const std::string& filter)
{
   if (filter.empty())
      return boost::lambda::constant(true);

   std::vector<std::string> results;
   boost::algorithm::split(results, filter,
                           boost::algorithm::is_any_of(" \t\r\n"));
   return boost::bind(commitIsMatch, results, _1);
}

} // anonymous namespace

class GitVCSImpl : public VCSImpl
{
public:
   static FilePath detectGitDir(FilePath workingDir)
   {
      std::string command = shell_utils::join_and(
            ShellCommand("cd") << workingDir,
            git() << "rev-parse" << "--show-toplevel");

      core::system::ProcessResult result;
      Error error = core::system::runCommand(command,
                                             procOptions(),
                                             &result);
      if (error)
         return FilePath();

      if (result.exitStatus != 0)
         return FilePath();

      return FilePath(boost::algorithm::trim_copy(result.stdOut));
   }

   GitVCSImpl(FilePath repoDir) : VCSImpl(repoDir)
   {
   }

   VCS id() { return VCSGit; }
   std::string name() { return "Git"; }

   core::Error status(const FilePath& dir, StatusResult* pStatusResult)
   {
      using namespace boost;

      std::vector<FileWithStatus> files;

      std::vector<std::string> args;
      args.push_back("status");
      args.push_back("--porcelain");
      args.push_back("--");
      args.push_back(string_utils::utf8ToSystem(dir.absolutePath()));

      std::vector<std::string> lines;
      Error error = runCommand(
            git() << "status" << "--porcelain" << "--" << dir,
            &lines);
      if (error)
         return error;

      for (std::vector<std::string>::iterator it = lines.begin();
           it != lines.end();
           it++)
      {
         std::string line = *it;
         if (line.length() < 4)
            continue;
         FileWithStatus file;

         file.status = line.substr(0, 2);

         std::string filePath = line.substr(3);
         if (filePath.length() > 1 && filePath[filePath.length() - 1] == '/')
            filePath = filePath.substr(0, filePath.size() - 1);
         file.path = root_.childPath(string_utils::systemToUtf8(filePath));

         files.push_back(file);
      }

      *pStatusResult = StatusResult(files);

      return Success();
   }

   core::Error doSimpleCmd(const ShellCommand& command,
                           std::string* pStdErr)
   {
      return runCommand(command, NULL, pStdErr);
   }

   core::Error add(const std::vector<FilePath>& filePaths,
                   std::string* pStdErr)
   {
      return doSimpleCmd(git() << "add" << "--" << filePaths,
                         pStdErr);
   }

   core::Error remove(const std::vector<FilePath>& filePaths,
                      std::string* pStdErr)
   {
      return doSimpleCmd(git() << "rm" << "--" << filePaths,
                         pStdErr);
   }

   core::Error discard(const std::vector<FilePath>& filePaths,
                       std::string* pStdErr)
   {
      std::vector<std::string> args;
      args.push_back("-f"); // don't fail on unmerged entries
      return doSimpleCmd(
            git() << "checkout" << "-f" << "--" << filePaths,
            pStdErr);
   }

   core::Error stage(const std::vector<FilePath> &filePaths,
                     std::string *pStdErr)
   {
      StatusResult statusResult;
      this->status(root_, &statusResult);

      std::vector<FilePath> filesToAdd;
      std::vector<FilePath> filesToRm;

      BOOST_FOREACH(const FilePath& path, filePaths)
      {
         std::string status = statusResult.getStatus(path).status();
         if (status.size() < 2)
         {
            // In the case of renames, getStatus(path) might not return
            // anything
            StatusResult individualStatusResult;
            this->status(path, &individualStatusResult);
            status = individualStatusResult.getStatus(path).status();
            if (status.size() < 2)
               continue;
         }
         if (status[1] == 'D')
            filesToRm.push_back(path);
         else if (status[1] != ' ')
            filesToAdd.push_back(path);
      }

      Error error;

      if (!filesToAdd.empty())
      {
         error = this->add(filesToAdd, pStdErr);
         if (error)
            return error;
      }

      if (!filesToRm.empty())
      {
         error = this->remove(filesToRm, pStdErr);
         if (error)
            return error;
      }

      return Success();
   }

   core::Error unstage(const std::vector<FilePath>& filePaths,
                       std::string* pStdErr)
   {
      return doSimpleCmd(git() << "reset" << "HEAD" << "--" << filePaths,
                         pStdErr);
   }

   core::Error listBranches(std::vector<std::string>* pBranches,
                            boost::optional<size_t>* pActiveBranchIndex)
   {
      std::vector<std::string> lines;

      Error error = runCommand(git() << "branch", &lines);
      if (error)
         return error;

      for (size_t i = 0; i < lines.size(); i++)
      {
         const std::string line = lines.at(i);
         if (line.size() < 2)
            break;

         if (boost::algorithm::starts_with(line, "* "))
            *pActiveBranchIndex = i;
         pBranches->push_back(line.substr(2));
      }

      return Success();
   }

   core::Error checkout(const std::string& id,
                        std::string* pHandle)
   {
      boost::shared_ptr<ConsoleProcess> ptrProc =
            console_process::ConsoleProcess::create(wrapWithCd(git() << "checkout"
                                                               << id << "--"),
                                                    procOptions(),
                                                    "Git Checkout",
                                                    true,
                                                    &enqueueRefreshEvent);
      *pHandle = ptrProc->handle();
      return Success();
   }

   core::Error commit(const std::string& message, bool amend, bool signOff,
                      std::string* pHandle)
   {
      FilePath tempFile = module_context::tempFile("gitmsg", "txt");
      boost::shared_ptr<std::ostream> pStream;

      Error error = tempFile.open_w(&pStream);
      if (error)
         return error;

      *pStream << message;

      FilePath gitDir = root_.childPath(".git");
      if (gitDir.childPath("MERGE_HEAD").exists())
      {
         FilePath mergeMsg = gitDir.childPath("MERGE_MSG");
         if (mergeMsg.exists())
         {
            std::string mergeMsgStr;
            error = core::readStringFromFile(mergeMsg, &mergeMsgStr);
            if (!error)
            {
               if (!message.empty())
                  *pStream << std::endl << std::endl;
               *pStream << mergeMsgStr;
            }
         }
      }

      pStream->flush();
      pStream.reset();  // release file handle

      ShellCommand command = git() << "commit" << "-F" << tempFile;
      if (amend)
         command << "--amend";
      if (signOff)
         command << "--signoff";

      boost::shared_ptr<ConsoleProcess> ptrProc =
            console_process::ConsoleProcess::create(
                  wrapWithCd(command), procOptions(), "Git Commit", true,
                  boost::bind(afterCommit, tempFile));

      *pHandle = ptrProc->handle();
      return Success();
   }

   core::Error clone(const std::string& url,
                     const std::string dirName,
                     const FilePath& parentPath,
                     std::string* pHandle)
   {

      std::string cmd = shell_utils::join_and(
            ShellCommand("cd") << parentPath,
            git() << "clone" << "--progress" << url << dirName);

      boost::shared_ptr<ConsoleProcess> ptrProc =
            console_process::ConsoleProcess::create(cmd,
                                                    procOptions(),
                                                    "Git Clone",
                                                    true);

      *pHandle = ptrProc->handle();
      return Success();
   }

   Error currentBranch(std::string* pBranch)
   {
      std::vector<std::string> branches;
      boost::optional<size_t> index;
      Error error = listBranches(&branches, &index);
      if (error)
         return error;
      if (!index)
      {
         pBranch->clear();
         return Success();
      }

      *pBranch = branches.at(*index);
      if (*pBranch == "(no branch)")
      {
         pBranch->clear();
      }
      return Success();
   }

   bool remoteMerge(const std::string& branch,
                    std::string* pRemote,
                    std::string* pMerge)
   {
      core::system::ProcessResult result;
      Error error = core::system::runCommand(
            wrapWithCd(git() << "config" << "--get" <<
               "branch." + branch + ".remote"),
            procOptions(),
            &result);
      if (error)
      {
         LOG_ERROR(error);
         return false;
      }
      if (result.exitStatus != EXIT_SUCCESS)
         return false;
      *pRemote = result.stdOut;
      boost::algorithm::trim(*pRemote);

      core::system::ProcessResult result2;
      error = core::system::runCommand(
            wrapWithCd(git() << "config" << "--get" <<
               "branch." + branch + ".merge"),
            procOptions(),
            &result2);
      if (error)
      {
         LOG_ERROR(error);
         return false;
      }
      if (result2.exitStatus != EXIT_SUCCESS)
         return false;
      *pMerge = result2.stdOut;
      boost::algorithm::trim(*pMerge);

      return true;
   }

   core::Error push(std::string* pHandle)
   {
      std::string branch;
      Error error = currentBranch(&branch);
      if (error)
         return error;

      ShellCommand cmd = git() << "push";

      std::string remote, merge;
      if (remoteMerge(branch, &remote, &merge))
      {
         cmd << remote << merge;
      }

      boost::shared_ptr<ConsoleProcess> ptrProc =
            console_process::ConsoleProcess::create(wrapWithCd(cmd),
                                                    procOptions(),
                                                    "Git Push",
                                                    true,
                                                    &enqueueRefreshEvent);

      *pHandle = ptrProc->handle();
      return Success();
   }

   core::Error pull(std::string* pHandle)
   {
      boost::shared_ptr<ConsoleProcess> ptrProc =
            console_process::ConsoleProcess::create(wrapWithCd(git() << "pull"),
                                                    procOptions(),
                                                    "Git Pull",
                                                    true,
                                                    &enqueueRefreshEvent);

      *pHandle = ptrProc->handle();
      return Success();
   }

   core::Error doDiffFile(const FilePath& filePath,
                          const FilePath* pCompareTo,
                          PatchMode mode,
                          int contextLines,
                          std::string* pOutput)
   {
      ShellCommand command = git() << "diff";
      command << "-U" + boost::lexical_cast<std::string>(contextLines);
      if (mode == PatchModeStage)
         command << "--cached";
      command << "--";
      if (pCompareTo)
         command << *pCompareTo;
      command << filePath;

      return runCommand(command, pOutput, NULL);
   }

   core::Error diffFile(const FilePath& filePath,
                        PatchMode mode,
                        int contextLines,
                        std::string* pOutput)
   {
      Error error = doDiffFile(filePath, NULL, mode, contextLines, pOutput);
      if (error)
         return error;

      if (pOutput->empty())
      {
         // detect add case
         VCSStatus status;
         error = fileStatus(filePath, &status);
         if (error)
            return error;
         if (status.status() == "??" && mode == PatchModeWorking)
         {
            error = doDiffFile(filePath,
                               &(shell_utils::devnull()),
                               mode,
                               contextLines,
                               pOutput);
            if (error)
               return error;
         }
      }

      return Success();
   }

   boost::int64_t convertGitRawDate(const std::string& time,
                          const std::string& timeZone)
   {
      boost::int64_t secs = safe_convert::stringTo<boost::int64_t>(time, 0);

      int offset = safe_convert::stringTo<int>(timeZone, 0);

      // Positive timezone offset means we have to SUBTRACT
      // the offset to get UTC time, and vice versa
      int factor = offset > 0 ? -1 : 1;

      offset = abs(offset);
      int hours = offset / 100;
      int minutes = offset % 100;

      secs += factor * (hours * 60*60);
      secs += factor * (minutes * 60);

      return secs;
   }

   core::Error applyPatch(const FilePath& patchFile,
                          PatchMode patchMode,
                          std::string* pStdErr)
   {
      ShellCommand cmd = git() << "apply";
      if (patchMode == PatchModeStage)
         cmd << "--cached";
      cmd << "--";
      cmd << patchFile;

      return doSimpleCmd(cmd, pStdErr);
   }

   void parseCommitValue(const std::string& value,
                         CommitInfo* pCommitInfo)
   {
      static boost::regex commitRegex("^([a-z0-9]+)(\\s+\\((.*)\\))?");
      boost::smatch smatch;
      if (boost::regex_match(value, smatch, commitRegex))
      {
         pCommitInfo->id = smatch[1];
         std::vector<std::string> refs;
         if (smatch[3].matched)
         {
            boost::algorithm::split(refs,
                                    static_cast<const std::string>(smatch[3]),
                                    boost::algorithm::is_any_of(","));
            BOOST_FOREACH(std::string ref, refs)
            {
               boost::algorithm::trim(ref);
               if (boost::algorithm::starts_with(ref, "tag: "))
                  pCommitInfo->tags.push_back(ref.substr(5));
               else if (!boost::algorithm::starts_with(ref, "refs/bisect/"))
                  pCommitInfo->refs.push_back(ref);
            }
         }
      }
      else
      {
         pCommitInfo->id = value;
      }
   }

   core::Error logLength(const std::string &rev,
                         const std::string &filterText,
                         int *pLength)
   {
      if (filterText.empty())
      {
         ShellCommand cmd = git() << "log";
         cmd << "--pretty=oneline";
         if (!rev.empty())
            cmd << rev;

         ShellCommand wcCmd("wc");
         if (!s_gitBinDir.empty())
         {
            FilePath fullPath = FilePath(s_gitBinDir).childPath("wc");
            wcCmd = ShellCommand(fullPath);
         }
         wcCmd << "-l";

         std::string output;
         Error error = runCommand(shell_utils::pipe(cmd, wcCmd),
                                  &output, NULL);
         if (error)
            return error;

         boost::algorithm::trim(output);
         *pLength = safe_convert::stringTo<int>(output, 0);
         return Success();
      }
      else
      {
         std::vector<CommitInfo> output;
         Error error = log(rev, 0, -1, filterText, &output);
         if (error)
            return error;
         *pLength = output.size();
         return Success();
      }
   }

   core::Error log(const std::string& rev,
                   int skip,
                   int maxentries,
                   const std::string& filterText,
                   std::vector<CommitInfo>* pOutput)
   {
      std::vector<std::string> outLines;

      ShellCommand cmd = git() << "log";
      cmd << "--pretty=raw" << "--decorate=full" << "--date-order";

      ShellCommand revListCmd = git() << "rev-list" << "--date-order" << "--parents";
      int revListSkip = skip;

      if (filterText.empty())
      {
         // This is a way more efficient way to implement skip and maxentries
         // if we know that all commits are included.
         if (skip > 0)
         {
            cmd << "--skip=" + boost::lexical_cast<std::string>(skip);
            skip = 0;
         }
         if (maxentries >= 0)
         {
            cmd << "--max-count=" + boost::lexical_cast<std::string>(maxentries);
            maxentries = -1;

            revListCmd << "--max-count=" + boost::lexical_cast<std::string>(
                  (skip < 0 ? 0 : skip) + maxentries);
         }
      }

      if (!rev.empty())
      {
         cmd << rev;
         revListCmd << rev;
      }
      else
      {
         revListCmd << "HEAD";
      }

      if (maxentries < 0)
         maxentries = std::numeric_limits<int>::max();

      Error error = runCommand(cmd, &outLines);
      if (error)
         return error;

      std::vector<std::string> graphLines;
      if (filterText.empty())
      {
         std::vector<std::string> revOutLines;
         error = runCommand(revListCmd, &revOutLines);
         if (error)
            return error;

         gitgraph::GitGraph graph;
         for (size_t i = 0; i < revOutLines.size(); i++)
         {
            typedef std::vector<std::string> find_vector_type;
            find_vector_type parents;
            boost::algorithm::split(parents, revOutLines[i],
                                    boost::algorithm::is_any_of(" "));
            if (parents.size() < 1)
               break;

            std::string commit = parents.front();
            parents.erase(parents.begin());

            gitgraph::Line line = graph.addCommit(commit, parents);
            if (revListSkip <= 0 || static_cast<int>(i) >= revListSkip)
               graphLines.push_back(line.string());
         }
      }

      boost::function<bool(CommitInfo)> filter = createFilterPredicate(filterText);

      boost::regex kvregex("^(\\w+) (.*)$");
      boost::regex authTimeRegex("^(.*?) (\\d+) ([+\\-]?\\d+)$");

      size_t graphLineIndex = 0;
      int skipped = 0;
      CommitInfo currentCommit;

      for (std::vector<std::string>::const_iterator it = outLines.begin();
           it != outLines.end() && pOutput->size() < static_cast<size_t>(maxentries);
           it++)
      {
         boost::smatch smatch;
         if (boost::regex_search(*it, smatch, kvregex))
         {
            std::string key = smatch[1];
            std::string value = smatch[2];
            if (key == "commit")
            {
               if (!currentCommit.id.empty() && filter(currentCommit))
               {
                  if (skipped < skip)
                     skipped++;
                  else
                  {
                     if (graphLineIndex < graphLines.size())
                        currentCommit.graph = graphLines[graphLineIndex];
                     pOutput->push_back(currentCommit);
                  }

                  graphLineIndex++;
               }

               currentCommit = CommitInfo();
               parseCommitValue(value, &currentCommit);
            }
            else if (key == "author" || key == "committer")
            {
               boost::smatch authTimeMatch;
               if (boost::regex_search(value, authTimeMatch, authTimeRegex))
               {
                  std::string author = authTimeMatch[1];
                  std::string time = authTimeMatch[2];
                  std::string tz = authTimeMatch[3];

                  if (key == "author")
                     currentCommit.author = author;
                  else // if (key == "committer")
                     currentCommit.date = convertGitRawDate(time, tz);
               }
            }
            else if (key == "parent")
            {
               if (!currentCommit.parent.empty())
                  currentCommit.parent.push_back(' ');
               currentCommit.parent.append(value, 0, 8);
            }
         }
         else if (boost::starts_with(*it, "    "))
         {
            if (currentCommit.subject.empty())
               currentCommit.subject = it->substr(4);

            if (!currentCommit.description.empty())
               currentCommit.description.append("\n");
            currentCommit.description.append(it->substr(4));
         }
         else if (it->length() == 0)
         {
         }
         else
         {
            LOG_ERROR_MESSAGE("Unexpected git-log output");
         }
      }

      if (pOutput->size() < static_cast<size_t>(maxentries)
          && !currentCommit.id.empty()
          && filter(currentCommit))
      {
         if (skipped < skip)
            skipped++;
         else
         {
            if (graphLineIndex < graphLines.size())
               currentCommit.graph = graphLines[graphLineIndex];
            pOutput->push_back(currentCommit);
         }
         graphLineIndex++;
      }

      return Success();
   }

   virtual core::Error show(const std::string& rev,
                            std::string* pOutput)
   {
      ShellCommand cmd = git() << "show" << "--pretty=oneline" << "-M";
      if (s_gitVersion >= GIT_1_7_2)
         cmd << "-c";
      cmd << rev;

      return runCommand(cmd, pOutput, NULL);
   }

   virtual core::Error hasRemote(bool *pHasRemote)
   {
      std::string branch;
      Error error = currentBranch(&branch);
      if (error)
         return error;

      if (branch.empty())
      {
         *pHasRemote = false;
         return Success();
      }

      std::string remote, merge;
      *pHasRemote = remoteMerge(branch, &remote, &merge);
      return Success();
   }
};

class SubversionVCSImpl : public VCSImpl
{
public:
   SubversionVCSImpl(FilePath rootDir) : VCSImpl(rootDir)
   {
   }

   VCS id() { return VCSSubversion; }
   std::string name() { return "SVN"; }

   core::Error status(const FilePath& dir, StatusResult* pStatusResult)
   {
      using namespace boost;

      std::vector<FileWithStatus> files;

      std::vector<std::string> lines;
      Error error = runCommand(
            svn() << "status" << "--non-interactive" << "--depth=immediates"
                  << "--" << dir,
            &lines);
      if (error)
         return error;

      for (std::vector<std::string>::iterator it = lines.begin();
           it != lines.end();
           it++)
      {
         std::string line = *it;
         if (line.length() < 4)
            continue;
         FileWithStatus file;

         file.status = line.substr(0, 7);

         std::string filePath = line.substr(8);
         if (filePath.length() > 1 && filePath[filePath.length() - 1] == '/')
            filePath = filePath.substr(0, filePath.size() - 1);
         file.path = FilePath(string_utils::systemToUtf8(filePath));
         files.push_back(file);
      }

      *pStatusResult = StatusResult(files);

      return Success();
   }

   core::Error revert(const std::vector<FilePath>& filePaths,
                      std::string* pStdErr)
   {
      return runCommand(svn() << "revert" << "--" << filePaths,
                        NULL, pStdErr);
   }
};

FilePath resolveAliasedPath(std::string path)
{
   if (boost::algorithm::starts_with(path, "~/"))
      return module_context::resolveAliasedPath(path);
   else
      return s_pVcsImpl_->root().childPath(path);
}

bool splitRename(const std::string& path, std::string* pOld, std::string* pNew)
{
   const std::string RENAME(" -> ");

   size_t index = path.find(RENAME);
   if (index == path.npos)
      return false;

   if (pOld)
      *pOld = std::string(path.begin(), path.begin() + index);
   if (pNew)
      *pNew = std::string(path.begin() + index + RENAME.size(), path.end());

   return true;
}

std::vector<FilePath> resolveAliasedPaths(const json::Array& paths,
                                          bool includeRenameOld = false,
                                          bool includeRenameNew = true)
{
   std::vector<FilePath> results;
   for (json::Array::const_iterator it = paths.begin();
        it != paths.end();
        it++)
   {
      std::string oldPath, newPath;
      if (splitRename(it->get_str(), &oldPath, &newPath))
      {
         if (includeRenameOld)
            results.push_back(resolveAliasedPath(oldPath));
         if (includeRenameNew)
            results.push_back(resolveAliasedPath(newPath));
      }
      else
      {
         results.push_back(resolveAliasedPath(it->get_str()));
      }
   }
   return results;
}

} // anonymous namespace


VCS activeVCS()
{
   return s_pVcsImpl_->id();
}

std::string activeVCSName()
{
   return s_pVcsImpl_->name();
}

VCSStatus StatusResult::getStatus(const FilePath& fileOrDirectory) const
{
   std::map<std::string, VCSStatus>::const_iterator found =
         this->filesByPath_.find(fileOrDirectory.absolutePath());
   if (found != this->filesByPath_.end())
      return found->second;

   return VCSStatus();
}

core::Error status(const FilePath& dir, StatusResult* pStatusResult)
{
   return s_pVcsImpl_->status(dir, pStatusResult);
}

Error fileStatus(const FilePath& filePath, VCSStatus* pStatus)
{
   StatusResult statusResult;
   Error error = source_control::status(filePath.parent(), &statusResult);
   if (error)
      return error;

   *pStatus = statusResult.getStatus(filePath);

   return Success();
}

namespace {

struct RefreshOnExit : public boost::noncopyable
{
   ~RefreshOnExit()
   {
      try
      {
         enqueueRefreshEvent();
      }
      catch(...)
      {
      }
   }
};

Error vcsAdd(const json::JsonRpcRequest& request,
             json::JsonRpcResponse* pResponse)
{
   RefreshOnExit refreshOnExit;

   json::Array paths;
   Error error = json::readParam(request.params, 0, &paths);
   if (error)
      return error ;

   return s_pVcsImpl_->add(resolveAliasedPaths(paths), NULL);
}

Error vcsRemove(const json::JsonRpcRequest& request,
                json::JsonRpcResponse* pResponse)
{
   RefreshOnExit refreshOnExit;

   json::Array paths;
   Error error = json::readParam(request.params, 0, &paths);
   if (error)
      return error ;

   return s_pVcsImpl_->remove(resolveAliasedPaths(paths), NULL);
}

Error vcsDiscard(const json::JsonRpcRequest& request,
                 json::JsonRpcResponse* pResponse)
{
   RefreshOnExit refreshOnExit;

   json::Array paths;
   Error error = json::readParam(request.params, 0, &paths);
   if (error)
      return error ;

   return s_pVcsImpl_->discard(resolveAliasedPaths(paths), NULL);
}

Error vcsRevert(const json::JsonRpcRequest& request,
                json::JsonRpcResponse* pResponse)
{
   RefreshOnExit refreshOnExit;

   json::Array paths;
   Error error = json::readParam(request.params, 0, &paths);
   if (error)
      return error ;

   error = s_pVcsImpl_->unstage(resolveAliasedPaths(paths, true, true), NULL);
   if (error)
      return error;
   error = s_pVcsImpl_->discard(resolveAliasedPaths(paths, true, false), NULL);
   if (error)
      return error;

   return Success();
}

Error vcsStage(const json::JsonRpcRequest& request,
               json::JsonRpcResponse* pResponse)
{
   RefreshOnExit refreshOnExit;

   json::Array paths;
   Error error = json::readParam(request.params, 0, &paths);
   if (error)
      return error ;

   return s_pVcsImpl_->stage(resolveAliasedPaths(paths), NULL);
}

Error vcsUnstage(const json::JsonRpcRequest& request,
                 json::JsonRpcResponse* pResponse)
{
   RefreshOnExit refreshOnExit;

   json::Array paths;
   Error error = json::readParam(request.params, 0, &paths);
   if (error)
      return error ;

   return s_pVcsImpl_->unstage(resolveAliasedPaths(paths, true, true), NULL);
}

Error vcsListBranches(const json::JsonRpcRequest& request,
                      json::JsonRpcResponse* pResponse)
{
   std::vector<std::string> branches;
   boost::optional<size_t> activeIndex;
   Error error = s_pVcsImpl_->listBranches(&branches, &activeIndex);
   if (error)
      return error;

   json::Array jsonBranches;
   std::transform(branches.begin(), branches.end(),
                  std::back_inserter(jsonBranches),
                  json::toJsonString);

   json::Object result;
   result["branches"] = jsonBranches;
   result["activeIndex"] =
         activeIndex
            ? json::Value(static_cast<boost::uint64_t>(activeIndex.get()))
            : json::Value();

   pResponse->setResult(result);

   return Success();
}

Error vcsCheckout(const json::JsonRpcRequest& request,
                  json::JsonRpcResponse* pResponse)
{
   RefreshOnExit refreshOnExit;

   std::string id;
   Error error = json::readParams(request.params, &id);
   if (error)
      return error;

   std::string handle;
   error = s_pVcsImpl_->checkout(id, &handle);
   if (error)
      return error;

   pResponse->setResult(handle);

   return Success();
}

Error vcsFullStatus(const json::JsonRpcRequest&,
                    json::JsonRpcResponse* pResponse)
{
   StatusResult statusResult;
   Error error = s_pVcsImpl_->status(s_pVcsImpl_->root(), &statusResult);
   if (error)
      return error;

   std::vector<FileWithStatus> files = statusResult.files();
   json::Array result;
   for (std::vector<FileWithStatus>::const_iterator it = files.begin();
        it != files.end();
        it++)
   {
      VCSStatus status = it->status;
      FilePath path = it->path;
      json::Object obj;
      error = statusToJson(path, status, &obj);
      if (error)
         return error;
      result.push_back(obj);
   }

   pResponse->setResult(result);

   return Success();
}

Error vcsAllStatus(const json::JsonRpcRequest& request,
                   json::JsonRpcResponse* pResponse)
{
   json::Object result;
   json::JsonRpcResponse tmp;

   Error error = vcsFullStatus(request, &tmp);
   if (error)
      return error;
   result["status"] = tmp.result();

   error = vcsListBranches(request, &tmp);
   if (error)
      return error;
   result["branches"] = tmp.result();

   bool hasRemote;
   error = s_pVcsImpl_->hasRemote(&hasRemote);
   if (error)
      return error;
   result["has_remote"] = hasRemote;

   pResponse->setResult(result);

   return Success();
}

Error vcsCommitGit(const json::JsonRpcRequest& request,
                   json::JsonRpcResponse* pResponse)
{
   RefreshOnExit refreshOnExit;

   GitVCSImpl* pGit = dynamic_cast<GitVCSImpl*>(s_pVcsImpl_.get());
   if (!pGit)
      return systemError(boost::system::errc::operation_not_supported, ERROR_LOCATION);

   std::string commitMsg;
   bool amend, signOff;
   Error error = json::readParams(request.params, &commitMsg, &amend, &signOff);
   if (error)
      return error;

   std::string handle;
   error = pGit->commit(commitMsg, amend, signOff, &handle);
   if (error)
      return error;

   pResponse->setResult(handle);

   return Success();
}

Error vcsClone(const json::JsonRpcRequest& request,
               json::JsonRpcResponse* pResponse)
{
   GitVCSImpl git(options().userHomePath());

   std::string vcsName;   // ignored for now
   std::string url;
   std::string dirName;   // ignored for now
   std::string parentDir;
   Error error = json::readObjectParam(request.params, 0,
                                       "vcs_name", &vcsName,
                                       "repo_url", &url,
                                       "directory_name", &dirName,
                                       "parent_path", &parentDir);

   FilePath parentPath = module_context::resolveAliasedPath(parentDir);

   std::string handle;
   error = git.clone(url, dirName, parentPath, &handle);
   if (error)
      return error;

   pResponse->setResult(handle);

   return Success();
}

Error vcsPush(const json::JsonRpcRequest& request,
              json::JsonRpcResponse* pResponse)
{
   GitVCSImpl* pGit = dynamic_cast<GitVCSImpl*>(s_pVcsImpl_.get());
   if (!pGit)
      return systemError(boost::system::errc::operation_not_supported, ERROR_LOCATION);

   std::string handle;
   Error error = pGit->push(&handle);
   if (error)
      return error;

   pResponse->setResult(handle);

   return Success();
}

Error vcsPull(const json::JsonRpcRequest& request,
              json::JsonRpcResponse* pResponse)
{
   GitVCSImpl* pGit = dynamic_cast<GitVCSImpl*>(s_pVcsImpl_.get());
   if (!pGit)
      return systemError(boost::system::errc::operation_not_supported, ERROR_LOCATION);

   std::string handle;
   Error error = pGit->pull(&handle);
   if (error)
      return error;

   pResponse->setResult(handle);

   return Success();
}

Error vcsDiffFile(const json::JsonRpcRequest& request,
                  json::JsonRpcResponse* pResponse)
{
   std::string path;
   int mode;
   int contextLines;
   bool noSizeWarning;
   Error error = json::readParams(request.params,
                                  &path,
                                  &mode,
                                  &contextLines,
                                  &noSizeWarning);
   if (error)
      return error;

   if (contextLines < 0)
      contextLines = 999999999;

   if (static_cast<PatchMode>(mode) == PatchModeStage)
      splitRename(path, &path, NULL);
   else
      splitRename(path, NULL, &path);

   std::string output;
   error = s_pVcsImpl_->diffFile(resolveAliasedPath(path),
                                 static_cast<PatchMode>(mode),
                                 contextLines,
                                 &output);
   if (error)
      return error;

   if (!noSizeWarning && output.size() > WARN_SIZE)
   {
      error = systemError(boost::system::errc::file_too_large,
                          ERROR_LOCATION);
      pResponse->setError(error,
                          json::Value(static_cast<uint64_t>(output.size())));
   }
   else
   {
      pResponse->setResult(output);
   }
   return Success();
}

Error vcsApplyPatch(const json::JsonRpcRequest& request,
                    json::JsonRpcResponse* pResponse)
{
   RefreshOnExit refreshOnExit;

   std::string patch;
   int mode;
   Error error = json::readParams(request.params, &patch, &mode);
   if (error)
      return error;

   FilePath patchFile = module_context::tempFile("rstudiovcs", "patch");
   error = writeStringToFile(patchFile, patch);
   if (error)
      return error;

   error = s_pVcsImpl_->applyPatch(patchFile, static_cast<PatchMode>(mode), NULL);

   Error error2 = patchFile.remove();
   if (error2)
      LOG_ERROR(error2);

   if (error)
      return error;

   return Success();
}

Error vcsHistoryCount(const json::JsonRpcRequest& request,
                 json::JsonRpcResponse* pResponse)
{
   std::string rev, filterText;
   Error error = json::readParams(request.params, &rev, &filterText);
   if (error)
      return error;

   boost::algorithm::trim(filterText);

   int count;
   error = s_pVcsImpl_->logLength(rev, filterText, &count);
   if (error)
      return error;

   json::Object result;
   result["count"] = count;
   pResponse->setResult(result);

   return Success();
}

Error vcsHistory(const json::JsonRpcRequest& request,
                 json::JsonRpcResponse* pResponse)
{
   std::string rev, filterText;
   int skip, maxentries;
   Error error = json::readParams(request.params, &rev, &skip, &maxentries,
                                  &filterText);
   if (error)
      return error;

   boost::algorithm::trim(filterText);

   std::vector<CommitInfo> commits;
   error = s_pVcsImpl_->log(rev, skip, maxentries, filterText, &commits);
   if (error)
      return error;

   json::Array ids;
   json::Array authors;
   json::Array parents;
   json::Array subjects;
   json::Array dates;
   json::Array descriptions;
   json::Array refs;
   json::Array tags;
   json::Array graphs;

   for (std::vector<CommitInfo>::const_iterator it = commits.begin();
        it != commits.end();
        it++)
   {
      ids.push_back(it->id.substr(0, 8));
      authors.push_back(string_utils::filterControlChars(it->author));
      parents.push_back(string_utils::filterControlChars(it->parent));
      subjects.push_back(string_utils::filterControlChars(it->subject));
      descriptions.push_back(string_utils::filterControlChars(it->description));
      dates.push_back(static_cast<double>(it->date));
      graphs.push_back(it->graph);

      json::Array theseRefs;
      std::copy(it->refs.begin(), it->refs.end(), std::back_inserter(theseRefs));
      refs.push_back(theseRefs);

      json::Array theseTags;
      std::copy(it->tags.begin(), it->tags.end(), std::back_inserter(theseTags));
      tags.push_back(theseTags);
   }

   json::Object result;
   result["id"] = ids;
   result["author"] = authors;
   result["parent"] = parents;
   result["subject"] = subjects;
   result["description"] = descriptions;
   result["date"] = dates;
   result["refs"] = refs;
   result["tags"] = tags;
   result["graph"] = graphs;

   pResponse->setResult(result);

   return Success();
}

Error vcsExecuteCommand(const json::JsonRpcRequest& request,
                        json::JsonRpcResponse* pResponse)
{
   std::string command;
   Error error = json::readParams(request.params, &command);
   if (error)
      return error;

   command = shell_utils::sendStdErrToStdOut(shell_utils::join(
         ShellCommand("cd") << s_pVcsImpl_->root(),
         command));

   boost::shared_ptr<ConsoleProcess> ptrProc =
         console_process::ConsoleProcess::create(command,
                                                 procOptions(),
                                                 command,
                                                 false,
                                                 &enqueueRefreshEvent);

   pResponse->setResult(ptrProc->handle());
   return Success();
}

Error vcsShow(const json::JsonRpcRequest& request,
              json::JsonRpcResponse* pResponse)
{
   std::string rev;
   bool noSizeWarning;
   Error error = json::readParams(request.params, &rev, &noSizeWarning);
   if (error)
      return error;

   std::string output;
   s_pVcsImpl_->show(rev, &output);
   output = string_utils::filterControlChars(output);

   if (!noSizeWarning && output.size() > WARN_SIZE)
   {
      error = systemError(boost::system::errc::file_too_large,
                          ERROR_LOCATION);
      pResponse->setError(error,
                          json::Value(static_cast<uint64_t>(output.size())));
   }
   else
   {
      pResponse->setResult(output);
   }
   return Success();
}

Error vcsSshPublicKey(const json::JsonRpcRequest& request,
                      json::JsonRpcResponse* pResponse)
{
   // get public key path
   std::string aliasedPath;
   Error error = json::readParam(request.params, 0, &aliasedPath);
   if (error)
      return error;

   // unalias it and check for existence
   FilePath publicKeyPath = module_context::resolveAliasedPath(aliasedPath);
   if (!publicKeyPath.exists())
   {
      return core::fileNotFoundError(publicKeyPath.absolutePath(),
                                     ERROR_LOCATION);
   }

   // read the key
   std::string publicKeyContents;
   error = core::readStringFromFile(publicKeyPath, &publicKeyContents);
   if (error)
      return error;

   pResponse->setResult(publicKeyContents);

   return Success();
}

Error vcsCreateSshKey(const json::JsonRpcRequest& request,
                      json::JsonRpcResponse* pResponse)
{
   std::string path, type, passphrase;
   Error error = json::readObjectParam(request.params, 0,
                                       "path", &path,
                                       "type", &type,
                                       "passphrase", &passphrase);
   if (error)
      return error;

#ifdef RSTUDIO_SERVER
   // In server mode, passphrases are encrypted
   using namespace core::system::crypto;
   error = rsaPrivateDecrypt(passphrase, &passphrase);
   if (error)
      return error;
#endif

   // verify that the path doesn't already exist
   FilePath sshKeyPath = module_context::resolveAliasedPath(path);
   FilePath sshPublicKeyPath = sshKeyPath.parent().complete(
                                             sshKeyPath.stem() + ".pub");
   if (sshKeyPath.exists() || sshPublicKeyPath.exists())
   {
      json::Object resultJson;
      resultJson["failed_key_exists"] = true;
      pResponse->setResult(resultJson);
      return Success();
   }

   // compose a shell command to create the key
   ShellCommand cmd("ssh-keygen");

   // type
   cmd << "-t" << type;

   // passphrase (optional)
   cmd << "-N";
   if (!passphrase.empty())
      cmd << passphrase;
   else
      cmd << std::string("");

   // path
   cmd << "-f" << sshKeyPath;

   // run it
   core::system::ProcessResult result;
   error = runCommand(shell_utils::sendStdErrToStdOut(cmd),
                      procOptions(),
                      &result);
   if (error)
      return error;

   // return exit code and output
   json::Object resultJson;
   resultJson["failed_key_exists"] = false;
   resultJson["exit_status"] = result.exitStatus;
   resultJson["output"] = result.stdOut;
   pResponse->setResult(resultJson);
   return Success();
}


std::string toBashPath(const std::string& path)
{
#ifdef _WIN32
   std::string result(path);
   for (std::string::iterator it = result.begin();
        it != result.end();
        it++)
   {
      if (*it == '\\')
         *it = '/';
   }

   return boost::regex_replace(result, boost::regex("^([A-Za-z]):"), "/\\1");
#else
   return path;
#endif
}

FilePath getTrueHomeDir()
{
#if _WIN32
   // On Windows, R's idea of "$HOME" is not, by default, the same as
   // $USERPROFILE, which is what we want for ssh purposes
   return FilePath(string_utils::systemToUtf8(core::system::getenv("USERPROFILE")));
#else
   return FilePath(string_utils::systemToUtf8(core::system::getenv("HOME")));
#endif
}

// This is necessary to hook up the completion of ssh-add (which we
// must run asynchronously) with the continuation passed into
// postbackGitSSH. It's important that we pass the output along, as
// it may contain the output from ssh-agent that is needed by the
// postback-gitssh script.
void postbackGitSSH_onSSHAddComplete(
      const module_context::PostbackHandlerContinuation& cont,
      const std::string& output,
      const core::system::ProcessResult& result)
{
   cont(result.exitStatus, output);
}

// If we fail to initialize ssh-agent, don't attempt to do it again
bool s_suppressSshAgentStart = false;

// This is a hook that runs whenever git calls into ssh. We prepare
// ssh-agent and add the ssh key path, which may involve
// prompting the user (a complex process which involves $SSH_ASKPASS
// and rpostback-askpass, and calling into the client to show the
// passphrase prompt).
//
// This function uses continuation-passing-style, which means the
// "cont" param is a function that MUST be called. Failure to call
// the param means the CPS operation will be stuck forever.
//
// TODO: Lots of failure modes here, all of which will *probably*
// result in the user not succeeding in whatever they were trying
// to do. Think about how to let them know what went wrong and how
// they can go about making things work.
void postbackGitSSH(const std::string& argument,
                    const module_context::PostbackHandlerContinuation& cont)
{
   using namespace core::system;

   FilePath key = source_control::verifiedSshKeyPath();
   if (!key.exists())
   {
      // No default key, we're not going to know how to call ssh-add. Give up.
      cont(EXIT_FAILURE, "");
      return;
   }

   if (s_suppressSshAgentStart)
   {
      // I failed in an earlier attempt and told myself not to try again.
      cont(EXIT_FAILURE, "");
      return;
   }

   boost::shared_ptr<std::istream> pKeyStream;
   Error error = key.open_r(&pKeyStream);
   if (error)
   {
      LOG_ERROR(error);
      // OK to continue in this case.
   }
   else
   {
      std::vector<char> buffer(300);
      pKeyStream->read(&(buffer[0]), buffer.capacity());
      if (!pKeyStream->fail())
      {
         buffer.resize(pKeyStream->gcount());
         std::string data(buffer.begin(), buffer.end());
         if (data.find("ENCRYPTED") == std::string::npos)
         {
            // The key was found to be unencrypted
            cont(EXIT_SUCCESS, "");
            return;
         }
      }
   }

   // Use "ssh-add -l" to see if ssh-agent is running
   ProcessResult result;
   error = runCommand(shell_utils::sendStdErrToNull("ssh-add -l"),
                      procOptions(), &result);
   if (error)
   {
      // We couldn't even launch ssh-add. Seems unlikely we'll be able to
      // in the future.
      s_suppressSshAgentStart = true;
      LOG_ERROR(error);
      cont(EXIT_FAILURE, "");
      return;
   }

   bool startSshAgent;
   bool runSshAdd;
   if (result.exitStatus == 1)
   {
      // exitStatus == 1 means ssh-agent was running but no identities were
      // present.
      startSshAgent = false;
      runSshAdd = true;
   }
   else if (result.exitStatus == EXIT_SUCCESS)
   {
      startSshAgent = false;
      std::string keyPathSys =
            core::string_utils::utf8ToSystem(toBashPath(key.absolutePath()));
      runSshAdd = result.stdOut.find(keyPathSys) == std::string::npos;
   }
   else
   {
      startSshAgent = true;
      runSshAdd = true;
   }

   // This will hold the values we eventually will write to stdout by
   // passing it into the continuation--specifically, the output of ssh-agent.
   std::ostringstream output;

   if (startSshAgent)
   {
      // Start ssh-agent using bash-style output
      error = runCommand("ssh-agent -s", procOptions(), &result);
      if (error)
      {
         // Failed to start ssh-agent, give up.
         s_suppressSshAgentStart = true;
         LOG_ERROR(error);
         cont(EXIT_FAILURE, "");
         return;
      }
      if (result.exitStatus != EXIT_SUCCESS)
      {
         // Failed to start ssh-agent, give up.
         s_suppressSshAgentStart = true;
         cont(result.exitStatus, "");
         return;
      }

      // ssh-agent succeeded, so capture its output
      output << result.stdOut;

      // In addition to dumping the ssh-agent output, we also need to parse
      // it so we can modify rsession's environment to use the new ssh-agent
      // as well.
      boost::sregex_iterator it(result.stdOut.begin(), result.stdOut.end(),
                                boost::regex("^([A-Za-z0-9_]+)=([^;]+);"));
      boost::sregex_iterator end;
      for (; it != end; it++)
      {
         std::string name = (*it).str(1);
         std::string value = (*it).str(2);
         core::system::setenv(name, value);

         if (name == "SSH_AGENT_PID")
         {
            int pid = safe_convert::stringTo<int>(value, 0);
            if (pid)
               s_pidsToTerminate_.push_back(pid);
         }
      }
   }

   if (runSshAdd)
   {
      // Finally, call ssh-add, which will (probably) use git-
      ShellCommand cmd("ssh-add");
#ifdef __APPLE__
      // Automatically use Keychain for passphrase
      cmd << "-k";
#endif
      cmd << string_utils::utf8ToSystem(toBashPath(key.absolutePath()));
      module_context::processSupervisor().runCommand(
            shell_utils::sendAllOutputToNull(shell_utils::sendNullToStdIn(cmd)),
            "", procOptions(),
            boost::bind(postbackGitSSH_onSSHAddComplete,
                        cont,
                        output.str(),
                        _1));
   }
   else
   {
      cont(EXIT_SUCCESS, output.str());
      return;
   }
}


module_context::WaitForMethodFunction s_waitForAskPass;

void postbackSSHAskPass(const std::string&,
                        const module_context::PostbackHandlerContinuation& cont)
{
   // default to failure unless we successfully receive a passphrase
   int retcode = EXIT_FAILURE;
   std::string passphrase;

   // wait for method
   core::json::JsonRpcRequest request;
   if (s_waitForAskPass(&request))
   {
      json::Value value;
      Error error = json::readParams(request.params, &value);
      if (!error)
      {
         if (json::isType<std::string>(value))
         {
            passphrase = value.get_value<std::string>();
            retcode = EXIT_SUCCESS;

#ifdef RSTUDIO_SERVER
            if (options().programMode() == kSessionProgramModeServer)
            {
               // In server mode, passphrases are encrypted

               error = core::system::crypto::rsaPrivateDecrypt(passphrase,
                                                               &passphrase);
               if (error)
               {
                  passphrase.clear();
                  retcode = EXIT_FAILURE;
                  LOG_ERROR(error);
               }
            }
#endif
         }
      }
      else
      {
         LOG_ERROR(error);
      }
   }

   // satisfy continuation
   cont(retcode, passphrase);
}

#ifdef _WIN32

template <typename T>
class AutoRelease
{
public:
   AutoRelease(T* pUnk) : pUnk_(pUnk)
   {
   }

   ~AutoRelease()
   {
      if (pUnk_)
         pUnk_->Release();
   }

private:
   T* pUnk_;
};


bool detectGitExeDirOnPath(FilePath* pPath)
{
   std::vector<wchar_t> path(MAX_PATH+2);
   wcscpy(&(path[0]), L"git.exe");
   if (::PathFindOnPathW(&(path[0]), NULL))
   {
      *pPath = FilePath(&(path[0])).parent();
      return true;
   }
   else
   {
      return false;
   }
}

bool isGitExeOnPath()
{
   FilePath gitExeDir;
   return detectGitExeDirOnPath(&gitExeDir);
}


bool detectGitBinDirFromPath(FilePath* pPath)
{
   std::vector<wchar_t> path(MAX_PATH+2);
   wcscpy(&(path[0]), L"git.cmd");

   if (::PathFindOnPathW(&(path[0]), NULL))
   {
      *pPath = FilePath(&(path[0])).parent().parent().childPath("bin");
      return true;
   }

   return false;
}

HRESULT detectGitBinDirFromShortcut(FilePath* pPath)
{
   using namespace boost;

   CoInitialize(NULL);

   // Step 1. Find the Git Bash shortcut on the Start menu
   std::vector<wchar_t> data(MAX_PATH+2);
   HRESULT hr = ::SHGetFolderPathW(NULL,
                                   CSIDL_COMMON_PROGRAMS,
                                   NULL,
                                   SHGFP_TYPE_CURRENT,
                                   &(data[0]));
   if (FAILED(hr))
      return hr;

   std::wstring path(&(data[0]));
   path.append(L"\\Git\\Git Bash.lnk");
   if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
      return E_FAIL;


   // Step 2. Extract the argument from the Git Bash shortcut
   IShellLinkW* pShellLink;
   hr = CoCreateInstance(CLSID_ShellLink,
                         NULL,
                         CLSCTX_INPROC_SERVER,
                         IID_IShellLinkW,
                         (void**)&pShellLink);
   if (FAILED(hr))
      return hr;
   AutoRelease<IShellLinkW> arShellLink(pShellLink);

   IPersistFile* pPersistFile;
   hr = pShellLink->QueryInterface(IID_IPersistFile, (void**)&pPersistFile);
   if (FAILED(hr))
      return hr;
   AutoRelease<IPersistFile> arPersistFile(pPersistFile);

   pPersistFile->Load(path.c_str(), STGM_READ);
   if (FAILED(hr))
      return hr;

   hr = pShellLink->Resolve(NULL, SLR_NO_UI | 0x10000);
   if (FAILED(hr))
      return hr;
   std::vector<wchar_t> argbuff(1024);
   hr = pShellLink->GetArguments(&(argbuff[0]), argbuff.capacity() - 1);
   if (FAILED(hr))
      return hr;


   // Step 3. Extract the git/bin directory from the arguments.
   // Example: /c ""C:\Program Files\Git\bin\sh.exe" --login -i"
   wcmatch match;
   if (!regex_search(&(argbuff[0]), match, wregex(L"\"\"([^\"]*)\"")))
   {
      LOG_ERROR_MESSAGE("Unexpected git bash argument format: " +
                        string_utils::wideToUtf8(&(argbuff[0])));
      return E_FAIL;
   }

   *pPath = FilePath(match[1]);
   if (!pPath->exists())
      return E_FAIL;
   // The path we have is to sh.exe, we want the parent
   *pPath = pPath->parent();
   if (!pPath->exists())
      return E_FAIL;

   return S_OK;
}

Error discoverGitBinDir(FilePath* pPath)
{
   if (detectGitBinDirFromPath(pPath))
      return Success();

   HRESULT hr = detectGitBinDirFromShortcut(pPath);
   if (SUCCEEDED(hr))
      return Success();

   return systemError(boost::system::errc::no_such_file_or_directory,
                      ERROR_LOCATION);
}

Error detectAndSaveGitBinDir()
{
   if (isGitExeOnPath())
      return Success();

   FilePath path;
   Error error = discoverGitBinDir(&path);
   if (error)
      return error;

   // save it
   s_gitBinDir = path.absolutePath();

   return Success();
}


#endif

void onShutdown(bool)
{
   std::for_each(s_pidsToTerminate_.begin(), s_pidsToTerminate_.end(),
                 &core::system::terminateProcess);
   s_pidsToTerminate_.clear();
}

Error addFilesToGitIgnore(const FilePath& gitIgnoreFile,
                          const std::vector<std::string>& filesToIgnore,
                          bool addExtraNewline)
{
   if (filesToIgnore.empty())
      return Success();

   boost::shared_ptr<std::ostream> ptrOs;
   Error error = gitIgnoreFile.open_w(&ptrOs, false);
   if (error)
      return error;

   ptrOs->seekp(0, std::ios::end);
   if (ptrOs->good())
   {
      if (addExtraNewline)
         *ptrOs << std::endl;

      BOOST_FOREACH(const std::string& line, filesToIgnore)
      {
         *ptrOs << line << std::endl;
      }

      ptrOs->flush();
   }

   return Success();
}

Error augmentGitIgnore(const FilePath& gitIgnoreFile)
{
   // Add stuff to .gitignore
   std::vector<std::string> filesToIgnore;
   if (!gitIgnoreFile.exists())
   {
      // If no .gitignore exists, add this stuff
      filesToIgnore.push_back(".Rproj.user");
      filesToIgnore.push_back(".Rhistory");
      filesToIgnore.push_back(".RData");
      return addFilesToGitIgnore(gitIgnoreFile, filesToIgnore, false);
   }
   else
   {
      // If .gitignore exists, add .Rproj.user unless it's already there

      std::string strIgnore;
      Error error = core::readStringFromFile(gitIgnoreFile, &strIgnore);
      if (error)
         return error;

      if (boost::regex_search(strIgnore, boost::regex("^\\.Rproj\\.user$")))
         return Success();

      bool addExtraNewline = strIgnore.size() > 0
                             && strIgnore[strIgnore.size() - 1] != '\n';

      std::vector<std::string> filesToIgnore;
      filesToIgnore.push_back(".Rproj.user");
      return addFilesToGitIgnore(gitIgnoreFile,
                                 filesToIgnore,
                                 addExtraNewline);
   }
}

FilePath whichGitExe()
{
   std::string whichGit;
   Error error = r::exec::RFunction("Sys.which", "git").call(&whichGit);
   if (error)
   {
      LOG_ERROR(error);
      return FilePath();
   }
   else
   {
      return FilePath(whichGit);
   }
}

} // anonymous namespace

bool isGitInstalled()
{
   if (!userSettings().vcsEnabled())
      return false;

   core::system::ProcessResult result;
   Error error = core::system::runCommand(git() << "--version",
                                          procOptions(),
                                          &result);
   if (error)
      return false;
   return result.exitStatus == EXIT_SUCCESS;
}

FilePath detectedGitBinDir()
{
#ifdef _WIN32
   FilePath path;
   if (detectGitExeDirOnPath(&path))
   {
      return path;
   }
   else
   {
      Error error = discoverGitBinDir(&path);
      if (!error)
      {
         return path;
      }
      else
      {
         LOG_ERROR(error);
         return FilePath();
      }
   }
#else
   FilePath gitExeFilePath = whichGitExe();
   if (!gitExeFilePath.empty())
      return FilePath(gitExeFilePath).parent();
   else
      return FilePath();
#endif
}

// in RStudio Server mode ensure that there are no group or other permissions
// set for the ssh key (ssh will fail if there are). we do this automagically
// in server mode because often users will upload their ssh keys into the
// .ssh folder (resulting in default permissions) and then still be in an
// inoperable state. the users could figure out how to do system("chmod ...")
// but many will probably end up getting stimied before trying that. we don't
// consider this intrusive because we are resetting the permissions to what
// they would be if the user called ssh-keygen directory and we can't think
// of any good reason why you'd want an ssh key with incorrect/inoperable
// permissions set on it
void ensureCorrectPermissions(const FilePath& sshKeyPath)
{
#ifdef RSTUDIO_SERVER
   const char * path = sshKeyPath.absolutePath().c_str();
   struct stat st;
   if (::stat(path, &st) == -1)
   {
      LOG_ERROR(systemError(errno, ERROR_LOCATION));
      return;
   }

   // check if there are any permissions for group or other defined. if
   // there are then remove them
   mode_t mode = st.st_mode;
   if (mode & S_IRWXG || mode & S_IRWXO)
   {
      mode &= ~S_IRWXG;
      mode &= ~S_IRWXO;
      core::system::safePosixCall<int>(boost::bind(::chmod, path, mode),
                                       ERROR_LOCATION);
   }
#endif
}


FilePath verifiedSshKeyPath()
{
   // if there is user override first try that -- if the override is
   // specified but doesn't exit then advance to auto-resolution logic
   std::string sskKeyPathSetting = userSettings().sshKeyPath();
   FilePath sshKeyPath;
   if (!sskKeyPathSetting.empty())
   {
      sshKeyPath = module_context::resolveAliasedPath(sskKeyPathSetting);
      if (!sshKeyPath.exists())
         sshKeyPath = FilePath();
   }

   // if there isn't a valid user specified default then scan known locations
   if (sshKeyPath.empty())
   {
      FilePath sshKeyDir = defaultSshKeyPath();
      std::vector<FilePath> candidatePaths;
      candidatePaths.push_back(sshKeyDir.childPath("id_rsa"));
      candidatePaths.push_back(sshKeyDir.childPath("id_dsa"));
      candidatePaths.push_back(sshKeyDir.childPath("identity"));
      BOOST_FOREACH(const FilePath& path, candidatePaths)
      {
         if (path.exists())
         {
            sshKeyPath = path;
            break;
         }
      }
   }

   // ensure permissions if we have a path to return
   if (!sshKeyPath.empty())
   {
      ensureCorrectPermissions(sshKeyPath);
      return sshKeyPath;
   }
   else
   {
      return FilePath();
   }
}


FilePath defaultSshKeyPath()
{
   return getTrueHomeDir().childPath(".ssh");
}

void onUserSettingsChanged()
{
   FilePath gitBinDir = userSettings().gitBinDir();
   if (!gitBinDir.empty())
   {
      // if there is an explicit value then set it
      s_gitBinDir = gitBinDir.absolutePath();
   }
   else
   {
      // if we are relying on an auto-detected value then scan on windows
      // and reset to empty on posix
#ifdef _WIN32
      Error error = detectAndSaveGitBinDir();
      if (error)
         LOG_ERROR(error);
#else
      s_gitBinDir = "";
#endif
   }
}

bool isSvnInstalled()
{
   if (!userSettings().vcsEnabled())
      return false;

   // TODO
   return false;
}

Error statusToJson(const core::FilePath &path,
                   const VCSStatus &status,
                   core::json::Object *pObject)
{
   json::Object& obj = *pObject;
   obj["status"] = status.status();
   obj["path"] = path.relativePath(s_pVcsImpl_->root());
   obj["raw_path"] = path.absolutePath();
   obj["discardable"] = status.status()[1] != ' ' && status.status()[1] != '?';
   return Success();
}

void onSuspend(core::Settings*)
{
}

void onResume(const core::Settings&)
{
   enqueueRefreshEvent();
}

bool tryGit(const FilePath& workingDir)
{
   // get the git bin dir from settings if it is there
   s_gitBinDir = userSettings().gitBinDir().absolutePath();

   // if it wasn't provided in settings then make sure we can detect it
   if (s_gitBinDir.empty())
   {
#ifdef _WIN32
      Error error = detectAndSaveGitBinDir();
      if (error)
         return false; // no Git install detected
#else
      FilePath gitExeFilePath = whichGitExe();
      if (gitExeFilePath.empty())
         return false; // no Git install detected
#endif
   }

   FilePath gitDir = GitVCSImpl::detectGitDir(workingDir);
   if (gitDir.empty())
      return false;

   s_pVcsImpl_.reset(new GitVCSImpl(GitVCSImpl::detectGitDir(workingDir)));

   FilePath gitIgnore = s_pVcsImpl_->root().childPath(".gitignore");
   Error error = augmentGitIgnore(gitIgnore);
   if (error)
      LOG_ERROR(error);

   // Save version
   s_gitVersion = GIT_1_7_2;
   core::system::ProcessResult result;
   error = core::system::runCommand(git() << "--version",
                                    procOptions(),
                                    &result);
   if (error)
      LOG_ERROR(error);
   else
   {
      if (result.exitStatus == 0)
      {
         boost::smatch matches;
         if (boost::regex_search(result.stdOut,
                                 matches,
                                 boost::regex("\\d+(\\.\\d+)+")))
         {
            string_utils::parseVersion(matches[0], &s_gitVersion);
         }
      }
   }

   return true;
}

core::Error initialize()
{
   using namespace session::module_context;

   Error error;

   module_context::events().onShutdown.connect(onShutdown);

   FilePath workingDir = projects::projectContext().directory();
   if (!userSettings().vcsEnabled())
      s_pVcsImpl_.reset(new VCSImpl(workingDir));
   else if (workingDir.empty())
      s_pVcsImpl_.reset(new VCSImpl(workingDir));
   else if (tryGit(workingDir))
   {
      // Tntentionally blank. tryGit() has side effects.
   }
   else if (workingDir.childPath(".svn").isDirectory())
      s_pVcsImpl_.reset(new SubversionVCSImpl(workingDir));
   else
      s_pVcsImpl_.reset(new VCSImpl(workingDir));

   bool interceptSsh;
   bool interceptAskPass;

   if (options().programMode() == kSessionProgramModeServer)
   {
      interceptSsh = true;
      interceptAskPass = true;
   }
   else
   {
#ifdef _WIN32
      // Windows probably unlikely to have either ssh-agent or askpass
      interceptSsh = true;
      interceptAskPass = true;
#else
      // Everything fine on Mac and Linux
      interceptSsh = false;
      interceptAskPass = false;
#endif
   }

   if (interceptSsh)
   {
      std::string gitSshCmd;
      error = module_context::registerPostbackHandler("gitssh",
                                                      postbackGitSSH,
                                                      &gitSshCmd);
      if (error)
         return error;
      BOOST_ASSERT(boost::algorithm::ends_with(gitSshCmd, "rpostback-gitssh"));
      core::system::setenv("GIT_SSH", "rpostback-gitssh");
   }

   if (interceptAskPass)
   {
      // register postback handler
      std::string sshAskCmd;
      error = module_context::registerPostbackHandler("askpass",
                                                      postbackSSHAskPass,
                                                      &sshAskCmd);
      if (error)
         return error;

      // register waitForMethod handler
      json::Object payload;
      payload["prompt"] = std::string("Enter passphrase:");
      ClientEvent askPassEvent(client_events::kAskPass, payload);
      s_waitForAskPass = module_context::registerWaitForMethod(
                                                   "askpass_completed",
                                                   askPassEvent);

      // setup environment
      BOOST_ASSERT(boost::algorithm::ends_with(sshAskCmd, "rpostback-askpass"));
      core::system::setenv("SSH_ASKPASS", "rpostback-askpass");
      core::system::setenv("GIT_ASKPASS", "rpostback-askpass");
   }

   // add suspend/resume handler
   addSuspendHandler(SuspendHandler(onSuspend, onResume));

   // add settings changed handler
   userSettings().onChanged.connect(onUserSettingsChanged);

   // install rpc methods
   using boost::bind;
   using namespace module_context;
   ExecBlock initBlock ;
   initBlock.addFunctions()
      (bind(registerRpcMethod, "vcs_add", vcsAdd))
      (bind(registerRpcMethod, "vcs_remove", vcsRemove))
      (bind(registerRpcMethod, "vcs_discard", vcsDiscard))
      (bind(registerRpcMethod, "vcs_revert", vcsRevert))
      (bind(registerRpcMethod, "vcs_stage", vcsStage))
      (bind(registerRpcMethod, "vcs_unstage", vcsUnstage))
      (bind(registerRpcMethod, "vcs_list_branches", vcsListBranches))
      (bind(registerRpcMethod, "vcs_checkout", vcsCheckout))
      (bind(registerRpcMethod, "vcs_full_status", vcsFullStatus))
      (bind(registerRpcMethod, "vcs_all_status", vcsAllStatus))
      (bind(registerRpcMethod, "vcs_commit_git", vcsCommitGit))
      (bind(registerRpcMethod, "vcs_clone", vcsClone))
      (bind(registerRpcMethod, "vcs_push", vcsPush))
      (bind(registerRpcMethod, "vcs_pull", vcsPull))
      (bind(registerRpcMethod, "vcs_diff_file", vcsDiffFile))
      (bind(registerRpcMethod, "vcs_apply_patch", vcsApplyPatch))
      (bind(registerRpcMethod, "vcs_history_count", vcsHistoryCount))
      (bind(registerRpcMethod, "vcs_history", vcsHistory))
      (bind(registerRpcMethod, "vcs_execute_command", vcsExecuteCommand))
      (bind(registerRpcMethod, "vcs_show", vcsShow))
      (bind(registerRpcMethod, "vcs_ssh_public_key", vcsSshPublicKey))
      (bind(registerRpcMethod, "vcs_create_ssh_key", vcsCreateSshKey));
   error = initBlock.execute();
   if (error)
      return error;

   return Success();
}

} // namespace source_control
} // namespace modules
} // namespace session
