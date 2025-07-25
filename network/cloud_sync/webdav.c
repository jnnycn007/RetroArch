/*  RetroArch - A frontend for libretro.
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <encodings/base64.h>
#include <lrc_hash.h>
#include <net/net_http.h>
#include <string/stdstring.h>
#include <time/rtime.h>

#include "../cloud_sync_driver.h"
#include "../../retroarch.h"
#include "../../tasks/tasks_internal.h"
#include "../../verbosity.h"

typedef struct
{
   char path[PATH_MAX_LENGTH];
   char file[PATH_MAX_LENGTH];
   cloud_sync_complete_handler_t cb;
   void *user_data;
   RFILE *rfile;
} webdav_cb_state_t;

typedef void (*webdav_mkdir_cb_t)(bool success, webdav_cb_state_t *state);

typedef struct
{
   char url[PATH_MAX_LENGTH];
   char *last_slash;
   char post_slash;
   webdav_mkdir_cb_t cb;
   webdav_cb_state_t *cb_st;
} webdav_mkdir_state_t;

/* TODO: all of this HTTP auth stuff should
 * probably live in libretro-common/net? */
typedef struct
{
   char url[PATH_MAX_LENGTH];

   bool basic;
   char *basic_auth_header;

   char *username;
   char *ha1hash;
   char *realm;
   char *nonce;
   char *algo;
   char *opaque;
   char *cnonce;
   bool qop_auth;
   unsigned nc;
} webdav_state_t;

static webdav_state_t webdav_driver_st = {0};

webdav_state_t *webdav_state_get_ptr(void)
{
   return &webdav_driver_st;
}

static char *webdav_create_basic_auth(void)
{
   int         flen;
   char       *base64auth;
   char        userpass[512];
   settings_t *settings  = config_get_ptr();
   size_t      _len      = 0;
   if (!string_is_empty(settings->arrays.webdav_username))
      _len += strlcpy(userpass + _len, settings->arrays.webdav_username, sizeof(userpass) - _len);
   userpass[_len++] = ':';
   if (!string_is_empty(settings->arrays.webdav_password))
      _len += strlcpy(userpass + _len, settings->arrays.webdav_password, sizeof(userpass) - _len);
   userpass[_len]   = '\0';
   base64auth = base64(userpass, (int)_len, &flen);
   _len  = strlcpy(userpass, "Authorization: Basic ", sizeof(userpass));
   _len += strlcpy(userpass + _len, base64auth, sizeof(userpass) - _len);
   free(base64auth);
   userpass[_len++] = '\r';
   userpass[_len++] = '\n';
   userpass[_len  ] = '\0';
   return strdup(userpass);
}

static void webdav_cleanup_digest(void)
{
   webdav_state_t *webdav_st = webdav_state_get_ptr();

   if (webdav_st->ha1hash)
      free(webdav_st->ha1hash);
   webdav_st->ha1hash = NULL;

   if (webdav_st->realm)
      free(webdav_st->realm);
   webdav_st->realm = NULL;

   if (webdav_st->nonce)
      free(webdav_st->nonce);
   webdav_st->nonce = NULL;

   if (webdav_st->algo)
      free(webdav_st->algo);
   webdav_st->algo = NULL;

   if (webdav_st->opaque)
      free(webdav_st->opaque);
   webdav_st->opaque = NULL;

   webdav_st->qop_auth = false;
   webdav_st->nc = 1;
}

static char *webdav_create_ha1_hash(char *user, char *realm, char *pass)
{
   char           *hash      = malloc(33);
   MD5_CTX         md5;
   unsigned char   digest[16];

   MD5_Init(&md5);
   MD5_Update(&md5, user, strlen(user));
   MD5_Update(&md5, ":", 1);
   MD5_Update(&md5, realm, strlen(realm));
   MD5_Update(&md5, ":", 1);
   MD5_Update(&md5, pass, strlen(pass));
   MD5_Final(digest, &md5);

   snprintf(hash, 33, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
            digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7],
            digest[8], digest[9], digest[10], digest[11], digest[12], digest[13], digest[14], digest[15]
      );

   return hash;
}

static bool webdav_create_digest_auth(char *digest)
{
   size_t _len;
   webdav_state_t *webdav_st = webdav_state_get_ptr();
   settings_t     *settings  = config_get_ptr();
   char           *ptr       = digest + STRLEN_CONST("WWW-Authenticate: Digest ");
   char           *end       = ptr + strlen(ptr);

   if (   string_is_empty(settings->arrays.webdav_username)
       && string_is_empty(settings->arrays.webdav_password))
      return false;

   webdav_cleanup_digest();

   webdav_st->username = settings->arrays.webdav_username;

   while (ptr < end)
   {
      while (ISSPACE(*ptr))
         ++ptr;

      if (!*ptr)
         break;

      if (string_starts_with(ptr, "realm=\""))
      {
         ptr += STRLEN_CONST("realm=\"");
         _len = strchr(ptr, '"') + 1 - ptr;
         webdav_st->realm = malloc(_len);
         strlcpy(webdav_st->realm, ptr, _len);
         ptr += _len;

         webdav_st->ha1hash = webdav_create_ha1_hash(
               webdav_st->username, webdav_st->realm,
               settings->arrays.webdav_password);
      }
      else if (string_starts_with(ptr, "qop=\""))
      {
         char *tail;
         ptr += STRLEN_CONST("qop=\"");
         tail = strchr(ptr, '"');
         while (ptr < tail)
         {
            if (    string_starts_with(ptr, "auth")
                && (ptr[4] == ',' || ptr[4] == '"'))
            {
               webdav_st->qop_auth = true;
               break;
            }
            while (*ptr != ',' && *ptr != '"' && *ptr != '\0')
               ptr++;
            ptr++;
         }
         /* not even going to try for auth-int, sorry */
         if (!webdav_st->qop_auth)
            return false;
         while (*ptr != ',' && *ptr != '"' && *ptr != '\0')
            ptr++;
         ptr++;
      }
      else if (string_starts_with(ptr, "nonce=\""))
      {
         ptr += STRLEN_CONST("nonce=\"");
         _len = strchr(ptr, '"') + 1 - ptr;
         webdav_st->nonce = malloc(_len);
         strlcpy(webdav_st->nonce, ptr, _len);
         ptr += _len;
      }
      else if (string_starts_with(ptr, "algorithm="))
      {
         ptr += STRLEN_CONST("algorithm=");
         if (strchr(ptr, ','))
         {
            _len = strchr(ptr, ',') + 1 - ptr;
            webdav_st->algo = malloc(_len);
            strlcpy(webdav_st->algo, ptr, _len);
            ptr += _len;
         }
         else
         {
            webdav_st->algo = strdup(ptr);
            ptr += strlen(ptr);
         }
      }
      else if (string_starts_with(ptr, "opaque=\""))
      {
         ptr += STRLEN_CONST("opaque=\"");
         _len = strchr(ptr, '"') + 1 - ptr;
         webdav_st->opaque = malloc(_len);
         strlcpy(webdav_st->opaque, ptr, _len);
         ptr += _len;
      }
      else
      {
         while (*ptr != '=' && *ptr != '\0')
            ptr++;
         ptr++;
         if (*ptr == '"')
         {
            ptr++;
            while (*ptr != '"' && *ptr != '\0')
               ptr++;
            ptr++;
         }
         else
         {
            while (*ptr != ',' && *ptr != ',')
               ptr++;
         }
      }

      while (ISSPACE(*ptr))
         ++ptr;
      if (*ptr == ',')
         ptr++;
   }

   if (!webdav_st->ha1hash || !webdav_st->nonce)
      return false;

   webdav_st->cnonce = "1a2b3c4f";
   webdav_st->basic = false;

   return true;
}

static char *webdav_create_ha1(void)
{
   webdav_state_t *webdav_st = webdav_state_get_ptr();
   char           *hash;
   MD5_CTX         md5;
   unsigned char   digest[16];

   if (!string_is_equal(webdav_st->algo, "MD5-sess"))
      return strdup(webdav_st->ha1hash);

   hash = malloc(33);

   MD5_Init(&md5);
   MD5_Update(&md5, webdav_st->ha1hash, 32);
   MD5_Update(&md5, ":", 1);
   MD5_Update(&md5, webdav_st->nonce, strlen(webdav_st->nonce));
   MD5_Update(&md5, ":", 1);
   MD5_Update(&md5, webdav_st->cnonce, strlen(webdav_st->cnonce));
   MD5_Final(digest, &md5);

   snprintf(hash, 33, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
            digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7],
            digest[8], digest[9], digest[10], digest[11], digest[12], digest[13], digest[14], digest[15]
      );

   return hash;
}

static char *webdav_create_ha2(const char *method, const char *path)
{
   MD5_CTX         md5;
   unsigned char   digest[16];
   /* no attempt at supporting auth-int, everything else uses this */
   char           *hash      = malloc(33);

   MD5_Init(&md5);
   MD5_Update(&md5, method, strlen(method));
   MD5_Update(&md5, ":", 1);
   MD5_Update(&md5, path, strlen(path));
   MD5_Final(digest, &md5);

   snprintf(hash, 33, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
            digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7],
            digest[8], digest[9], digest[10], digest[11], digest[12], digest[13], digest[14], digest[15]
      );

   return hash;
}

static char *webdav_create_digest_response(const char *method, const char *path)
{
   MD5_CTX         md5;
   unsigned char   digest[16];
   webdav_state_t *webdav_st = webdav_state_get_ptr();
   char           *ha1       = webdav_create_ha1();
   char           *ha2       = webdav_create_ha2(method, path);
   char           *hash      = malloc(33);

   MD5_Init(&md5);
   MD5_Update(&md5, ha1, 32);
   MD5_Update(&md5, ":", 1);
   MD5_Update(&md5, webdav_st->nonce, strlen(webdav_st->nonce));
   if (webdav_st->qop_auth)
   {
      char nonceCount[10];
      snprintf(nonceCount, sizeof(nonceCount), "%08x", webdav_st->nc);
      MD5_Update(&md5, ":", 1);
      MD5_Update(&md5, nonceCount, strlen(nonceCount));
      MD5_Update(&md5, ":", 1);
      MD5_Update(&md5, webdav_st->cnonce, strlen(webdav_st->cnonce));
      MD5_Update(&md5, ":", 1);
      MD5_Update(&md5, "auth", STRLEN_CONST("auth"));
   }
   MD5_Update(&md5, ":", 1);
   MD5_Update(&md5, ha2, 32);
   MD5_Final(digest, &md5);

   snprintf(hash, 33, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
            digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7],
            digest[8], digest[9], digest[10], digest[11], digest[12], digest[13], digest[14], digest[15]
      );

   free(ha1);
   free(ha2);

   return hash;
}

static char *webdav_create_digest_auth_header(const char *method, const char *url)
{
   size_t _len, __len;
   char nonceCount[10];
   webdav_state_t *webdav_st = webdav_state_get_ptr();
   char           *header    = NULL;
   char           *response  = NULL;
   const char     *path      = url;
   int             count     = 0;
   size_t          total     = 0;

   do
   {
      path++;
      path = strchr(path, '/');
      count++;
   } while (count < 3 && *path != '\0');

   response = webdav_create_digest_response(method, path);
   __len    = snprintf(nonceCount, sizeof(nonceCount),
         "%08x", webdav_st->nc++);

   _len  = STRLEN_CONST("Authorization: Digest ");
   _len += STRLEN_CONST("username=\"") + strlen(webdav_st->username) + STRLEN_CONST("\", ");
   _len += STRLEN_CONST("realm=\"")    + strlen(webdav_st->realm) + STRLEN_CONST("\", ");
   _len += STRLEN_CONST("nonce=\"")    + strlen(webdav_st->nonce) + STRLEN_CONST("\", ");
   _len += STRLEN_CONST("uri=\"")      + strlen(path) + STRLEN_CONST("\", ");
   _len += STRLEN_CONST("nc=\"")       + __len + STRLEN_CONST("\", ");
   _len += STRLEN_CONST("cnonce=\"")   + strlen(webdav_st->cnonce) + STRLEN_CONST("\", ");
   if (webdav_st->qop_auth)
      _len += STRLEN_CONST("qop=\"auth\", ");
   if (webdav_st->opaque)
      _len += STRLEN_CONST("opaque=\"") + strlen(webdav_st->opaque) + STRLEN_CONST("\", ");
   _len += STRLEN_CONST("response=\"")  + strlen(response) + STRLEN_CONST("\"\r\n");
   _len += 1;

   total  = _len;
   _len   = 0;
   header = (char*)malloc(total);
   _len   = strlcpy(header, "Authorization: Digest username=\"", total - _len);
   _len  += strlcpy(header + _len, webdav_st->username, total - _len);
   _len  += strlcpy(header + _len, "\", realm=\"", total - _len);
   _len  += strlcpy(header + _len, webdav_st->realm, total - _len);
   _len  += strlcpy(header + _len, "\", nonce=\"", total - _len);
   _len  += strlcpy(header + _len, webdav_st->nonce, total - _len);
   _len  += strlcpy(header + _len, "\", uri=\"", total - _len);
   _len  += strlcpy(header + _len, path, total - _len);
   _len  += strlcpy(header + _len, "\", nc=\"", total - _len);
   _len  += strlcpy(header + _len, nonceCount, total - _len);
   _len  += strlcpy(header + _len, "\", cnonce=\"", total - _len);
   _len  += strlcpy(header + _len, webdav_st->cnonce, total - _len);
   if (webdav_st->qop_auth)
      _len += strlcpy(header + _len, "\", qop=\"auth", total - _len);
   if (webdav_st->opaque)
   {
      _len += strlcpy(header + _len, "\", opaque=\"", total - _len);
      _len += strlcpy(header + _len, webdav_st->opaque, total - _len);
   }
   _len += strlcpy(header + _len, "\", response=\"", total - _len);
   _len += strlcpy(header + _len, response, total - _len);
   strlcpy(header + _len, "\"\r\n", total - _len);
   free(response);
   return header;
}

static char *webdav_get_auth_header(const char *method, const char *url)
{
   webdav_state_t *webdav_st = webdav_state_get_ptr();
   settings_t     *settings  = config_get_ptr();

   if (   string_is_empty(settings->arrays.webdav_username)
       && string_is_empty(settings->arrays.webdav_password))
      return NULL;

   if (webdav_st->basic)
   {
      if (!webdav_st->basic_auth_header)
         webdav_st->basic_auth_header = webdav_create_basic_auth();
      return strdup(webdav_st->basic_auth_header);
   }

   return webdav_create_digest_auth_header(method, url);
}

static void webdav_log_http_failure(const char *path, http_transfer_data_t *data)
{
    size_t i;
    RARCH_WARN("[webdav] Failed: %s: HTTP %d\n", path, data->status);
    for (i = 0; data->headers && i < data->headers->size; i++)
        RARCH_WARN("%s\n", data->headers->elems[i].data);
    if (data->data)
    {
        data->data[data->len] = 0;
        RARCH_WARN("%s\n", data->data);
    }
}

static bool webdav_needs_reauth(http_transfer_data_t *data)
{
   size_t i;

   if (!data || data->status != 401 || !data->headers)
      return false;

   for (i = 0; i < data->headers->size; i++)
   {
      if (!string_starts_with(data->headers->elems[i].data, "WWW-Authenticate: Digest "))
         continue;

      RARCH_DBG("[webdav] Found WWW-Authenticate: Digest header\n");
      if (webdav_create_digest_auth(data->headers->elems[i].data))
         return true;
      else
         RARCH_WARN("[webdav] Failure creating WWW-Authenticate: Digest header\n");
   }

   return false;
}

static void webdav_stat_cb(retro_task_t *task, void *task_data, void *user_data, const char *err)
{
   webdav_state_t       *webdav_st    = webdav_state_get_ptr();
   webdav_cb_state_t    *webdav_cb_st = (webdav_cb_state_t *)user_data;
   http_transfer_data_t *data         = (http_transfer_data_t*)task_data;
   bool                  success      = (data && data->status >= 200 && data->status < 300);

   if (!webdav_cb_st)
      return;

   if (!data)
      RARCH_WARN("[webdav] Did not get data for stat, is the server down?\n");

   if (webdav_needs_reauth(data))
   {
      char *auth_header = webdav_get_auth_header("OPTIONS", webdav_st->url);
      task_push_webdav_stat(webdav_st->url, true, auth_header, webdav_stat_cb, webdav_cb_st);
      free(auth_header);
      return;
   }

   if (!success && data)
       webdav_log_http_failure(webdav_st->url, data);

   webdav_cb_st->cb(webdav_cb_st->user_data, NULL, success, NULL);
   free(webdav_cb_st);
}

static bool webdav_sync_begin(cloud_sync_complete_handler_t cb, void *user_data)
{
   char *auth_header;
   size_t _len                  = 0;
   settings_t        *settings  = config_get_ptr();
   const char        *url       = settings->arrays.webdav_url;
   webdav_state_t    *webdav_st = webdav_state_get_ptr();

   if (string_is_empty(url))
      return false;

#ifndef HAVE_SSL
   if (strncmp(url, "https", 5) == 0)
      return false;
#endif
   /* TODO/FIXME: LOCK? */
   if (!strstr(url, "://"))
       _len += strlcpy(webdav_st->url, "http://", STRLEN_CONST("http://"));
   strlcpy(webdav_st->url + _len, url, sizeof(webdav_st->url) - _len);
   fill_pathname_slash(webdav_st->url, sizeof(webdav_st->url));

   /* URL/username/password may have changed, redo auth check */
   webdav_st->basic = true;
   auth_header      = webdav_get_auth_header(NULL, NULL);

   if (auth_header)
   {
      webdav_cb_state_t *webdav_cb_st = (webdav_cb_state_t*)calloc(1, sizeof(webdav_cb_state_t));
      webdav_cb_st->cb        = cb;
      webdav_cb_st->user_data = user_data;
      task_push_webdav_stat(webdav_st->url, true, auth_header, webdav_stat_cb, webdav_cb_st);
      free(auth_header);
   }
   else
   {
      RARCH_WARN("[webdav] No basic auth header, assuming no user, check username/password?\n");
      cb(user_data, NULL, true, NULL);
   }
   return true;
}

static bool webdav_sync_end(cloud_sync_complete_handler_t cb, void *user_data)
{
   webdav_state_t *webdav_st = webdav_state_get_ptr();

   /* TODO/FIXME: UNLOCK? */

   if (webdav_st->basic_auth_header)
      free(webdav_st->basic_auth_header);
   webdav_st->basic_auth_header = NULL;

   webdav_cleanup_digest();

   cb(user_data, NULL, true, NULL);
   return true;
}

static void webdav_read_cb(retro_task_t *task, void *task_data, void *user_data, const char *err)
{
   webdav_cb_state_t    *webdav_cb_st = (webdav_cb_state_t *)user_data;
   http_transfer_data_t *data         = (http_transfer_data_t*)task_data;
   RFILE                *file         = NULL;
   bool success = (data
              && ((data->status >= 200 && data->status < 300) || data->status == 404));

   if (!success && data)
       webdav_log_http_failure(webdav_cb_st->path, data);

   if (webdav_needs_reauth(data))
   {
      webdav_state_t *webdav_st = webdav_state_get_ptr();
      char            url[PATH_MAX_LENGTH];
      char            url_encoded[PATH_MAX_LENGTH];
      char           *auth_header;

      fill_pathname_join_special(url, webdav_st->url, webdav_cb_st->path, sizeof(url));
      net_http_urlencode_full(url_encoded, url, sizeof(url_encoded));

      RARCH_DBG("[webdav] GET %s\n", url_encoded);
      auth_header = webdav_get_auth_header("GET", url_encoded);
      task_push_http_transfer_with_headers(url_encoded, true, NULL, auth_header, webdav_read_cb, webdav_cb_st);
      free(auth_header);
      return;
   }

   if (success && data->data && webdav_cb_st)
   {
      /* TODO/FIXME: it would be better if writing
       * to the file happened during the network reads */
      file = filestream_open(webdav_cb_st->file,
                             RETRO_VFS_FILE_ACCESS_READ_WRITE,
                             RETRO_VFS_FILE_ACCESS_HINT_NONE);
      if (file)
      {
         filestream_write(file, data->data, data->len);
         filestream_seek(file, 0, SEEK_SET);
      }
   }

   if (webdav_cb_st)
   {
      webdav_cb_st->cb(webdav_cb_st->user_data,
            webdav_cb_st->path, success, file);
      free(webdav_cb_st);
   }
}

static bool webdav_read(const char *path, const char *file,
      cloud_sync_complete_handler_t cb, void *user_data)
{
   void              *t;
   char              *auth_header;
   char               url[PATH_MAX_LENGTH];
   char               url_encoded[PATH_MAX_LENGTH];
   webdav_state_t    *webdav_st    = webdav_state_get_ptr();
   webdav_cb_state_t *webdav_cb_st = (webdav_cb_state_t*)calloc(1, sizeof(webdav_cb_state_t));

   fill_pathname_join_special(url, webdav_st->url, path, sizeof(url));
   net_http_urlencode_full(url_encoded, url, sizeof(url_encoded));

   webdav_cb_st->cb = cb;
   webdav_cb_st->user_data = user_data;
   strlcpy(webdav_cb_st->path, path, sizeof(webdav_cb_st->path));
   strlcpy(webdav_cb_st->file, file, sizeof(webdav_cb_st->file));

   RARCH_DBG("[webdav] GET %s\n", url_encoded);
   auth_header = webdav_get_auth_header("GET", url_encoded);
   t = task_push_http_transfer_with_headers(url_encoded, true, NULL,
         auth_header, webdav_read_cb, webdav_cb_st);
   free(auth_header);
   return (t != NULL);
}

static void webdav_mkdir_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   char *auth_header;
   webdav_mkdir_state_t *webdav_mkdir_st = (webdav_mkdir_state_t *)user_data;
   http_transfer_data_t *data            = (http_transfer_data_t*)task_data;

   if (!webdav_mkdir_st)
      return;

   if (webdav_needs_reauth(data))
   {
      RARCH_DBG("[webdav] MKCOL %s\n", webdav_mkdir_st->url);
      auth_header = webdav_get_auth_header("MKCOL", webdav_mkdir_st->url);
      task_push_webdav_mkdir(webdav_mkdir_st->url, true, auth_header, webdav_mkdir_cb, webdav_mkdir_st);
      free(auth_header);
      return;
   }

   /* HTTP 405 on MKCOL means it's already there */
   if (!data || data->status < 200 || (data->status >= 400 && data->status != 405))
   {
      if (data)
         webdav_log_http_failure(webdav_mkdir_st->url, data);
      else
         RARCH_WARN("[webdav] Could not mkdir %s\n", webdav_mkdir_st ? webdav_mkdir_st->url : "<unknown>");
      webdav_mkdir_st->cb(false, webdav_mkdir_st->cb_st);
      free(webdav_mkdir_st);
      return;
   }

   webdav_mkdir_st->last_slash[1] = webdav_mkdir_st->post_slash;
   webdav_mkdir_st->last_slash = strchr(webdav_mkdir_st->last_slash + 1, '/');
   if (webdav_mkdir_st->last_slash)
   {
      webdav_mkdir_st->post_slash = webdav_mkdir_st->last_slash[1];
      webdav_mkdir_st->last_slash[1] = '\0';
      RARCH_DBG("[webdav] MKCOL %s\n", webdav_mkdir_st->url);
      auth_header = webdav_get_auth_header("MKCOL", webdav_mkdir_st->url);
      task_push_webdav_mkdir(webdav_mkdir_st->url, true, auth_header, webdav_mkdir_cb, webdav_mkdir_st);
      free(auth_header);
   }
   else
   {
      RARCH_DBG("[webdav] MKCOL %s success\n", webdav_mkdir_st->url);
      webdav_mkdir_st->cb(true, webdav_mkdir_st->cb_st);
      free(webdav_mkdir_st);
   }
}

static void webdav_ensure_dir(const char *dir, webdav_mkdir_cb_t cb,
      webdav_cb_state_t *webdav_cb_st)
{
   http_transfer_data_t  data;
   webdav_state_t       *webdav_st       = webdav_state_get_ptr();
   webdav_mkdir_state_t *webdav_mkdir_st = (webdav_mkdir_state_t *)malloc(sizeof(webdav_mkdir_state_t));
   char                  url[PATH_MAX_LENGTH];

   fill_pathname_join_special(url, webdav_st->url, dir, sizeof(url));
   net_http_urlencode_full(webdav_mkdir_st->url, url, sizeof(webdav_mkdir_st->url));
   webdav_mkdir_st->last_slash = strchr(webdav_mkdir_st->url + strlen(webdav_st->url) - 1, '/');
   webdav_mkdir_st->post_slash = webdav_mkdir_st->last_slash[1];
   webdav_mkdir_st->cb = cb;
   webdav_mkdir_st->cb_st = webdav_cb_st;

   /* this is a recursive callback, set it up so it looks like it's still proceeding */
   data.status = 200;
   webdav_mkdir_cb(NULL, &data, webdav_mkdir_st, NULL);
}

static void webdav_do_update(bool success, webdav_cb_state_t *webdav_cb_st);

static void webdav_update_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   webdav_cb_state_t    *webdav_cb_st = (webdav_cb_state_t *)user_data;
   http_transfer_data_t *data         = (http_transfer_data_t*)task_data;
   bool                  success      = (data && data->status >= 200 && data->status < 300);

   if (!success && data)
       webdav_log_http_failure(webdav_cb_st->path, data);
   else if (!data)
      RARCH_WARN("[webdav] Could not upload %s\n", webdav_cb_st ? webdav_cb_st->path : "<unknown>");

   if (webdav_needs_reauth(data))
   {
      webdav_do_update(true, webdav_cb_st);
      return;
   }

   if (webdav_cb_st)
   {
      webdav_cb_st->cb(webdav_cb_st->user_data, webdav_cb_st->path, success, webdav_cb_st->rfile);
      free(webdav_cb_st);
   }
   else
      RARCH_WARN("[webdav] Missing cb data in update?\n");
}

static void webdav_do_update(bool success, webdav_cb_state_t *webdav_cb_st)
{
   webdav_state_t *webdav_st = webdav_state_get_ptr();
   char            url_encoded[PATH_MAX_LENGTH];
   char            url[PATH_MAX_LENGTH];
   void           *buf;
   int64_t         len;
   char           *auth_header;

   if (!webdav_cb_st)
      return;

   if (!success)
   {
      RARCH_DBG("[webdav] Cannot upload %s\n", webdav_cb_st->path);
      webdav_cb_st->cb(webdav_cb_st->user_data, webdav_cb_st->path, false, webdav_cb_st->rfile);
      free(webdav_cb_st);
      return;
   }

   /* TODO: would be better to read file as it's being written to wire, this is very inefficient */
   len = filestream_get_size(webdav_cb_st->rfile);
   buf = malloc((size_t)(len + 1));
   filestream_read(webdav_cb_st->rfile, buf, len);

   fill_pathname_join_special(url, webdav_st->url, webdav_cb_st->path, sizeof(url));
   net_http_urlencode_full(url_encoded, url, sizeof(url_encoded));

   RARCH_DBG("[webdav] PUT %s\n", url_encoded);
   auth_header = webdav_get_auth_header("PUT", url_encoded);
   task_push_webdav_put(url_encoded, buf, len, true, auth_header, webdav_update_cb, webdav_cb_st);
   free(auth_header);

   free(buf);
}

static bool webdav_update(const char *path, RFILE *rfile,
      cloud_sync_complete_handler_t cb, void *user_data)
{
   webdav_cb_state_t *webdav_cb_st = (webdav_cb_state_t*)calloc(1, sizeof(webdav_cb_state_t));
   char               dir[DIR_MAX_LENGTH];

   /* TODO/FIXME: if !settings->bools.cloud_sync_destructive, should move to deleted/ first */

   webdav_cb_st->cb = cb;
   webdav_cb_st->user_data = user_data;
   strlcpy(webdav_cb_st->path, path, sizeof(webdav_cb_st->path));
   webdav_cb_st->rfile = rfile;

   if (strchr(path, '/'))
   {
      fill_pathname_basedir(dir, path, sizeof(dir));
      webdav_ensure_dir(dir, webdav_do_update, webdav_cb_st);
   }
   else
      webdav_do_update(true, webdav_cb_st);

   return true;
}

static void webdav_delete_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   webdav_cb_state_t    *webdav_cb_st = (webdav_cb_state_t *)user_data;
   http_transfer_data_t *data         = (http_transfer_data_t*)task_data;
   bool                  success      = (data != NULL && data->status >= 200 && data->status < 300);

   if (!success && data)
      webdav_log_http_failure(webdav_cb_st->path, data);
   else if (!data)
      RARCH_WARN("[webdav] Could not delete %s\n", webdav_cb_st ? webdav_cb_st->path : "<unknown>");

   if (webdav_needs_reauth(data))
   {
      webdav_state_t *webdav_st = webdav_state_get_ptr();
      char            url[PATH_MAX_LENGTH];
      char            url_encoded[PATH_MAX_LENGTH];
      char           *auth_header;

      fill_pathname_join_special(url, webdav_st->url, webdav_cb_st->path, sizeof(url));
      net_http_urlencode_full(url_encoded, url, sizeof(url_encoded));

      RARCH_DBG("[webdav] DELETE %s\n", url_encoded);
      auth_header = webdav_get_auth_header("DELETE", url_encoded);
      task_push_webdav_delete(url_encoded, true, auth_header, webdav_delete_cb, webdav_cb_st);
      free(auth_header);
      return;
   }

   if (webdav_cb_st)
   {
      webdav_cb_st->cb(webdav_cb_st->user_data, webdav_cb_st->path, success, NULL);
      free(webdav_cb_st);
   }
   else
      RARCH_WARN("[webdav] Missing cb data in delete?\n");
}

static void webdav_do_backup(bool success, webdav_cb_state_t *webdav_cb_st);

static void webdav_backup_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   webdav_cb_state_t    *webdav_cb_st = (webdav_cb_state_t *)user_data;
   http_transfer_data_t *data         = (http_transfer_data_t*)task_data;
   bool                  success      = (data != NULL && data->status >= 200 && data->status < 300);

   if (!success && data)
       webdav_log_http_failure(webdav_cb_st->path, data);
   else if (!data)
      RARCH_WARN("[webdav] Could not backup %s\n", webdav_cb_st ? webdav_cb_st->path : "<unknown>");

   if (webdav_needs_reauth(data))
   {
      webdav_do_backup(true, webdav_cb_st);
      return;
   }

   if (webdav_cb_st)
   {
      webdav_cb_st->cb(webdav_cb_st->user_data, webdav_cb_st->path, success, NULL);
      free(webdav_cb_st);
   }
   else
      RARCH_WARN("[webdav] Missing cb data in backup?\n");
}

static void webdav_do_backup(bool success, webdav_cb_state_t *webdav_cb_st)
{
   char *auth_header;
   size_t          len;
   struct tm       tm_;
   webdav_state_t *webdav_st = webdav_state_get_ptr();
   char            dest_encoded[PATH_MAX_LENGTH];
   char            dest[PATH_MAX_LENGTH];
   char            url_encoded[PATH_MAX_LENGTH];
   char            url[PATH_MAX_LENGTH];
   time_t          cur_time = time(NULL);

   if (!webdav_cb_st)
      return;

   if (!success)
   {
      RARCH_DBG("[webdav] Cannot backup/delete %s\n", webdav_cb_st->path);
      webdav_cb_st->cb(webdav_cb_st->user_data, webdav_cb_st->path, false, NULL);
      free(webdav_cb_st);
      return;
   }

   fill_pathname_join_special(url, webdav_st->url, webdav_cb_st->path, sizeof(url));
   net_http_urlencode_full(url_encoded, url, sizeof(url_encoded));

   fill_pathname_join_special(url, webdav_st->url, "deleted/", sizeof(url));
   len = fill_pathname_join_special(dest, url, webdav_cb_st->path, sizeof(dest));
   rtime_localtime(&cur_time, &tm_);
   strftime(dest + len, sizeof(dest) - len, "-%y%m%d-%H%M%S", &tm_);
   net_http_urlencode_full(dest_encoded, dest, sizeof(dest_encoded));

   RARCH_DBG("[webdav] MOVE %s -> %s\n", url_encoded, dest_encoded);
   auth_header = webdav_get_auth_header("MOVE", url_encoded);
   task_push_webdav_move(url_encoded, dest_encoded, true, auth_header, webdav_backup_cb, webdav_cb_st);
   free(auth_header);
}

static bool webdav_delete(const char *path, cloud_sync_complete_handler_t cb, void *user_data)
{
   webdav_cb_state_t *webdav_cb_st = (webdav_cb_state_t*)calloc(1, sizeof(webdav_cb_state_t));
   settings_t        *settings     = config_get_ptr();

   webdav_cb_st->cb        = cb;
   webdav_cb_st->user_data = user_data;
   strlcpy(webdav_cb_st->path, path, sizeof(webdav_cb_st->path));

   /*
    * Should all cloud_sync_destructive handling be done in task_cloudsync? I
    * think not because it gives each driver a chance to do a move rather than a
    * delete/update. Or we could add a cloud_sync_move() API to the driver.
    */
   if (settings->bools.cloud_sync_destructive)
   {
      char *auth_header;
      char url[PATH_MAX_LENGTH];
      char url_encoded[PATH_MAX_LENGTH];
      webdav_state_t *webdav_st = webdav_state_get_ptr();

      fill_pathname_join_special(url, webdav_st->url, path, sizeof(url));
      net_http_urlencode_full(url_encoded, url, sizeof(url_encoded));

      RARCH_DBG("[webdav] DELETE %s\n", url_encoded);
      auth_header = webdav_get_auth_header("DELETE", url_encoded);
      task_push_webdav_delete(url_encoded, true, auth_header, webdav_delete_cb, webdav_cb_st);
      free(auth_header);
   }
   else
   {
      char dir[DIR_MAX_LENGTH];
      size_t _len = strlcpy(dir, "deleted/", sizeof(dir));
      fill_pathname_basedir(dir + _len, path, sizeof(dir) - _len);
      webdav_ensure_dir(dir, webdav_do_backup, webdav_cb_st);
   }

   return true;
}

cloud_sync_driver_t cloud_sync_webdav = {
   webdav_sync_begin,
   webdav_sync_end,
   webdav_read,
   webdav_update,
   webdav_delete,
   "webdav" /* ident */
};
