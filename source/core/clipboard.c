#include <malloc.h>
#include <string.h>

#include <3ds.h>

#include "clipboard.h"
#include "fs.h"
#include "stringutil.h"

static bool clipboard_has = false;
static bool clipboard_contents_only;

static FS_Archive clipboard_archive;
static char clipboard_path[FILE_PATH_MAX];
/**
 * @brief      Returns a boolean indicating if the clipboard has contents
 *
 * @details    Returns a boolean indicating if the clipboard has contents
 *
 *
 * @return     bool
 */
bool clipboard_has_contents() {
    return clipboard_has;
}
/**
 * @brief      Gets the archive in the clipboard
 *
 * @details    Gets an FS_Archive from the clipboard, it doesn't check if the clipboard has contests. This means that if the clipboard has been cleared this archives will be a 0;
 *
 *
 * @return     FS_Archive or 0;
 */
FS_Archive clipboard_get_archive() {
    return clipboard_archive;
}
/**
 * @brief      Returns the path in the clipboard
 *
 * @details    Returns the path in the clipboard
 *
 *
 * @return     char*
 */
char* clipboard_get_path() {
    return clipboard_path;
}

bool clipboard_is_contents_only() {
    return clipboard_contents_only;
}
/**
 * @brief      Fills the clipboard
 *
 * @details    Fills the clipboard with the archive and path provided. FIXME: Check what contentsOnly flag means.
 *
 * @param      FS_Archive , char* path and bool contentsOnly
 *
 * @return     Result of the action.
 */
Result clipboard_set_contents(FS_Archive archive, const char* path, bool contentsOnly) {
    clipboard_clear();

    Result res = 0;
    if(R_SUCCEEDED(res = fs_ref_archive(archive))) {
        clipboard_has = true;
        clipboard_contents_only = contentsOnly;

        clipboard_archive = archive;
        string_copy(clipboard_path, path, FILE_PATH_MAX);
    }

    return res;
}
/**
 * @brief      Clear the clipboard
 *
 * @details    Clears the clipboard
 *
 *
 */
void clipboard_clear() {
    if(clipboard_archive != 0) {
        fs_close_archive(clipboard_archive);
    }

    clipboard_has = false;
    clipboard_contents_only = false;

    clipboard_archive = 0;
    memset(clipboard_path, '\0', FILE_PATH_MAX);
}