/*
 * Win32FileMonitor.cpp
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

// TODO: clean shutdown logic (confirm boost thread interrupt works and
// figure out how to wait on pending io operations)

// TODO: convert to short file name (docs say it could be short or long!)

// TODO: reconciliation / event-generation logic

// TODO: review code and comments for other egde/boundary conditions

// TODO: test error reporting


#include <core/system/FileMonitor.hpp>

#include <windows.h>

#include <memory>

#include <boost/algorithm/string/trim.hpp>
#include <boost/algorithm/string/classification.hpp>

#include <core/FilePath.hpp>

#include <core/system/FileScanner.hpp>
#include <core/system/System.hpp>

#include "FileMonitorImpl.hpp"


namespace core {
namespace system {
namespace file_monitor {

namespace {

// buffer size for notifications (cannot be > 64kb for network drives)
const std::size_t kBuffSize = 32768;

class FileEventContext : boost::noncopyable
{
public:
   FileEventContext()
      : hDirectory(NULL)
   {
      receiveBuffer.resize(kBuffSize);
      handlingBuffer.resize(kBuffSize);
   }
   virtual ~FileEventContext() {}

   std::wstring path;
   HANDLE hDirectory;
   OVERLAPPED overlapped;
   std::vector<BYTE> receiveBuffer;
   std::vector<BYTE> handlingBuffer;
   tree<FileInfo> fileTree;
   Callbacks::ReportError onMonitoringError;
   Callbacks::FilesChanged onFilesChanged;
};

void closeDirectoryHandle(HANDLE hDirectory)
{
   if (!::CloseHandle(hDirectory))
      LOG_ERROR(systemError(::GetLastError(), ERROR_LOCATION));
}

void cleanupContext(FileEventContext* pContext)
{
   if (pContext->hDirectory != NULL)
   {
      if (!::CancelIo(pContext->hDirectory))
         LOG_ERROR(systemError(::GetLastError(), ERROR_LOCATION));

      closeDirectoryHandle(pContext->hDirectory);

      pContext->hDirectory = NULL;
   }
}

void removeTrailingSlash(std::wstring* pPath)
{
   boost::algorithm::trim_right_if(*pPath, boost::algorithm::is_any_of(L"\\"));
}

void processFileChanges(FileEventContext* pContext)
{
   char* pBuffer = (char*)&pContext->handlingBuffer[0];

   while(true)
   {
      // get file notify struct
      FILE_NOTIFY_INFORMATION& fileNotify = (FILE_NOTIFY_INFORMATION&)*pBuffer;

      // compute a full wide path
      std::wstring name(fileNotify.FileName,
                        fileNotify.FileNameLength/sizeof(wchar_t));
      removeTrailingSlash(&name);
      std::wstring path = pContext->path + L'\\' + name;

      // TODO: convert to short file name (docs say it could be short or long!)


      // break or advance to next notification as necessary
      if (!fileNotify.NextEntryOffset)
         break;

      pBuffer += fileNotify.NextEntryOffset;
   };

}

// track number of active requests (we wait for this to get to zero before
// allowing the exit of the monitoring thread -- this ensures that we have
// performed full cleanup for all monitoring contexts before exiting.
volatile LONG s_activeRequests = 0;

Error readDirectoryChanges(FileEventContext* pContext);

VOID CALLBACK FileChangeCompletionRoutine(DWORD dwErrorCode,									// completion code
                                          DWORD dwNumberOfBytesTransfered,
                                          LPOVERLAPPED lpOverlapped)
{
   // get the context
   FileEventContext* pContext = (FileEventContext*)(lpOverlapped->hEvent);

   // check for aborted
   if (dwErrorCode == ERROR_OPERATION_ABORTED)
   {
      // decrement the active request counter
      ::InterlockedDecrement(&s_activeRequests);

      // we wait to delete the pContext until here because it owns the
      // OVERLAPPED structure and buffers, and so if we deleted it earlier
      // and the OS tried to access those memory regions we would crash
      delete pContext; 
      return;
   }

   // bail if we don't have an onFilesChanged callback (could have occurred
   // if we encountered an error during file scanning which caused us to
   // fail but then a file notification still snuck through)
   if (!pContext->onFilesChanged)
      return;

   // check for buffer overflow
   if(dwNumberOfBytesTransfered == 0)
   {

      // TODO: full rescan of directory goes here

      return;
   }

   // copy to processing buffer (so we can immediately begin another read)
   ::CopyMemory(&(pContext->handlingBuffer),
                &(pContext->receiveBuffer),
                dwNumberOfBytesTransfered);

   // begin the next read -- if this fails then the file change notification
   // is effectively dead in the water
   Error error = readDirectoryChanges(pContext);

   // process file changes
   processFileChanges(pContext);

   // report the (fatal) error if necessary and unregister the monitor
   // (do this here so file notifications are received prior to the error)
   if (error)
   {
      // report the error
      pContext->onMonitoringError(error);

      // unregister the monitor
      file_monitor::unregisterMonitor((Handle)pContext);
   }
}

Error readDirectoryChanges(FileEventContext* pContext)
{
   DWORD dwBytes = 0;
   if(!::ReadDirectoryChangesW(pContext->hDirectory,
                               &(pContext->receiveBuffer[0]),
                               pContext->receiveBuffer.size(),
                               TRUE,
                               FILE_NOTIFY_CHANGE_LAST_WRITE |
                               FILE_NOTIFY_CHANGE_CREATION |
                               FILE_NOTIFY_CHANGE_FILE_NAME,
                               &dwBytes,
                               &(pContext->overlapped),
                               &FileChangeCompletionRoutine))
   {
      return systemError(::GetLastError(), ERROR_LOCATION);
   }
   else
   {
      return Success();
   }
}

} // anonymous namespace

namespace detail {

// register a new file monitor
Handle registerMonitor(const core::FilePath& filePath,
                       bool recursive,
                       const Callbacks& callbacks)
{
   // create and allocate FileEventContext (create auto-ptr in case we
   // return early, we'll call release later before returning)
   FileEventContext* pContext = new FileEventContext();
   std::auto_ptr<FileEventContext> autoPtrContext(pContext);

   // save the wide absolute path (notifications only come in wide strings)
   // strip any trailing slash for predictable append semantics
   pContext->path = filePath.absolutePathW();
   removeTrailingSlash(&(pContext->path));

   // open the directory
   pContext->hDirectory = ::CreateFile(
                     filePath.absolutePath().c_str(),
                     FILE_LIST_DIRECTORY,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     NULL,
                     OPEN_EXISTING,
                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                     NULL);
   if (pContext->hDirectory == INVALID_HANDLE_VALUE)
   {
      callbacks.onRegistrationError(
                     systemError(::GetLastError(),ERROR_LOCATION));
      return NULL;
   }



   // initialize overlapped structure to point to our context
   ::ZeroMemory(&(pContext->overlapped), sizeof(OVERLAPPED));
   pContext->overlapped.hEvent = pContext;

   // get the monitoring started
   Error error = readDirectoryChanges(pContext);
   if (error)
   {
      // cleanup
      closeDirectoryHandle(pContext->hDirectory);

      // return error
      callbacks.onRegistrationError(error);

      return NULL;
   }

   // we have passed the pContext into the system so it's ownership will
   // now be governed by the receipt of ERROR_OPERATION_ABORTED within
   // the completion callback
   autoPtrContext.release();

   // increment the number of active requests
   ::InterlockedIncrement(&s_activeRequests);

   // scan the files
   error = scanFiles(FileInfo(filePath), true, &pContext->fileTree);
   if (error)
   {
       // cleanup
       cleanupContext(pContext);

       // return error
       callbacks.onRegistrationError(error);

       return NULL;
   }

   // now that we have finished the file listing we know we have a valid
   // file-monitor so set the callbacks
   pContext->onMonitoringError = callbacks.onMonitoringError;
   pContext->onFilesChanged = callbacks.onFilesChanged;

   // notify the caller that we have successfully registered
   callbacks.onRegistered((Handle)pContext, pContext->fileTree);

   // return the handle
   return (Handle)pContext;
}

// unregister a file monitor
void unregisterMonitor(Handle handle)
{
   // this will end up calling the completion routine with
   // ERROR_OPERATION_ABORTED at which point we'll delete the context
   cleanupContext((FileEventContext*)handle);
}

void run(const boost::function<void()>& checkForInput)
{
   // initialize active requests to zero
   s_activeRequests = 0;

   // loop waiting for:
   //   - completion routine callbacks (occur during SleepEx); or
   //   - inbound commands (occur during checkForInput)
   //
   while (true)
   {
      ::SleepEx(1000, TRUE);
      checkForInput();
   }
}

void stop()
{
   // call ::SleepEx until all active requests hae terminated
   while (s_activeRequests > 0)
   {
      ::SleepEx(100, TRUE);
   }
}

} // namespace detail
} // namespace file_monitor
} // namespace system
} // namespace core 

   


