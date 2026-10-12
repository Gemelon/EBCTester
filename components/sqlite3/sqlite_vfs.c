#include "sqlite3.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_sys.h"

#define SQLITE_VFS_MAX_PATH 256
#define SQLITE_VFS_SECTOR_SIZE 512

static const char *TAG = "sqlite_vfs";

typedef struct {
    sqlite3_file base;
    int fd;
    int is_main_database;
    char path[SQLITE_VFS_MAX_PATH];
} sqlite_vfs_file_t;

static int vfs_close(sqlite3_file *file)
{
    sqlite_vfs_file_t *sqlite_file = (sqlite_vfs_file_t *)file;
    int result = close(sqlite_file->fd);
    sqlite_file->fd = -1;
    return result == 0 ? SQLITE_OK : SQLITE_IOERR_CLOSE;
}

static int vfs_read(sqlite3_file *file, void *buffer, int amount, sqlite3_int64 offset)
{
    sqlite_vfs_file_t *sqlite_file = (sqlite_vfs_file_t *)file;
    // FATFS extends files opened for writing when seeking past EOF, so never seek there.
    struct stat file_stat;
    if (fstat(sqlite_file->fd, &file_stat) != 0) {
        return SQLITE_IOERR_READ;
    }
    if (offset >= (sqlite3_int64)file_stat.st_size) {
        memset(buffer, 0, (size_t)amount);
        return SQLITE_IOERR_SHORT_READ;
    }
    if (offset + amount > (sqlite3_int64)file_stat.st_size) {
        size_t available = (size_t)((sqlite3_int64)file_stat.st_size - offset);
        memset((char *)buffer + available, 0, (size_t)amount - available);
        int result = vfs_read(file, buffer, (int)available, offset);
        return result == SQLITE_OK ? SQLITE_IOERR_SHORT_READ : result;
    }
    if (lseek(sqlite_file->fd, (off_t)offset, SEEK_SET) != (off_t)offset) {
        if (sqlite_file->is_main_database) {
            ESP_LOGE(TAG, "seek for read failed: offset=%" PRId64 ", errno=%d",
                     offset, errno);
        }
        return SQLITE_IOERR_READ;
    }
    size_t bytes_read = 0;
    while (bytes_read < (size_t)amount) {
        ssize_t result = read(sqlite_file->fd, (char *)buffer + bytes_read,
                              (size_t)amount - bytes_read);
        if (result > 0) {
            bytes_read += (size_t)result;
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else if (result == 0) {
            memset((char *)buffer + bytes_read, 0, (size_t)amount - bytes_read);
            return SQLITE_IOERR_SHORT_READ;
        } else {
            if (sqlite_file->is_main_database) {
                ESP_LOGE(TAG, "read failed: offset=%" PRId64 ", requested=%d, errno=%d",
                         offset, amount, errno);
            }
            return SQLITE_IOERR_READ;
        }
    }
    return SQLITE_OK;
}

static int vfs_write(sqlite3_file *file, const void *buffer, int amount,
                     sqlite3_int64 offset)
{
    sqlite_vfs_file_t *sqlite_file = (sqlite_vfs_file_t *)file;
    // A seek past EOF would extend the FATFS file with undefined content; zero-fill instead.
    struct stat file_stat;
    if (fstat(sqlite_file->fd, &file_stat) != 0) {
        return SQLITE_IOERR_WRITE;
    }
    if (offset > (sqlite3_int64)file_stat.st_size &&
        ftruncate(sqlite_file->fd, (off_t)offset) != 0) {
        ESP_LOGE(TAG, "zero-fill before write failed: offset=%" PRId64 ", errno=%d",
                 offset, errno);
        return SQLITE_IOERR_WRITE;
    }
    if (lseek(sqlite_file->fd, (off_t)offset, SEEK_SET) != (off_t)offset) {
        if (sqlite_file->is_main_database) {
            ESP_LOGE(TAG, "seek for write failed: offset=%" PRId64 ", errno=%d",
                     offset, errno);
        }
        return SQLITE_IOERR_WRITE;
    }
    size_t bytes_written = 0;
    while (bytes_written < (size_t)amount) {
        ssize_t result = write(sqlite_file->fd, (const char *)buffer + bytes_written,
                               (size_t)amount - bytes_written);
        if (result > 0) {
            bytes_written += (size_t)result;
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            if (sqlite_file->is_main_database) {
                ESP_LOGE(TAG, "write failed: offset=%" PRId64 ", requested=%d, errno=%d",
                         offset, amount, errno);
            }
            return SQLITE_IOERR_WRITE;
        }
    }
    return SQLITE_OK;
}

static int vfs_truncate(sqlite3_file *file, sqlite3_int64 size)
{
    sqlite_vfs_file_t *sqlite_file = (sqlite_vfs_file_t *)file;
    int result = ftruncate(sqlite_file->fd, (off_t)size);
    if (result != 0 && sqlite_file->is_main_database) {
        ESP_LOGE(TAG, "main-DB truncate failed: size=%" PRId64 ", errno=%d", size, errno);
    }
    return result == 0 ? SQLITE_OK : SQLITE_IOERR_TRUNCATE;
}

static int vfs_sync(sqlite3_file *file, int flags)
{
    (void)flags;
    sqlite_vfs_file_t *sqlite_file = (sqlite_vfs_file_t *)file;
    int result = fsync(sqlite_file->fd);
    if (result != 0 && sqlite_file->is_main_database) {
        ESP_LOGE(TAG, "main-DB sync failed: errno=%d", errno);
    }
    return result == 0 ? SQLITE_OK : SQLITE_IOERR_FSYNC;
}

static int vfs_file_size(sqlite3_file *file, sqlite3_int64 *size)
{
    sqlite_vfs_file_t *sqlite_file = (sqlite_vfs_file_t *)file;
    struct stat file_stat;
    if (fstat(sqlite_file->fd, &file_stat) != 0) {
        if (sqlite_file->is_main_database) {
            ESP_LOGE(TAG, "fstat for main DB failed: errno=%d", errno);
        }
        return SQLITE_IOERR_FSTAT;
    }
    *size = file_stat.st_size;
    return SQLITE_OK;
}

static int vfs_lock(sqlite3_file *file, int lock_level)
{
    (void)file;
    (void)lock_level;
    return SQLITE_OK;
}

static int vfs_unlock(sqlite3_file *file, int lock_level)
{
    (void)file;
    (void)lock_level;
    return SQLITE_OK;
}

static int vfs_check_reserved_lock(sqlite3_file *file, int *result)
{
    (void)file;
    *result = 0;
    return SQLITE_OK;
}

static int vfs_file_control(sqlite3_file *file, int operation, void *argument)
{
    (void)file;
    (void)operation;
    (void)argument;
    return SQLITE_NOTFOUND;
}

static int vfs_sector_size(sqlite3_file *file)
{
    (void)file;
    return SQLITE_VFS_SECTOR_SIZE;
}

static int vfs_device_characteristics(sqlite3_file *file)
{
    (void)file;
    return 0;
}

static const sqlite3_io_methods vfs_io_methods = {
    .iVersion = 1,
    .xClose = vfs_close,
    .xRead = vfs_read,
    .xWrite = vfs_write,
    .xTruncate = vfs_truncate,
    .xSync = vfs_sync,
    .xFileSize = vfs_file_size,
    .xLock = vfs_lock,
    .xUnlock = vfs_unlock,
    .xCheckReservedLock = vfs_check_reserved_lock,
    .xFileControl = vfs_file_control,
    .xSectorSize = vfs_sector_size,
    .xDeviceCharacteristics = vfs_device_characteristics,
};

static int vfs_open(sqlite3_vfs *vfs, const char *path, sqlite3_file *file,
                   int flags, int *out_flags)
{
    (void)vfs;
    if (path == NULL) {
        return SQLITE_CANTOPEN;
    }
    size_t path_length = strlen(path);
    if (path_length >= SQLITE_VFS_MAX_PATH) {
        return SQLITE_CANTOPEN;
    }

    int open_flags = 0;
    if (flags & SQLITE_OPEN_READONLY) {
        open_flags |= O_RDONLY;
    } else if (flags & SQLITE_OPEN_READWRITE) {
        open_flags |= O_RDWR;
    } else {
        return SQLITE_CANTOPEN;
    }
    if (flags & SQLITE_OPEN_CREATE) {
        open_flags |= O_CREAT;
    }
    if (flags & SQLITE_OPEN_EXCLUSIVE) {
        open_flags |= O_EXCL;
    }

    sqlite_vfs_file_t *sqlite_file = (sqlite_vfs_file_t *)file;
    memset(sqlite_file, 0, sizeof(*sqlite_file));
    sqlite_file->fd = open(path, open_flags, 0600);
    if (sqlite_file->fd < 0) {
        ESP_LOGE(TAG, "open failed: flags=0x%x, errno=%d", flags, errno);
        return SQLITE_CANTOPEN;
    }
    sqlite_file->is_main_database = (flags & SQLITE_OPEN_MAIN_DB) != 0;
    memcpy(sqlite_file->path, path, path_length + 1);
    sqlite_file->base.pMethods = &vfs_io_methods;
    if (out_flags != NULL) {
        *out_flags = (open_flags & O_ACCMODE) == O_RDONLY
                         ? SQLITE_OPEN_READONLY
                         : SQLITE_OPEN_READWRITE;
    }
    ESP_LOGD(TAG, "opened %s: path=%s sqlite flags=0x%x fd=%d",
             sqlite_file->is_main_database ? "main DB" : "auxiliary file",
             sqlite_file->path, flags, sqlite_file->fd);
    return SQLITE_OK;
}

static int vfs_delete(sqlite3_vfs *vfs, const char *path, int sync_directory)
{
    (void)vfs;
    (void)sync_directory;
    return unlink(path) == 0 || errno == ENOENT ? SQLITE_OK : SQLITE_IOERR_DELETE;
}

static int vfs_access(sqlite3_vfs *vfs, const char *path, int flags, int *result)
{
    (void)vfs;
    (void)flags;
    struct stat file_stat;
    if (stat(path, &file_stat) == 0) {
        *result = 1;
        return SQLITE_OK;
    }
    if (errno == ENOENT) {
        *result = 0;
        return SQLITE_OK;
    }
    return SQLITE_IOERR_ACCESS;
}

static int vfs_full_pathname(sqlite3_vfs *vfs, const char *path, int output_size,
                             char *output)
{
    (void)vfs;
    size_t path_length = strlen(path);
    if (path_length >= (size_t)output_size) {
        return SQLITE_CANTOPEN;
    }
    memcpy(output, path, path_length + 1);
    return SQLITE_OK;
}

static int vfs_randomness(sqlite3_vfs *vfs, int amount, char *output)
{
    (void)vfs;
    esp_fill_random(output, (size_t)amount);
    return amount;
}

static int vfs_sleep(sqlite3_vfs *vfs, int microseconds)
{
    (void)vfs;
    esp_rom_delay_us((uint32_t)microseconds);
    return microseconds;
}

static int vfs_current_time(sqlite3_vfs *vfs, double *julian_day)
{
    (void)vfs;
    *julian_day = (double)time(NULL) / 86400.0 + 2440587.5;
    return SQLITE_OK;
}

static sqlite3_vfs sqlite_vfs = {
    .iVersion = 1,
    .szOsFile = sizeof(sqlite_vfs_file_t),
    .mxPathname = SQLITE_VFS_MAX_PATH,
    .zName = "esp-fatfs",
    .xOpen = vfs_open,
    .xDelete = vfs_delete,
    .xAccess = vfs_access,
    .xFullPathname = vfs_full_pathname,
    .xRandomness = vfs_randomness,
    .xSleep = vfs_sleep,
    .xCurrentTime = vfs_current_time,
};

int sqlite3_os_init(void)
{
    return sqlite3_vfs_register(&sqlite_vfs, 1);
}

int sqlite3_os_end(void)
{
    return SQLITE_OK;
}
