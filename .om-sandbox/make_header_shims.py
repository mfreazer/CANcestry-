#!/usr/bin/env python3
"""Write minimal curl.h / uuid.h shims (sandbox has the runtime .so files
but no dev headers; linking goes straight to the system shared libs)."""
import os

OUT = "/tmp/fakeinc"

CURL_H = r"""/* Minimal curl.h shim for OM runtime om_curl.c (system libcurl.so.4, no dev headers). */
#ifndef __FAKE_CURL_H
#define __FAKE_CURL_H
#include <stddef.h>
#define CURL_STATICLIB
#define CURL_VERSION_NUM 7881
typedef enum {
  CURLE_OK = 0, CURLE_UNSUPPORTED_PROTOCOL, CURLE_FAILED_INIT,
  CURLE_URL_MALFORMAT, CURLE_COULDNT_RESOLVE_PROXY, CURLE_COULDNT_RESOLVE_HOST,
  CURLE_COULDNT_CONNECT, CURLE_WEIRD_SERVER_REPLY, CURLE_REMOTE_ACCESS_DENIED,
  CURLE_HTTP2, CURLE_GOT_NOTHING, CURLE_HTTP_RETURNED_ERROR, CURLE_WRITE_ERROR,
  CURLE_UPLOAD_FAILED, CURLE_READ_ERROR, CURLE_OUT_OF_MEMORY, CURLE_OPERATION_TIMEDOUT,
  CURLE_RANGE_ERROR, CURLE_HTTP_POST_ERROR, CURLE_SSL_CONNECT_ERROR,
  CURLE_BAD_DOWNLOAD_RESUME, CURLE_FILE_COULDNT_READ_FILE, CURLE_LDAP,
  CURLE_FILE_SIZE_EXCEEDED, CURLE_SSL_CIPHER, CURLE_PEER_FAILED_VERIFICATION,
  CURLE_GSS_NAMETYPE, CURLE_SSL_ISSUER_ERROR, CURLE_NO_CONNECT_AVAILABLE,
  CURLE_SEND_FAILED, CURLE_REDIR_LOOP, CURLE_SSL_CACERT_BADFILE,
  CURLE_REMOTE_FILE_NOT_FOUND, CURLE_CHUNK_FAILED,
  CURLE_NO_HOST_DB, CURLE_BAD_FUNCTION_ARGUMENT, CURLE_LOCAL_FILE_NOT_FOUND,
  CURLE_REMOTE_FILE_SIZE_EXCEEDED, CURLE_SSH, CURLE_NO_PROXY, CURLE_FTP_WEIRD_227_FORMAT,
  CURLE_FTP_COULDNT_SET_FILETIME, CURLE_FTP_COULDNT_RETR_FILE, CURLE_FTP_WRITE_ERROR,
  CURLE_FTP_QUOTE_ERROR, CURLE_FTP_SSL_FAILED, CURLE_FILE_COULDNT_WRITE_FILE,
  CURLE_FTP_COULDNT_STOR_FILE, CURLE_FTP_PARTIAL_FILE, CURLE_FTP_ACCESS_DENIED,
  CURLE_FTP_USER_PASSWORD_INCORRECT, CURLE_FTP_SSL_CACERT_BADFILE,
  CURLE_FTP_REMOTE_ACCESS_DENIED, CURLE_FTP_COULDNT_GET_SIZE, CURLE_FTP_COULDNT_PASV,
  CURLE_FTP_WEIRD_PASSV_REPLY, CURLE_FTP_WEIRD_257_FORMAT, CURLE_FTP_COULDNT_CD,
  CURLE_FTP_COULDNOT_USE_REST, CURLE_FTP_COULDNT_RECONNECT, CURLE_FTP_COULDNT_RESET,
  CURLE_SSL_CRL_BADFILE, CURLE_SSL_ENGINE_NOTFOUND, CURLE_SSL_ENGINE_SETFAILED,
  CURLE_SEND_ERROR_RESEND, CURLE_BAD_FILE_ENCODING, CURLE_LDAP_INVALID_URL,
  CURLE_FILESIZE_EXCEEDED, CURLE_FTP_PRET_FAILED, CURLE_RTSP_CSEQ_ERROR,
  CURLE_RTSP_SESSION_ERROR, CURLE_FTP_BAD_FILE_LIST, CURLE_CHUNK_GRAFT_FAILED,
  CURLE_FGPAAR_NOTFOUND, CURLE_FTP_COULDNT_SET_BINARY, CURLE_LOGIN_DENIED,
  CURLE_TFTP_NOTFOUND, CURLE_TFTP_PERM, CURLE_TFTP_ILLEGAL, CURLE_TFTP_UNKNOWN,
  CURLE_TFTP_FILEEXISTS, CURLE_TFTP_NOSUCHUSER, CURLE_LAST
} CURLcode;
typedef enum {
  CURLOPT_PROTOCOLS = 10100, CURLOPT_CUSTOMREQUEST = 10044,
  CURLOPT_WRITEFUNCTION = 10002, CURLOPT_WRITEDATA = 10001,
  CURLOPT_URL = 10102, CURLOPT_PRIVATE = 10190,
  CURLOPT_USERAGENT = 10018, CURLOPT_FOLLOWLOCATION = 10052,
  CURLOPT_FAILONERROR = 10066, CURLOPT_VERBOSE = 10041,
  CURLOPT_CONNECTTIMEOUT = 10059, CURLOPT_CAINFO = 10065,
  CURLOPT_MAXCONNECTS = 62, CURLOPT_ACCEPT_ENCODING = 10109,
  CURLOPT_DNS_CACHE_TIMEOUT = 67, CURLOPT_SSL_VERIFYPEER = 64,
  CURLOPT_SSL_VERIFYHOST = 66
} CURLoption;
typedef struct Curl_easy CURL;
typedef struct Curl_multi CURLM;
typedef void (*curl_write_callback)(char *ptr, size_t size, size_t nmemb, void *userdata);
typedef enum { CURLINFO_PRIVATE = 100050 } CURLINFO;
typedef enum { CURLMOPT_MAXCONNECTS = 0 } CURLMoption;
typedef struct {
  int msg;
  void *easy_handle;
  union { CURLcode result; } data;
} CURLMsg;
#define CURLMSG_DONE 1
#define CURL_GLOBAL_ALL -1
CURL *curl_easy_init(void);
CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...);
CURLcode curl_easy_perform(CURL *curl);
void curl_easy_cleanup(CURL *curl);
CURLcode curl_easy_getinfo(CURL *curl, CURLINFO info, ...);
const char *curl_easy_strerror(CURLcode code);
CURLM *curl_multi_init(void);
int curl_multi_add_handle(CURLM *multi_handle, CURL *easy_handle);
int curl_multi_remove_handle(CURLM *multi_handle, CURL *easy_handle);
int curl_multi_perform(CURLM *multi_handle, int *running_handles);
CURLMsg *curl_multi_info_read(CURLM *multi_handle, int *msgs_in_queue);
int curl_multi_setopt(CURLM *multi_handle, CURLMoption option, ...);
int curl_multi_wait(CURLM *multi_handle, void *extra_fds, int nfds, int timeout, int *err);
int curl_multi_cleanup(CURLM *multi_handle);
int curl_multi_download(CURL *curl);
CURLcode curl_global_init(long flags);
void curl_global_cleanup(void);
const char *curl_ca_bundle_suffix(void);
#endif
"""

UUID_H = r"""/* Minimal uuid.h shim (system libuuid.so.1, no dev headers). */
#ifndef _FAKE_UUID_H
#define _FAKE_UUID_H
#include <stddef.h>
typedef struct { unsigned char time_low[4]; unsigned char time_mid[2];
  unsigned char time_hi_and_version[2]; unsigned char clock_seq[2];
  unsigned char node[6]; } uuid_t;
void uuid_clear(uuid_t uu);
int uuid_compare(const uuid_t uu1, const uuid_t uu2);
void uuid_copy(uuid_t dst, const uuid_t src);
int uuid_is_null(const uuid_t uu);
int uuid_parse(const char *in, uuid_t uu);
void uuid_unparse(const uuid_t uu, char *out);
void uuid_unparse_lower(const uuid_t uu, char *out);
void uuid_unparse_upper(const uuid_t uu, char *out);
int uuid_generate(uuid_t uu);
int uuid_generate_random(uuid_t uu);
int uuid_generate_time(uuid_t uu);
int uuid_generate_time_safe(uuid_t uu);
int uuid_generate_md5(uuid_t out, const uuid_t ns, const char *name, size_t len);
int uuid_generate_sha1(uuid_t out, const uuid_t ns, const char *name, size_t len);
#endif
"""

os.makedirs(os.path.join(OUT, "curl"), exist_ok=True)
os.makedirs(os.path.join(OUT, "uuid"), exist_ok=True)
open(os.path.join(OUT, "curl", "curl.h"), "w").write(CURL_H)
open(os.path.join(OUT, "uuid", "uuid.h"), "w").write(UUID_H)
print("header shims written to", OUT)
