#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <curl/curl.h>
#include <jansson.h>
#include <zlib.h>

#include "fs.h"
#include "error.h"
#include "http.h"
#include "stringutil.h"

#define MAKE_HTTP_USER_AGENT_(major, minor, micro) ("Mozilla/5.0 (Nintendo 3DS; Mobile; rv:10.0) Gecko/20100101 FBI/" #major "." #minor "." #micro)
#define MAKE_HTTP_USER_AGENT(major, minor, micro) MAKE_HTTP_USER_AGENT_(major, minor, micro)
#define HTTP_USER_AGENT MAKE_HTTP_USER_AGENT(VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO)

#define HTTP_MAX_REDIRECTS 50
#define HTTP_TIMEOUT_SEC 15
#define HTTP_TIMEOUT_NS ((u64) HTTP_TIMEOUT_SEC * 1000000000)

/**
 * @brief      Struct for holding httpc Context, buffer and zlib z_stream
 *
 * @details    Struct for holding httpc Context, buffer and zlib z_stream
 *
 */
struct httpc_context_s {
    httpcContext httpc;

    bool compressed;
    z_stream inflate;
    u8 buffer[32 * 1024];
  u32 bufferSize; //Refactor this to buffedFilledSize to reduce missunderstandings.
};

typedef struct httpc_context_s* httpc_context;

/**
 * @brief      Generates the redirect url.
 *
 * @details    Generates the redirect url given the original url, the redirect and the redirect size.
 *
 * @param      char* oldUrl - Original Url
 *
 * @param      const char* redirectTo - Path to redirect to, it can be a relative path.
 *
 * @param      size_t size - Size of the redirecTo char array.
 *
 * @return     return type
 */
static void httpc_resolve_redirect(char* oldUrl, const char* redirectTo, size_t size) {
    if(size > 0) {
      // Checking if it's a subpath by checking if the first char is an slash.
        if(redirectTo[0] == '/') {
            char* baseEnd = oldUrl;

            // Find the third slash to find the end of the URL's base; e.g.
            // https://www.example.com/
            u32 slashCount = 0;
            while(*baseEnd != '\0' && (baseEnd = strchr(baseEnd + 1, '/')) != NULL) {
                slashCount++;
                if(slashCount == 3) {
                    break;
                }
            }

            // If there are less than 3 slashes, assume the base URL ends at the
            // end of the string; e.g. https://www.example.com
            if(slashCount != 3) {
                baseEnd = oldUrl + strlen(oldUrl);
            }

            // Gets the length of the base url.
            size_t baseLen = baseEnd - oldUrl;
            // If there is space left appends the redirectTo at the end of the
            // baseUrl.
            if(baseLen < size) {
                string_copy(baseEnd, redirectTo, size - baseLen);
            }
        } else {
          // It's a full url, we replace the oldUrl with the new redirect.
            string_copy(oldUrl, redirectTo, size);
        }
    }
}
/**
 * @brief      Creates/Opens an http connection.
 *
 * @details    Create a http context, configures it and connects to the provided url.
 *
 * @param      context a pointer where to store the httpc_context
 *
 * @param      url Url to connect to.
 *
 * @param      userAgent Optional. UserAgent to use instead of the default one.
 *
 * @return     Result result of the operation (R_FAILED, R_SUCCEEDED, R_APP_INVALID_ARGUMENT, R_APP_HTTP_ERROR_BASE,...)
 */
static Result httpc_open(httpc_context* context, const char* url, bool userAgent) {
    if(url == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    Result res = 0;
    // Initalizate the httpc_context
    httpc_context ctx = (httpc_context) calloc(1, sizeof(struct httpc_context_s));
    if(ctx != NULL) {
        char currUrl[1024];
        string_copy(currUrl, url, sizeof(currUrl));

        bool resolved = false;
        u32 redirectCount = 0;
        // Starting the connection, limited to HTTP_MAX_REDIRECTS.
        while(R_SUCCEEDED(res) && !resolved && redirectCount < HTTP_MAX_REDIRECTS) {
          // Opening the httpc_context with the provided url..
            if(R_SUCCEEDED(res = httpcOpenContext(&ctx->httpc, HTTPC_METHOD_GET, currUrl, 1))) {
                u32 response = 0;
                // Disabling SSL certificate verification, setting the headers,
                // including the User-Agent,the time to keep alive and
                // initalizating the connection.
                if(R_SUCCEEDED(res = httpcSetSSLOpt(&ctx->httpc, SSLCOPT_DisableVerify))
                   && (!userAgent || R_SUCCEEDED(res = httpcAddRequestHeaderField(&ctx->httpc, "User-Agent", HTTP_USER_AGENT)))
                   && R_SUCCEEDED(res = httpcAddRequestHeaderField(&ctx->httpc, "Accept-Encoding", "gzip, deflate"))
                   && R_SUCCEEDED(res = httpcSetKeepAlive(&ctx->httpc, HTTPC_KEEPALIVE_ENABLED))
                   && R_SUCCEEDED(res = httpcBeginRequest(&ctx->httpc))
                   && R_SUCCEEDED(res = httpcGetResponseStatusCodeTimeout(&ctx->httpc, &response, HTTP_TIMEOUT_NS))) {
                  // There is a redirect to make.
                    if(response == 301 || response == 302 || response == 303) {
                        redirectCount++;

                        char redirectTo[1024];
                        memset(redirectTo, '\0', sizeof(redirectTo));

                        // Getting the redirect location from the header
                        // "Location", closing the context, and setting the new
                        // url as destination with the httpc_resolve_redirect
                        // function. The next loop it will try this url.
                        if(R_SUCCEEDED(res = httpcGetResponseHeader(&ctx->httpc, "Location", redirectTo, sizeof(redirectTo)))) {
                            httpcCloseContext(&ctx->httpc);

                            httpc_resolve_redirect(currUrl, redirectTo, sizeof(currUrl));
                        }
                    } else {
                      // Resolved without any redirection.
                        resolved = true;

                        if(response == 200) {
                          // Status OK. We check to see if it's compressed.If so
                          // we decompressed/inflated.
                            char encoding[32];
                            if(R_SUCCEEDED(httpcGetResponseHeader(&ctx->httpc, "Content-Encoding", encoding, sizeof(encoding)))) {
                                bool gzip = strncmp(encoding, "gzip", sizeof(encoding)) == 0;
                                bool deflate = strncmp(encoding, "deflate", sizeof(encoding)) == 0;

                                ctx->compressed = gzip || deflate;

                                if(ctx->compressed) {
                                    memset(&ctx->inflate, 0, sizeof(ctx->inflate));
                                    if(deflate) {
                                        inflateInit(&ctx->inflate);
                                    } else if(gzip) {
                                        inflateInit2(&ctx->inflate, MAX_WBITS | 16);
                                    }
                                }
                            }
                        } else {
                          // Status NO OK. (403, 404, 500,etc.)
                            res = R_APP_HTTP_ERROR_BASE + response;
                        }
                    }
                }

                if(R_FAILED(res)) {
                    httpcCloseContext(&ctx->httpc);
                }
            }
        }
        // FIXME: It should be redirectCount > HTTP_MAX_REDIRECTS
        if(R_SUCCEEDED(res) && redirectCount >= 32) {
            res = R_APP_HTTP_TOO_MANY_REDIRECTS;
        }

        if(R_FAILED(res)) {
            free(ctx);
        }
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    if(R_SUCCEEDED(res)) {
        *context = ctx;
    }

    return res;
}
/**
 * @brief      Closes httpc_context
 *
 * @details    Closes httpc_context
 *
 * @param      context httpc_context
 *
 * @return     Result result of the action
 */
static Result httpc_close(httpc_context context) {
    if(context == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }
    // Freeing the decompression stream state
    if(context->compressed) {
        inflateEnd(&context->inflate);
    }

    Result res = httpcCloseContext(&context->httpc);
    free(context);
    return res;
}
/**
 * @brief      Returns the download size
 *
 * @details    Returns the download size. (Not the downloaded, the full content size)
 *
 * @param      context - httpc_context
 *
 * @return     Result - result of the operation.
 */
static Result httpc_get_size(httpc_context context, u32* size) {
    if(context == NULL || size == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    return httpcGetDownloadSizeState(&context->httpc, NULL, size);
}

/**
 * @brief      Read the data downloaded
 *
 * @details    Reads n bytes from the connection and saves them to buffer of the specified size.
 *
 * @param      context httpc_context
 * @param      bytesRead The bytes to read
 * @param      buffer The buffer where to store the data
 * @param      size The size of the destination buffer.
 *
 * @return     return type
 */
static Result httpc_read(httpc_context context, u32* bytesRead, void* buffer, u32 size) {
    if(context == NULL || buffer == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    Result res = 0;

    u32 startPos = 0;
    if(R_SUCCEEDED(res = httpcGetDownloadSizeState(&context->httpc, &startPos, NULL))) {
      // We download compressed data.
        res = HTTPC_RESULTCODE_DOWNLOADPENDING;

        u32 outPos = 0;
        if(context->compressed) {
          // FIXME: It seems the intention was to download data after a
          // partially emptied buffer (due to partial inflate), that can be seen
          // in the receivedatatimeout calculating the buffer pointer but there
          // is a check of bufferSize > 0 that prevents this of happening. This is related with the next 
            u32 lastPos = context->bufferSize;
            while(res == HTTPC_RESULTCODE_DOWNLOADPENDING && outPos < size) {
                if((context->bufferSize > 0
                    // If buffersize is 0 then we downlod data from the http
                    // context. FIXME: This command is pointing to the memory
                    // after the data already in the buffer using the
                    // buffersize. This is pointless here cause it will always
                    // be 0. Plus the size is also calculated with the size of
                    // the buffer minus the buffersize, btw bufferSize is actually the
                    // bufferFilledSize, the size of the filled part of the
                    // buffer.
                    || R_SUCCEEDED(res = httpcReceiveDataTimeout(&context->httpc, &context->buffer[context->bufferSize], sizeof(context->buffer) - context->bufferSize, HTTP_TIMEOUT_NS))
                    || res == HTTPC_RESULTCODE_DOWNLOADPENDING)) {
                    Result posRes = 0;
                    u32 currPos = 0;
                    if(R_SUCCEEDED(posRes = httpcGetDownloadSizeState(&context->httpc, &currPos, NULL))) {
                      // We calculate the new buffersize based on the current downloaded size and the last buffersize(lastPos)
                        context->bufferSize += currPos - lastPos;
                        // We setup ZLib
                        // First we setup the context buffer as input
                        context->inflate.next_in = context->buffer;
                        // We setup the provided buffer as the output (plus outPos to append at the end if we added previous data)
                        context->inflate.next_out = buffer + outPos;
                        // We set the bytes available with the buffersize.
                        context->inflate.avail_in = context->bufferSize;
                        // We set the bytes available to output with the size provided (minus outPos to check the remaining space).
                        context->inflate.avail_out = size - outPos;
                        // We decompress/inflate the data using the Sync Flush implementation.
                        inflate(&context->inflate, Z_SYNC_FLUSH);
                        // After the inflate avail_in can be 0 (if it finished) or >0 which means we have to continue inflating.
                        // We copy the leftovers (not inflated) of context->buffer to itself.
                        memcpy(context->buffer, context->buffer + (context->bufferSize - context->inflate.avail_in), context->inflate.avail_in);
                        context->bufferSize = context->inflate.avail_in;

                        lastPos = currPos;
                        outPos = size - context->inflate.avail_out;
                    } else {
                        res = posRes;
                    }
                }
            }
        } else {
          // We download not compressed data.
            while(res == HTTPC_RESULTCODE_DOWNLOADPENDING && outPos < size) {
                if(R_SUCCEEDED(res = httpcReceiveDataTimeout(&context->httpc, &((u8*) buffer)[outPos], size - outPos, HTTP_TIMEOUT_NS)) || res == HTTPC_RESULTCODE_DOWNLOADPENDING) {
                    Result posRes = 0;
                    u32 currPos = 0;
                    if(R_SUCCEEDED(posRes = httpcGetDownloadSizeState(&context->httpc, &currPos, NULL))) {
                        outPos = currPos - startPos;
                    } else {
                        res = posRes;
                    }
                }
            }
        }

        if(res == HTTPC_RESULTCODE_DOWNLOADPENDING) {
            res = 0;
        }

        if(R_SUCCEEDED(res) && bytesRead != NULL) {
            *bytesRead = outPos;
        }
    }

    return res;
}

#define R_HTTP_TLS_VERIFY_FAILED 0xD8A0A03C

/**
 * @brief      Struct to store curl http curl data
 *
 * @details    Struct to store curl http curl data
 *
 */
typedef struct {
    u32 bufferSize;
    void* userData;
    Result (*callback)(void* userData, void* buffer, size_t size);
    Result (*checkRunning)(void* userData);
    Result (*progress)(void* userData, u64 total, u64 curr);

    void* buf;
    u32 pos;

    Result res;
} http_curl_data;

/**
 * @brief      Function that writes the data from curl and executes a callback.
 *
 * @details    This function write the data received by curl to the provided destination and at the end it calls the callback
 *
 * @param      *ptr Pointer to the destination to copy the data
  * @param      size_t Size of each element on destination.
 * @param      nmemb Number of elements, each one with size of size.
 * @param      *userdata Stream of downloaded data.
 *
 * @return     size_t Size of the returned buffer.
 */
static size_t http_curl_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    http_curl_data* curlData = (http_curl_data*) userdata;
    // We set the start position to 0
    size_t srcPos = 0;
    // We calculate the available memory. N elements of X size.
    size_t available = size * nmemb;
    while(R_SUCCEEDED(curlData->res) && available > 0) {
        // We calculate the remaining data to copy from userdata.
        size_t remaining = curlData->bufferSize - curlData->pos;
        // We calculate the data to copy, if it fits on destination we copy all
        // the remaining data otherwise only the data that fits.
        size_t copySize = available < remaining ? available : remaining;
        // We copy the data and move the cursors position.
        memcpy((u8*) curlData->buf + curlData->pos, ptr + srcPos, copySize);
        curlData->pos += copySize;

        srcPos += copySize;
        available -= copySize;

        // We arrived at the end. We execute the callback and we reset the cursor position to 0.
        if(curlData->pos == curlData->bufferSize) {
            curlData->res = curlData->callback(curlData->userData, curlData->buf, curlData->bufferSize);
            curlData->pos = 0;
        }
    }
    // If it succeded we return the size of the filled buffer otherwise return 0.
    return R_SUCCEEDED(curlData->res) ? size * nmemb : 0;
}

/**
 * @brief      Returns download info. Total size and downloded.
 *
 * @details    Returns the download total and the amount downloaded so far. Altought it's required in the parameters it doesn't return the uploaded amount.
 *
 * @param      clientp - Pointer to a curl client / http_curl_data.
 *
 * @param      dltotal - Output of the total amount to download.
 *
 * @param      dlnow - Output of the amount downloaded up to this point.
 *
 * @param      uptotal - Output of the total amount to upload. NOT USED
 *
 * @param      upnow - Output of the amount uploaded up to this point.
 *
 * @return     int - 1 if failed and 0 if succesfully.
 */
int http_curl_xfer_info_callback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    http_curl_data* curlData = (http_curl_data*) clientp;
    // If it failed or the checkRunning callback is set  and its not running.
    if(R_FAILED(curlData->res) || (curlData->checkRunning != NULL && R_FAILED(curlData->res = curlData->checkRunning(curlData->userData)))) {
        return 1;
    }

    // If progress is not null, return the progress (only download info)
    if(curlData->progress != NULL) {
        curlData->progress(curlData->userData, (u64) dltotal, (u64) dlnow);
    }

    return 0;
}

/**
 * @brief      Http Download Callback - Callback for curl download.
 *
 * @details It downloads the provided url up the provided buffersize and updates
 * the calls the callback for saving the data, the checkRunning provided
 * function and updates progress.
 *
 * @param      char* url - The url to download.
 *
 * @param      u32 bufferSize - The size of the buffer destination in the userData.
 *
 * @param      void* userData - A pointer to an userData struct where the data will be saved, this includes the buffer, progress,etc..
 *
 * @param      char* url - The url to download.
 *
 * @param      char* url - The url to download.
 *
 * @param      char* url - The url to download.
 *
 *
 * @return     return type
 */
Result http_download_callback(const char* url, u32 bufferSize, void* userData, Result (*callback)(void* userData, void* buffer, size_t size),
                                                                               Result (*checkRunning)(void* userData),
                                                                               Result (*progress)(void* userData, u64 total, u64 curr)) {
    Result res = 0;

    void* buf = malloc(bufferSize);
    if(buf != NULL) {
        httpc_context context = NULL;
        if(R_SUCCEEDED(res = httpc_open(&context, url, true))) {
            u32 dlSize = 0;
            if(R_SUCCEEDED(res = httpc_get_size(context, &dlSize))) {
                if(progress != NULL) {
                    progress(userData, dlSize, 0);
                }

                u32 total = 0;
                u32 currSize = 0;
                // Main download While running, it keeps reading the data from
                // the httpc context, passing it to the callback and updating
                // the progress.
                while(total < dlSize
                      && (checkRunning == NULL || R_SUCCEEDED(res = checkRunning(userData)))
                      && R_SUCCEEDED(res = httpc_read(context, &currSize, buf, bufferSize))
                      && R_SUCCEEDED(res = callback(userData, buf, currSize))) {
                    if(progress != NULL) {
                        progress(userData, dlSize, total);
                    }

                    total += currSize;
                }

                // Download finished, closing connection.
                Result closeRes = httpc_close(context);
                if(R_SUCCEEDED(res)) {
                    res = closeRes;
                }
            }
            // If it fails to establish an https connection due to failed verification it switches to using curl for downloading.
         else if(res == R_HTTP_TLS_VERIFY_FAILED) {
            res = 0;

            // Setting up CURL with all the required paramters, buffer, callbacks,etc.
            CURL* curl = curl_easy_init();
            if(curl != NULL) {
                http_curl_data curlData = {bufferSize, userData, callback, checkRunning, progress, buf, 0, 0};

                curl_easy_setopt(curl, CURLOPT_URL, url);
                curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, bufferSize);
                curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
                curl_easy_setopt(curl, CURLOPT_USERAGENT, HTTP_USER_AGENT);
                curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long) HTTP_TIMEOUT_SEC);
                curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long) HTTP_MAX_REDIRECTS);
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long) CURL_HTTP_VERSION_2TLS);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_curl_write_callback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*) &curlData);
                curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, http_curl_xfer_info_callback);
                curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void*) &curlData);

                // Curl is setup. We perform a blocking transfer.
                CURLcode ret = curl_easy_perform(curl);

                // Checking of the transfer finished OK, and if curlData holds
                // data (pos != 0) it calls the provided callback to save the
                // data.
                if(ret == CURLE_OK && curlData.pos != 0) {
                    curlData.res = curlData.callback(curlData.userData, curlData.buf, curlData.pos);
                    curlData.pos = 0;
                }

                res = curlData.res;

                // Checking if it finished succesfully, if not report the responsecode reported.
                if(R_SUCCEEDED(res) && ret != CURLE_OK) {
                    if(ret == CURLE_HTTP_RETURNED_ERROR) {
                        long responseCode = 0;
                        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);

                        res = R_APP_HTTP_ERROR_BASE + responseCode;
                    } else {
                        res = R_APP_CURL_ERROR_BASE + ret;
                    }
                }

                curl_easy_cleanup(curl);
            } else {
                res = R_APP_CURL_INIT_FAILED;
            }
        }

        free(buf);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}

typedef struct {
    void* buf;
    size_t size;

    size_t pos;
} http_buffer_data;


/**
 * @brief      Copies downloaded data (userData) into buffer.
 *
 * @details    Given an userData pointer it copies it's buffer into the provided buffer.
 *
 * @param      userData - Download data
 *
 * @param      buffer - Buffer where to store the downloaded resource.
 *
 * @param      size - The size of the provided buffer.
 *
 * @return     Result - Result of the operation, in this case always 0.
 */
static Result http_download_buffer_callback(void* userData, void* buffer, size_t size) {
    http_buffer_data* data = (http_buffer_data*) userData;

    size_t remaining = data->size - data->pos;
    size_t copySize = size;
    if(copySize > remaining) {
        copySize = remaining;
    }

    if(copySize > 0) {
        memcpy((u8*) data->buf + data->pos, buffer, copySize);
        data->pos += copySize;
    }

    return 0;
}

/**
 * @brief      Downloads the provided url into the provided buffer.
 *
 * @details    Downloads the provided url into the provided buffer using the http_download_callback function and the http_download_buffer_callback as default callbadk.
 *
 * @param      url - Url of the resource to download.
 *
 * @param      downloadedSize - Pointer to an u32 to save the size of the downloaed resource.
 *
 * @param      buf - Buffer where to store the downloaded resource.
 *
 * @param      size - The size of the provided buffer.
 *
 * @return     Result - result of the call to http_downlo_callback.
 */
Result http_download_buffer(const char* url, u32* downloadedSize, void* buf, size_t size) {
    http_buffer_data data = {buf, size, 0};
    Result res = http_download_callback(url, size, &data, http_download_buffer_callback, NULL, NULL);

    if(R_SUCCEEDED(res)) {
        *downloadedSize = data.pos;
    }

    return res;
}


/**
 * @brief      Downloads the provided url and converts it into a json_t struct.
 *
 * @details    Downloads the provided url using the http_download_callback function and then parses the content as json.
 *
 * @param      url - Url of the resource to download.
 *
 * @param      json - Pointer to a json_t.
 *
 * @param      maxSize - The maximum size of the download, used to create a temporary buffer.
 *
 * @return     Result - result of the call to http_downlo_callback and loading of the json.
 */
Result http_download_json(const char* url, json_t** json, size_t maxSize) {
    if(url == NULL || json == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    Result res = 0;
    // Creates a temporal buffer.
    char* text = (char*) calloc(sizeof(char), maxSize);
    if(text != NULL) {
        u32 textSize = 0;
        // Downloading the url into the temporal buffer.
        if(R_SUCCEEDED(res = http_download_buffer(url, &textSize, text, maxSize))) {
            json_error_t error;
            // Parsing the json received.
            json_t* parsed = json_loads(text, 0, &error);
            if(parsed != NULL) {
                *json = parsed;
            } else {
                res = R_APP_PARSE_FAILED;
            }
        }

        free(text);
    } else {
      // Allocation failed
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}

static Result FSUSER_AddSeed(u64 titleId, const void* seed) {
    u32 *cmdbuf = getThreadCommandBuffer();

    cmdbuf[0] = 0x087A0180;
    cmdbuf[1] = (u32) (titleId & 0xFFFFFFFF);
    cmdbuf[2] = (u32) (titleId >> 32);
    memcpy(&cmdbuf[3], seed, 16);

    Result ret = 0;
    if(R_FAILED(ret = svcSendSyncRequest(*fsGetSessionHandle()))) return ret;

    ret = cmdbuf[1];
    return ret;
}

/**
 * @brief      Downloads & Install the seed for the provided titleId.
 *
 * @details    Downloads and installs the seed for the provided titleId from the Nintendo cdn. The seed is used to decrypt data from the game.
 *
 * @param      titleId - titleId of the game in u64.
 *
 * @return     Result - Result of the download,failed, bad_data, out of memory,etc....
 */
Result http_download_seed(u64 titleId) {
    char pathBuf[64];
    snprintf(pathBuf, 64, "/fbi/seed/%016llX.dat", titleId);

    Result res = 0;

    FS_Path* fsPath = fs_make_path_utf8(pathBuf);
    if(fsPath != NULL) {
        u8 seed[16];

        Handle fileHandle = 0;
        if(R_SUCCEEDED(res = FSUSER_OpenFileDirectly(&fileHandle, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), *fsPath, FS_OPEN_READ, 0))) {
            u32 bytesRead = 0;
            res = FSFILE_Read(fileHandle, &bytesRead, 0, seed, sizeof(seed));

            FSFILE_Close(fileHandle);
        }

        fs_free_path_utf8(fsPath);

        if(R_FAILED(res)) {
            u8 region = CFG_REGION_USA;
            CFGU_SecureInfoGetRegion(&region);

            if(region <= CFG_REGION_TWN) {
                static const char* regionStrings[] = {"JP", "US", "GB", "GB", "HK", "KR", "TW"};

                char url[128];
                snprintf(url, 128, "https://kagiya-ctr.cdn.nintendo.net/title/0x%016llX/ext_key?country=%s", titleId, regionStrings[region]);

                u32 downloadedSize = 0;
                if(R_SUCCEEDED(res = http_download_buffer(url, &downloadedSize, seed, sizeof(seed))) && downloadedSize != sizeof(seed)) {
                    res = R_APP_BAD_DATA;
                }
            } else {
                res = R_APP_OUT_OF_RANGE;
            }
        }

        if(R_SUCCEEDED(res)) {
            res = FSUSER_AddSeed(titleId, seed);
        }
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}
