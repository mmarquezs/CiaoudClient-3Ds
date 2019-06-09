#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "error.h"
#include "fs.h"
#include "linkedlist.h"
#include "stringutil.h"
/**
 * @brief      Returns if the path points to a directory
 *
 * @detail     Returns if the path points to a directory
 *
 * @param      archive Archive file
 * @param      path Pat, in utf8, to check if is a directory
 *
 * @return     boolean indicating if it's a directory
 */
bool fs_is_dir(FS_Archive archive, const char* path) {
    Result res = 0;

    #Obtains FS_Paths from a utf8 char array.
    FS_Path* fsPath = fs_make_path_utf8(path);

    if(fsPath != NULL) {
      #Is a valid FS_Path so we try to open the path as a directory in the provided archive.
        Handle dirHandle = 0;
        if(R_SUCCEEDED(res = FSUSER_OpenDirectory(&dirHandle, archive, *fsPath))) {
            FSDIR_Close(dirHandle);
        }

        fs_free_path_utf8(fsPath);
    } else {
#If is null it failed to allocated memory so return out of memory error
        res = R_APP_OUT_OF_MEMORY;
    }

    return R_SUCCEEDED(res);
}

/**
 * @brief      Ensures the provided path is or will be a directory
 *
 * @details    Checks that the path is a directory if it isn't it deletes the file and creates a directory
 *
 * @param      archive Archive file
 *
 * @param      path path to the directory
 *
 * @return     Result result of function, if it was succesfull obtaining a directory
 */
Result fs_ensure_dir(FS_Archive archive, const char* path) {
    Result res = 0;

    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath != NULL) {
        Handle dirHandle = 0;
        if(R_SUCCEEDED(FSUSER_OpenDirectory(&dirHandle, archive, *fsPath))) {
            FSDIR_Close(dirHandle);
        } else {
            FSUSER_DeleteFile(archive, *fsPath);
            res = FSUSER_CreateDirectory(archive, *fsPath, 0);
        }

        fs_free_path_utf8(fsPath);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}

/**
 * @brief      Given data it creates a FS_Path as a PATH_BINARY
 *
 * @details    Given data it creates a FS_Path as a PATH_BINARY
 *
 * @param      data, size - Where data is the binary path, and size the size of it.
 *
 * @return     FS_Path(PATH_BINARY)
 */
FS_Path fs_make_path_binary(const void* data, u32 size) {
    FS_Path path = {PATH_BINARY, size, data};
    return path;
}


/**
 * @brief      Returns FS_Path in UTF16 from an UTF8 char path
 *
 * @details    Given an utf8 char array path
 *
 * @param      char* Utf8 path
 *
 * @return     FS_Path(PATH_UTF16)
 */
FS_Path* fs_make_path_utf8(const char* path) {
    size_t len = strlen(path);

    u16* utf16 = (u16*) calloc(len + 1, sizeof(u16));
    if(utf16 == NULL) {
        return NULL;
    }

    ssize_t utf16Len = utf8_to_utf16(utf16, (const uint8_t*) path, len);

    FS_Path* fsPath = (FS_Path*) calloc(1, sizeof(FS_Path));
    if(fsPath == NULL) {
        free(utf16);
        return NULL;
    }

    fsPath->type = PATH_UTF16;
    fsPath->size = (utf16Len + 1) * sizeof(u16);
    fsPath->data = utf16;

    return fsPath;
}


/**
 * @brief      Deallocates a path
 *
 * @details    Deallocates the path struct and the data element it's pointing to.
 *
 * @param      FS_Path
 *
 * @return     void
 */
void fs_free_path_utf8(FS_Path* path) {
    free((void*) path->data);
    free(path);
}

typedef struct {
    FS_Archive archive;
    u32 refs;
} archive_ref;

static linked_list opened_archives;

/**
 * @brief      Opens a FS_Archive.
 *
 * @details    Opens a provided FS_Archive using it's ID (ARCHIVE_ROMFS,ARCHIVE_USER_SAVEDATA,...  )  and path.
 *
 * @param      archive to open
 * @param      id of archive type
 * @param      path of the archive
 *
 * @return     Result
 */
Result fs_open_archive(FS_Archive* archive, FS_ArchiveID id, FS_Path path) {
    if(archive == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    Result res = 0;

    FS_Archive arch = 0;
    if(R_SUCCEEDED(res = FSUSER_OpenArchive(&arch, id, path))) {
        if(R_SUCCEEDED(res = fs_ref_archive(arch))) {
            *archive = arch;
        } else {
            FSUSER_CloseArchive(arch);
        }
    }

    return res;
}

/**
 * @brief      Adds the reference of a FS_Archive to the opened_archives list
 *
 * @details    It adds the references to a FS_Archive to the opened_archives list, if the archive is already there it adds +1 to the refs to that archive. This is used to keep track of the opened archives and close them when they arent referenced anymore.
 *
 * @param      FS_Archive to add
 *
 * @return     Result
 */
Result fs_ref_archive(FS_Archive archive) {
# Gets an iterator from opened_archives list
    linked_list_iter iter;
    linked_list_iterate(&opened_archives, &iter);

# Loops through the list, searching for the reference to the archive
    while(linked_list_iter_has_next(&iter)) {
        archive_ref* ref = (archive_ref*) linked_list_iter_next(&iter);
        if(ref->archive == archive) {
          # Found a reference to the archive already in the list. Adds +1 to the references to that archive and exits.
            ref->refs++;
            return 0;
        }
    }

    Result res = 0;

# If there wasn't any reference to that file creates a new one and adds it to the opened_archives list.
    archive_ref* ref = (archive_ref*) calloc(1, sizeof(archive_ref));
    if(ref != NULL) {
        ref->archive = archive;
        ref->refs = 1;

        linked_list_add(&opened_archives, ref);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}


/**
 * @brief      Closes an FS_Archive
 *
 * @details    Given an FS_Archive it remove a references from the opened_archives and if it's the last reference to it, it closes the archive.
 *
 * @param      FS_Archive Archive to close.
 *
 * @return     Result of the action
 */
Result fs_close_archive(FS_Archive archive) {
# Gets an iterator from opened_archives list
    linked_list_iter iter;
    linked_list_iterate(&opened_archives, &iter);

# Loops through the list, searching for the reference to the archive

    while(linked_list_iter_has_next(&iter)) {
        archive_ref* ref = (archive_ref*) linked_list_iter_next(&iter);
        if(ref->archive == archive) {
          # Found a reference to the archive already in the list. Adds -1 to the references to that archive.
            ref->refs--;

            if(ref->refs == 0) {
              # If it's the last reference it removes the archive from the opened_archives list.
                linked_list_iter_remove(&iter);
                free(ref);
            } else {
                return 0;
            }
        }
    }

    return FSUSER_CloseArchive(archive);
}

static char path_3dsx[FILE_PATH_MAX] = "";

const char* fs_get_3dsx_path() {
    if(strlen(path_3dsx) == 0) {
        return NULL;
    }

    return path_3dsx;
}

void fs_set_3dsx_path(const char* path) {
    if(strlen(path) >= 5 && strncmp(path, "sdmc:", 5) == 0) {
        string_copy(path_3dsx, path + 5, FILE_PATH_MAX);
    } else {
        string_copy(path_3dsx, path, FILE_PATH_MAX);
    }
}

int fs_make_3dsx_path(char* out, const char* name, size_t size) {
    char filename[FILE_NAME_MAX];
    string_escape_file_name(filename, name, sizeof(filename));

    return snprintf(out, size, "/3ds/%s/%s.3dsx", filename, filename);
}

int fs_make_smdh_path(char* out, const char* name, size_t size) {
    char filename[FILE_NAME_MAX];
    string_escape_file_name(filename, name, sizeof(filename));

    return snprintf(out, size, "/3ds/%s/%s.smdh", filename, filename);
}

/**
 * @brief      Returns the title media destination (NAND Memory or SD)
 *
 * @details    Returns the media destination, NAND Memory or SD, based on the type of application that the title id indicates it is.
 *
 * @param      u64 Nintendo 3DS titleI
 *
 * @return     FS_Mediatype [MEDIATYPE_NAND or MEDIATYPE_SD]
 */
FS_MediaType fs_get_title_destination(u64 titleId) {
    u16 platform = (u16) ((titleId >> 48) & 0xFFFF);
    u16 category = (u16) ((titleId >> 32) & 0xFFFF);
    u8 variation = (u8) (titleId & 0xFF);

    //     DSiWare                3DS                    DSiWare, System, DLP         Application           System Title
    return platform == 0x0003 || (platform == 0x0004 && ((category & 0x8011) != 0 || (category == 0x0000 && variation == 0x02))) ? MEDIATYPE_NAND : MEDIATYPE_SD;
}

bool fs_filter_cias(void* data, const char* name, u32 attributes) {
    if(data != NULL) {
        fs_filter_data* filterData = (fs_filter_data*) data;
        if(filterData->parentFilter != NULL && !filterData->parentFilter(filterData->parentFilterData, name, attributes)) {
            return false;
        }
    }

    if((attributes & FS_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }

    size_t len = strlen(name);
    return len >= 4 && strncasecmp(name + len - 4, ".cia", 4) == 0;
}

bool fs_filter_tickets(void* data, const char* name, u32 attributes) {
    if(data != NULL) {
        fs_filter_data* filterData = (fs_filter_data*) data;
        if(filterData->parentFilter != NULL && !filterData->parentFilter(filterData->parentFilterData, name, attributes)) {
            return false;
        }
    }

    if((attributes & FS_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }

    size_t len = strlen(name);
    return (len >= 4 && strncasecmp(name + len - 4, ".tik", 4) == 0) || (len >= 5 && strncasecmp(name + len - 5, ".cetk", 5) == 0);
}
