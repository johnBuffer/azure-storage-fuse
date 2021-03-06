#include "blobfuse.h"
#include <sys/file.h>
#include <cstring>

#include <iostream>
#include <fstream>

file_lock_map* file_lock_map::get_instance()
{
    if(nullptr == s_instance.get())
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if(nullptr == s_instance.get())
        {
            s_instance.reset(new file_lock_map());
        }
    }
    return s_instance.get();
}

std::shared_ptr<std::mutex> file_lock_map::get_mutex(const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto iter = m_lock_map.find(path);
    if(iter == m_lock_map.end())
    {
        auto file_mutex = std::make_shared<std::mutex>();
        m_lock_map[path] = file_mutex;
        return file_mutex;
    }
    else
    {
        return iter->second;
    }
}

std::shared_ptr<file_lock_map> file_lock_map::s_instance;
std::mutex file_lock_map::s_mutex;

std::deque<file_to_delete> cleanup;
std::mutex deque_lock;

// Opens a file for reading or writing
// Behavior is defined by a normal, open() system call.
// In all methods in this file, the variables "path" and "pathString" refer to the input path - the path as seen by the application using FUSE as a file system.
// The variables "mntPath" and "mntPathString" refer to on-disk cached location of the corresponding file/blob.
int azs_open(const char *path, struct fuse_file_info *fi)
{
    try
    {
        std::ofstream myfile;
        myfile.open ("/home/jean/debug.lol", std::fstream::out);
        myfile << "Start open.\n";
        myfile.close();

        int res;

        azure::storage::cloud_blob_client blob_client = streaming_client_wrapper->create_cloud_blob_client();

        myfile.open("/home/jean/debug.lol", std::fstream::out | std::fstream::app);
        myfile << "Created blob_client.\n";
        myfile.close();

        const std::string pathString(path);
        azure::storage::cloud_blob_container container(blob_client.get_container_reference(str_options.containerName));

        myfile.open ("/home/jean/debug.lol", std::fstream::out | std::fstream::app);
        myfile << "Git container.\n";
        myfile.close();

        struct fhwrapper *fhwrap = new fhwrapper(0, false);
        fhwrap->blob = container.get_blob_reference(pathString.substr(1));

        myfile.open ("/home/jean/debug.lol", std::fstream::out | std::fstream::app);
        myfile << "Got blob.\n";
        myfile.close();
        /*fhwrap->blob.download_attributes();

        myfile.open ("/home/jean/debug.lol");
        myfile << "Got attrib.\n";
        myfile.close();*/
        fi->fh = (long unsigned int)fhwrap; // Store the file handle for later use.

        myfile.open ("/home/jean/debug.lol", std::fstream::out | std::fstream::app);
        myfile << "Open ok.\n";
        myfile.close();

        return 0;
    }
    catch (const azure::storage::storage_exception& e)
    {
        std::ofstream myfile;
        myfile.open ("/home/jean/debug.lol", std::fstream::out | std::fstream::app);
        myfile << "Open failed.\n";
        myfile.close();
        // Cannot find file
        return -EACCES;
    }
}

// We don't use the 'path' parameter
#pragma GCC diagnostic ignored "-Wunused-parameter"
/**
 * Read data from the file (the blob) into the input buffer
 * @param  path   Path of the file (blob) to read from
 * @param  buf    Buffer in which to copy the data
 * @param  size   Amount of data to copy
 * @param  offset Offset in the file (the blob) from which to begin reading.
 * @param  fi     File info for this file.
 * @return        TODO: Error codes
 */
int azs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    std::ofstream myfile;
    myfile.open ("/home/jean/debug.lol", std::fstream::out | std::fstream::app);
    myfile << "Read " << size << " " << offset << std::endl;
    myfile.close();

    fhwrapper* fw = (fhwrapper*)fi->fh;

    uint32_t file_size = fw->blob.properties().size();

    if (offset >= file_size)
        return 0;

    int32_t start(offset - fw->cache_start);
    if (start < 0 || fw->cache_size < start + size)
    {
        uint32_t rest = file_size - offset;

        myfile.open ("/home/jean/debug.lol", std::fstream::out | std::fstream::app);
        myfile << "Rest to read " << rest << std::endl;
        myfile.close();

        uint32_t to_dl = rest;
        if (to_dl > 1000000)
            to_dl = 1000000;

        fw->cache_start = offset;
        fw->cache_size  = to_dl;

        concurrency::streams::container_buffer<std::vector<uint8_t>> buffer;
        concurrency::streams::ostream out_stream(buffer);
        fw->blob.download_range_to_stream(out_stream, offset, to_dl);

        fw->blob.download_to_file("/home/jean/okokokok.txt");

        auto data = buffer.collection();
        memcpy(fw->cache.data(), data.data(), data.size());
    }
    
    memcpy(buf, fw->cache.data(), fw->cache_size);

    return fw->cache_size;
}
#pragma GCC diagnostic pop

// Note that in FUSE, create is not the same as open with specific flags (the way it is in Linux)
// See the FUSE docs on these methods for more details.
int azs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    AZS_DEBUGLOGV("azs_create called with path = %s, mode = %d, fi->flags = %x\n", path, mode, fi->flags);

    auto fmutex = file_lock_map::get_instance()->get_mutex(path);
    std::lock_guard<std::mutex> lock(*fmutex);

    std::string pathString(path);
    const char * mntPath;
    std::string mntPathString = prepend_mnt_path_string(pathString);
    mntPath = mntPathString.c_str();
    int res;
    ensure_files_directory_exists_in_cache(mntPathString);

    // FUSE will set the O_CREAT and O_WRONLY flags, but not O_EXCL, which is generally assumed for 'create' semantics.
    res = open(mntPath, fi->flags | O_EXCL, default_permission);
    if (res == -1)
    {
        syslog(LOG_ERR, "Failure to open cache file %s in azs_open.  errno = %d\n.", path, errno);
        return -errno;
    }

    // At this point, the file exists in the cache and we have an open file handle to it.  We now attempt to acquire the flock lock in shared mode, to be held while reading and writing to the file.
    int lock_result = shared_lock_file(fi->flags, res);
    if(lock_result != 0)
    {
        return lock_result;
    }

    struct fhwrapper *fhwrap = new fhwrapper(res, true);
    fi->fh = (long unsigned int)fhwrap;
    syslog(LOG_INFO, "Successfully created file %s in file cache.\n", path);
    AZS_DEBUGLOGV("Returning success from azs_create with file %s.\n", path);
    return 0;
}

#pragma GCC diagnostic ignored "-Wunused-parameter"
/**
 * Write data to the file.
 *
 * Here, we are still just writing data to the local buffer, not forwarding to Storage.
 * TODO: possible in-memory caching?
 * TODO: for very large files, start uploading to Storage before all the data has been written here.
 * @param  path   Path to the file to write.
 * @param  buf    Buffer containing the data to write.
 * @param  size   Amount of data to write
 * @param  offset Offset in the file to write the data to
 * @param  fi     Fuse file info, containing the fh pointer
 * @return        TODO: Error codes.
 */
int azs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    int fd = ((struct fhwrapper *)fi->fh)->fh;

    errno = 0;
    int res = pwrite(fd, buf, size, offset);
    if (res == -1)
        res = -errno;

    return res;
}

#pragma GCC diagnostic pop

int azs_flush(const char *path, struct fuse_file_info *fi)
{
    AZS_DEBUGLOGV("azs_flush called with path = %s, fi->flags = %d, (((struct fhwrapper *)fi->fh)->fh) = %d.\n", path, fi->flags, (((struct fhwrapper *)fi->fh)->fh));

    // At this point, the shared flock will be held.

    // In some cases, due (I believe) to us using the hard_unlink option, path will be null.  Thus, we need to get the file name from the file descriptor:

    char path_link_buffer[50];
    snprintf(path_link_buffer, 50, "/proc/self/fd/%d", (((struct fhwrapper *)fi->fh)->fh));

    // canonicalize_file_name will follow symlinks to give the actual path name.
    char *path_buffer = canonicalize_file_name(path_link_buffer);
    if (path_buffer == NULL)
    {
        AZS_DEBUGLOGV("Skipped blob upload in azs_flush with input path %s because file no longer exists.\n", path);
        return 0;
    }
    else
    {
        AZS_DEBUGLOGV("Successfully looked up mntPath.  Input path = %s, path_link_buffer = %s, path_buffer = %s\n", path, path_link_buffer, path_buffer);
    }

    // Note that we don't have to prepend the tmpPath, because we already have it, because we're not using the input path but instead are querying for it.
    std::string mntPathString(path_buffer);
    const char * mntPath = path_buffer;
    if (access(mntPath, F_OK) != -1 )
    {
        // We cannot close the actual file handle to the temp file, because of the possibility of flush being called multiple times for a given call to open().
        // For some file systems, however, close() flushes data, so we do want to do that before uploading data to a blob.
        // The solution (taken from the FUSE documentation) is to close a duplicate of the file descriptor.
        close(dup(((struct fhwrapper *)fi->fh)->fh));
        if (((struct fhwrapper *)fi->fh)->upload)
        {
            // Here, we acquire the mutex on the file path.  This is necessary to guard against several race conditions.
            // For example, say that a cache refresh is triggered.  There is a small window of time where the file has been removed and not yet re-downloaded.
            // If the blob upload occurred during that window, this could result in the blob being over-written with a zero-length blob, causing data loss.
            // An flock exclusive lock is not good enough here, because it does not hold across unlink and re-creates, and because the flosk is not acquired in open() before remove() is called during cache refresh.
            // We are not concerned with the possibility of writes from another process occurring during blob upload, because when that other process flushes the file, it will re-upload the blob, correcting any potential errors.
            auto fmutex = file_lock_map::get_instance()->get_mutex(mntPathString.substr(str_options.tmpPath.size() + 5));
            std::lock_guard<std::mutex> lock(*fmutex);

            // Check to ensure that the file still exists; that unlink() hasn't been called previously.
            struct stat buf;
            int statret = stat(mntPath, &buf);
            if (statret != 0)
            {
                int storage_errno = errno;
                if (storage_errno == ENOENT)
                {
                    // If the file in the cache no longer exists, that means unlink() was called on some other thread/process, since we opened the file.
                    // In this case, we do not want to upload a zero-length blob to the service or error out, we want to silently discard any data that has been written and
                    // and with no blob on the service or in the cache.
                    // This mimics the behavior of a real file system.

                    free(path_buffer);
                    AZS_DEBUGLOGV("Skipped blob upload in azs_flush with input path %s because file no longer exists due to a race condition with azs_unlink.\n", path);
                    return 0;
                }
                else
                {
                    syslog(LOG_ERR, "Failing blob upload in azs_flush with input path %s because of an error from stat().  Errno = %d.\n", path, storage_errno);
                    free(path_buffer);
                    return -storage_errno;
                }
            }

            // TODO: This will currently upload the full file on every flush() call.  We may want to keep track of whether
            // or not flush() has been called already, and not re-upload the file each time.
            std::vector<std::pair<std::string, std::string>> metadata;
            std::string blob_name = mntPathString.substr(str_options.tmpPath.size() + 6 /* there are six characters in "/root/" */);
            errno = 0;
            azure_blob_client_wrapper->upload_file_to_blob(mntPath, str_options.containerName, blob_name, metadata, 8);
            if (errno != 0)
            {
                int storage_errno = errno;
                syslog(LOG_ERR, "Failing blob upload in azs_flush with input path %s because of an error from upload_file_to_blob().  Errno = %d.\n", path, storage_errno);
                free(path_buffer);
                return 0 - map_errno(storage_errno);
            }
            else
            {
                syslog(LOG_INFO, "Successfully uploaded file %s to blob %s.\n", path, blob_name.c_str());
            }
        }
    }
    else
    {
        AZS_DEBUGLOGV("Skipped blob upload in azs_flush with input path %s because file no longer exists.\n", path);
    }

    free(path_buffer);
    return 0;
}

// Note that there is not much point in doing error-checking in this method, as release() does not offer a way to communicate any errors with the caller (it's called async with the thread that called close())
int azs_release(const char *path, struct fuse_file_info * fi)
{
    AZS_DEBUGLOGV("azs_release called with path = %s, fi->flags = %d\n", path, fi->flags);

    // Unlock the file
    // Note that this will release the shared lock acquired in the corresponding open() call (the one that gave us this file descriptor, in the fuse_file_info).
    // It will not release any locks acquired from other calls to open(), in this process or in others.
    // If the file handle is invalid, this will fail with EBADF, which is not an issue here.
    flock(((struct fhwrapper *)fi->fh)->fh, LOCK_UN);

    // Close the file handle.
    // This must be done, even if the file no longer exists, otherwise we're leaking file handles.
    close(((struct fhwrapper *)fi->fh)->fh);

    // TODO: Make this method resiliant to renames of the file (same way flush() is)
    std::string pathString(path);
    const char * mntPath;
    std::string mntPathString = prepend_mnt_path_string(pathString);
    mntPath = mntPathString.c_str();
    if (access(mntPath, F_OK) != -1 )
    {
        AZS_DEBUGLOGV("Adding file to the GC from azs_release.  File = %s\n.", mntPath);

        // store the file in the cleanup list
        gc_cache.add_file(pathString);

    }
    else
    {
        syslog(LOG_INFO, "Accessing file %s from azs_release failed.\n", mntPath);
    }
    delete (struct fhwrapper *)fi->fh;
    return 0;
}

int azs_unlink(const char *path)
{
    AZS_DEBUGLOGV("azs_unlink called with path = %s.\n", path);
    std::string pathString(path);
    const char * mntPath;
    std::string mntPathString = prepend_mnt_path_string(pathString);
    mntPath = mntPathString.c_str();

    AZS_DEBUGLOGV("Attempting to delete file %s from local cache.\n", mntPath);

    // We must hold the mutex here, otherwise there is a potential race condition in the following scenario:
    // 1. Process A opens a file for writing and writes to it.
    // 2. Process B calls "unlink"
    // 3. Process A flushes and closes the file.
    // In this case, the file (blob) should not exist.  When process A closes the file, it's closing a file handle that's been unlinked from the directory tree, so any data in the file should be discarded when all handles/links to the file are closed.
    // Most of the time, this will work here as well, because when we upload the blob in flush(), the Azure Storage C++ Lite library acquires a new handle to the file to upload it.  If the file has been unlinked during this time, no data will be uploaded.
    // However, there is a potential race condition.  If unlink() is called in between the Azure Storage C++ Lite library opening the file (for upload), and actually uploading the data, data may be successfully uploaded.
    // Acquiring the mutex here guards against that condition.
    auto fmutex = file_lock_map::get_instance()->get_mutex(path);
    std::lock_guard<std::mutex> lock(*fmutex);
    int remove_success = remove(mntPath);
    // We don't fail if the remove() failed, because that's just removing the file in the local file cache, which may or may not be there.

    if (remove_success)
    {
        AZS_DEBUGLOGV("Successfully removed file %s from local cache in azs_unlink.\n", mntPath);
    }
    else
    {
        AZS_DEBUGLOGV("Failed to remove file %s from local cache in azs_unlink.  errno = %d\n.", mntPath, errno);
    }

    int retval = 0;
    errno = 0;
    azure_blob_client_wrapper->delete_blob(str_options.containerName, pathString.substr(1));
    if (errno != 0)
    {
        int storage_errno = errno;

        // If we successfully removed the file locally and the blob does not exist, we should still return success - this accounts for the case where the file hasn't yet been uploaded.
        if (!((remove_success == 0) && (storage_errno = 404)))
        {
            syslog(LOG_ERR, "Failure to delete blob %s (errno = %d) and local cached file does not exist; returning failure from azs_unlink", pathString.c_str()+1, storage_errno);
            retval = 0 - map_errno(storage_errno);
        }
        else
        {
            syslog(LOG_INFO, "Blob representing path %s did not exist, but file in local cache was removed successfully.", path);
        }
    }
    else
    {
        syslog(LOG_INFO, "Successfully deleted blob %s.", pathString.c_str()+1);
    }

    // Try removing the directory from the local file cache
    // This will fail in the case when the directory is not empty, which is intended.
    // This is needed, because if there are no more files in the directory, and the directory doesn't have a ".directory" blob on the service,
    // We should remove the local directory, to reflect the state of the service.
    size_t last_slash_idx = mntPathString.rfind('/');
    if (std::string::npos != last_slash_idx)
    {
        AZS_DEBUGLOGV("Attempting to remove directory %s from local file cache, in case all files have been deleted.", mntPathString.substr(0, last_slash_idx).c_str());
        remove(mntPathString.substr(0, last_slash_idx).c_str());
    }
    return retval;
}

int azs_truncate(const char * path, off_t off)
{
    AZS_DEBUGLOGV("azs_truncate called.  Path = %s, offset = %s\n", path, to_str(off).c_str());

    std::string pathString(path);
    const char * mntPath;
    std::string mntPathString = prepend_mnt_path_string(pathString);
    mntPath = mntPathString.c_str();

    if (off != 0) // Truncating to zero gets optimized
    {
        // TODO: Refactor azs_open, azs_flush, and azs_release so as to not require us calling them directly here
        struct fuse_file_info fi;
        fi.flags = O_RDWR;
        int res = azs_open(path, &fi);
        if(res != 0)
        {
            syslog(LOG_ERR, "Failing azs_truncate operation on file %s due to failure %d from azs_open.\n.", path, res);
            return res;
        }

        errno = 0;
        int truncret = truncate(mntPath, off);
        if (truncret == 0)
        {
            AZS_DEBUGLOGV("Successfully truncated file %s in the local file cache.", mntPath);
            int flushret = azs_flush(path, &fi);
            if(flushret != 0)
            {
                syslog(LOG_ERR, "Failing azs_truncate operation on file %s due to failure %d from azs_flush.  Note that truncate on cached file succeeded.\n.", path, flushret);
                azs_release(path, &fi);
                return flushret;
            }
        }
        else
        {
            int truncate_errno = errno;
            syslog(LOG_ERR, "Failing azs_truncate operation on file %s due to failure to truncate local file in cache.  Errno = %d.\n.", path, truncate_errno);
            azs_release(path, &fi);
            return -truncate_errno;
        }

        azs_release(path, &fi);
        return 0;
    }

    auto fmutex = file_lock_map::get_instance()->get_mutex(path);
    std::lock_guard<std::mutex> lock(*fmutex);

    struct stat buf;
    int statret = stat(mntPath, &buf);
    if (statret == 0)
    {
        // The file exists in the local cache.  So, we call truncate() on the file in the cache, then upload a zero-length blob to the service, overriding any data.
        int truncret = truncate(mntPath, 0);
        if (truncret == 0)
        {
            AZS_DEBUGLOGV("Successfully truncated file %s in the local file cache.", mntPath);

            // We want to upload a zero-length blob.
            std::istringstream emptyDataStream("");

            std::vector<std::pair<std::string, std::string>> metadata;
            errno = 0;
            azure_blob_client_wrapper->upload_block_blob_from_stream(str_options.containerName, pathString.substr(1), emptyDataStream, metadata);
            if (errno != 0)
            {
                syslog(LOG_ERR, "Failed to upload zero-length blob to %s from azs_truncate.  errno = %d\n.", pathString.c_str()+1, errno);
                return 0 - map_errno(errno); // TODO: Investigate what might happen in this case - the blob has been truncated locally, but not on the service.
            }
            else
            {
                syslog(LOG_INFO, "Successfully uploaded zero-length blob to path %s from azs_truncate.", pathString.c_str()+1);
                return 0;
            }

        }
        else
        {
            syslog(LOG_ERR, "Failed to truncate file %s in local file cache.  errno = %d\n.", pathString.c_str()+1, errno);
            return -errno;
        }
    }
    else
    {
        AZS_DEBUGLOGV("File to truncate %s does not exist in the local cache.\n", path);

        // The blob/file does not exist locally.  We need to see if it exists on the service (if it doesn't we return ENOENT.)
        if (azure_blob_client_wrapper->blob_exists(str_options.containerName, pathString.substr(1))) // TODO: Once we have support for access conditions, we could remove this call, and replace with a put_block_list with if-match-*
        {
            AZS_DEBUGLOGV("Blob %s representing file %s exists on the service.\n", pathString.c_str()+1, path);

            int fd = open(mntPath, O_CREAT|O_WRONLY|O_TRUNC, S_IRWXU | S_IRWXG);  //TODO: Consider removing this, I don't think the optimization will really be worth it.
            if (fd != 0)
            {
                return -errno;
            }
            close(fd);

            // We want to upload a zero-length blob.
            std::istringstream emptyDataStream("");

            std::vector<std::pair<std::string, std::string>> metadata;
            errno = 0;
            azure_blob_client_wrapper->upload_block_blob_from_stream(str_options.containerName, pathString.substr(1), emptyDataStream, metadata);
            if (errno != 0)
            {
                int storage_errno = errno;
                syslog(LOG_ERR, "Failed to upload zero-length blob to %s from azs_truncate.  errno = %d\n.", pathString.c_str()+1, storage_errno);
                return 0 - map_errno(storage_errno); // TODO: Investigate what might happen in this case - the blob has been truncated locally, but not on the service.
            }
            else
            {
                syslog(LOG_INFO, "Successfully uploaded zero-length blob to path %s from azs_truncate.", pathString.c_str()+1);
                return 0;
            }
        }
        else
        {
            syslog(LOG_ERR, "File %s does not exist; failing azs_truncate.\n", path);
            return -ENOENT;
        }
    }
    return 0;
}

int azs_rename_single_file(const char *src, const char *dst)
{
    AZS_DEBUGLOGV("Renaming a single file.  src = %s, dst = %s.\n", src, dst);

    // TODO: if src == dst, return?
    // TODO: lock in alphabetical order?
    auto fsrcmutex = file_lock_map::get_instance()->get_mutex(src);
    std::lock_guard<std::mutex> locksrc(*fsrcmutex);

    auto fdstmutex = file_lock_map::get_instance()->get_mutex(dst);
    std::lock_guard<std::mutex> lockdst(*fdstmutex);

    std::string srcPathString(src);
    const char * srcMntPath;
    std::string srcMntPathString = prepend_mnt_path_string(srcPathString);
    srcMntPath = srcMntPathString.c_str();

    std::string dstPathString(dst);
    const char * dstMntPath;
    std::string dstMntPathString = prepend_mnt_path_string(dstPathString);
    dstMntPath = dstMntPathString.c_str();

    struct stat buf;
    int statret = stat(srcMntPath, &buf);
    if (statret == 0)
    {
        AZS_DEBUGLOGV("Source file %s in rename operation exists in the local cache.\n", src);

        // The file exists in the local cache.  Call rename() on it (note this will preserve existing handles.)
        ensure_files_directory_exists_in_cache(dstMntPath);
        errno = 0;
        int renameret = rename(srcMntPath, dstMntPath);
        if (renameret < 0)
        {
            syslog(LOG_ERR, "Failure to rename source file %s in the local cache.  Errno = %d.\n", src, errno);
            return -errno;
        }
        else
        {
            AZS_DEBUGLOGV("Successfully to renamed file %s to %s in the local cache.\n", src, dst);
        }
        errno = 0;
        auto blob_property = azure_blob_client_wrapper->get_blob_property(str_options.containerName, srcPathString.substr(1));
        if ((errno == 0) && blob_property.valid())
        {
            AZS_DEBUGLOGV("Source file %s for rename operation exists as a blob on the service.\n", src);
            // Blob also exists on the service.  Perform a server-side copy.
            errno = 0;
            azure_blob_client_wrapper->start_copy(str_options.containerName, srcPathString.substr(1), str_options.containerName, dstPathString.substr(1));
            if (errno != 0)
            {
                int storage_errno = errno;
                syslog(LOG_ERR, "Attempt to call start_copy from %s to %s failed.  errno = %d\n.", srcPathString.c_str()+1, dstPathString.c_str()+1, storage_errno);
                return 0 - map_errno(errno);
            }
            else
            {
                syslog(LOG_INFO, "Successfully called start_copy from blob %s to blob %s\n", srcPathString.c_str()+1, dstPathString.c_str()+1);
            }

            errno = 0;
            do
            {
                blob_property = azure_blob_client_wrapper->get_blob_property(str_options.containerName, dstPathString.substr(1));
            }
            while(errno == 0 && blob_property.valid() && blob_property.copy_status.compare(0, 7, "pending") == 0);
            if(blob_property.copy_status.compare(0, 7, "success") == 0)
            {
                syslog(LOG_INFO, "Copy operation from %s to %s succeeded.", srcPathString.c_str()+1, dstPathString.c_str()+1);

                // int retval = azs_unlink(srcPathString); // This will remove the blob from the service, and also take care of removing the directory in the local file cache.
                azure_blob_client_wrapper->delete_blob(str_options.containerName, srcPathString.substr(1));
                if(errno != 0)
                {
                    int storage_errno = errno;
                    syslog(LOG_ERR, "Failed to delete source blob %s during rename operation.  errno = %d\n.", srcPathString.c_str()+1, storage_errno);
                    return 0 - map_errno(storage_errno);
                }
                else
                {
                    syslog(LOG_INFO, "Successfully deleted source blob %s during rename operation.\n", srcPathString.c_str()+1);
                }
            }
            else
            {
                syslog(LOG_ERR, "Copy operation from %s to %s failed on the service.  Copy status = %s.\n", srcPathString.c_str()+1, dstPathString.c_str()+1, blob_property.copy_status.c_str());
                return EFAULT;
            }

            // store the file in the cleanup list
            gc_cache.add_file(dstPathString);

            return 0;
        }
        else if (errno != 0)
        {
            int storage_errno = errno;
            syslog(LOG_ERR, "Failed to get blob properties for blob %s during rename operation.  errno = %d\n", srcPathString.c_str()+1, storage_errno);
            return 0 - map_errno(storage_errno);
        }
    }
    else
    {
        AZS_DEBUGLOGV("Source file %s in rename operation does not exist in the local cache.\n", src);

        // File does not exist locally.  Just do the blob copy.
        // TODO: remove duplicated code.
        errno = 0;
        auto blob_property = azure_blob_client_wrapper->get_blob_property(str_options.containerName, srcPathString.substr(1));
        if ((errno == 0) && blob_property.valid())
        {
            AZS_DEBUGLOGV("Source file %s for rename operation exists as a blob on the service.\n", src);

            // Blob also exists on the service.  Perform a server-side copy.
            errno = 0;
            azure_blob_client_wrapper->start_copy(str_options.containerName, srcPathString.substr(1), str_options.containerName, dstPathString.substr(1));
            if (errno != 0)
            {
                int storage_errno = errno;
                syslog(LOG_ERR, "Attempt to call start_copy from %s to %s failed.  errno = %d\n.", srcPathString.c_str()+1, dstPathString.c_str()+1, storage_errno);
                return 0 - map_errno(storage_errno);
            }
            else
            {
                syslog(LOG_INFO, "Successfully called start_copy from blob %s to blob %s\n", srcPathString.c_str()+1, dstPathString.c_str()+1);
            }

            errno = 0;
            do
            {
                blob_property = azure_blob_client_wrapper->get_blob_property(str_options.containerName, dstPathString.substr(1));
            }
            while(errno == 0 && blob_property.valid() && blob_property.copy_status.compare(0, 7, "pending") == 0);
            if(blob_property.copy_status.compare(0, 7, "success") == 0)
            {
                syslog(LOG_INFO, "Copy operation from %s to %s succeeded.", srcPathString.c_str()+1, dstPathString.c_str()+1);

                azure_blob_client_wrapper->delete_blob(str_options.containerName, srcPathString.substr(1));
                if(errno != 0)
                {
                    int storage_errno = errno;
                    syslog(LOG_ERR, "Failed to delete source blob %s during rename operation.  errno = %d\n.", srcPathString.c_str()+1, storage_errno);
                    return 0 - map_errno(storage_errno);
                }
                else
                {
                    syslog(LOG_INFO, "Successfully deleted source blob %s during rename operation.\n", srcPathString.c_str()+1);
                }
            }
            else
            {
                syslog(LOG_ERR, "Copy operation from %s to %s failed on the service.  Copy status = %s.\n", srcPathString.c_str()+1, dstPathString.c_str()+1, blob_property.copy_status.c_str());
                return EFAULT;
            }

            // in the case of directory_rename, there may be local cache
            // store the file in the cleanup list
            gc_cache.add_file(dstPathString);

            return 0;
        }
        else if (errno != 0)
        {
            int storage_errno = errno;
            syslog(LOG_ERR, "Failed to get blob properties for blob %s during rename operation.  errno = %d\n", srcPathString.c_str()+1, storage_errno);
            return 0 - map_errno(storage_errno);
        }
    }

    return 0;
}
