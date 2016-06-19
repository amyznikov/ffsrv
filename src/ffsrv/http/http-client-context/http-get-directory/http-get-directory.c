/*
 * http-get-saved-streams.c
 *
 *  Created on: Jun 7, 2016
 *      Author: amyznikov
 */

#include "http-get-directory.h"

#include "ffcfg.h"
#include "strfuncs.h"
#include "debug.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>


#define fsz2str(size) \
  sz2str((size), (char [64]){0})

static char * sz2str(size_t size, char buf[64])
{
  if ( size < 1024 ) {
    sprintf(buf,"%zu B", size);
  }
  else if ( size < 1024 * 1024 ) {
    sprintf(buf,"%g KB", size / 1024.0 );
  }
  else if ( size < 1024 * 1024 * 1024 ) {
    sprintf(buf, "%g MB", size / (1024.0 * 1024.0));
  }
  else {
    sprintf(buf, "%g GB", size / (1024.0 * 1024.0 * 1024.0));
  }

  return buf;
}


static bool strendswith(const char * str, const char * substr)
{
  size_t s1 = strlen(str);
  size_t s2 = strlen(substr);
  if ( s1 >= s2 ) {
    return strcmp(str + s1 - 2, substr) == 0;
  }
  return false;
}

static void send_directory_contents(const char * root, const char * path, struct http_client_ctx * client_ctx)
{
  struct dirent ** entry = NULL;
  struct stat fileinfo;
  char buf[PATH_MAX];
  char * base;

  int i, n = 0;

  const struct http_request * q = &client_ctx->req;
  bool fok;

  sprintf(buf, "%s/%s", root, path);

  PDBG("scandir('%s')", buf);


  if ( (n = scandir(buf, &entry, NULL, alphasort)) < 0 ) {
    http_ssend(client_ctx,
        "%s 404 Not Found\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<html>\r\n"
            "<body>\r\n"
            "<p>scandir('%s') fails:</p>\r\n"
            "<p>errno=%d %s</p>\r\n"
            "</body>\r\n"
            "</html>\r\n",
        q->proto,
        path,
        errno,
        strerror(errno));
    goto end;
  }


  fok = http_ssend(client_ctx,
      "%s 200 OK\r\n"
          "Content-Type: text/html; charset=utf-8\r\n"
          "Connection: close\r\n"
          "\r\n"
          "<html>\r\n"
            "<body>\r\n",
      q->proto);

  if ( !fok ) {
    goto end;
  }


  for ( i = 0; i < n; ++i ) {

    if ( entry[i]->d_type == DT_DIR ) {

      if ( strcmp(entry[i]->d_name, ".") == 0 || strendswith(entry[i]->d_name, "~") ) {
        continue;
      }


      if ( strcmp(entry[i]->d_name, "..") != 0 ) {

        fok = http_ssend(client_ctx, "<p><a href=/%s/%s>%s/</a></p>\n",
            path, entry[i]->d_name, entry[i]->d_name);
      }
      else {
        for ( base = buf + snprintf(buf, sizeof(buf), "%s", path); base > buf && *base != '/'; ) {
          --base;
        }
        *base = 0;
        fok = http_ssend(client_ctx, "<p><a href=/%s>..</a></p>\n", buf);
      }
    }

    else if ( entry[i]->d_type == DT_REG && *entry[i]->d_name != '.' ) {

      snprintf(buf, sizeof(buf), "%s/%s/%s", root, path, entry[i]->d_name);

      if ( lstat(buf, &fileinfo) != 0 ) {
        sprintf(buf, "Can not stat: %d %s",  errno, strerror(errno));
      }
      else {
        sprintf(buf, "%s", fsz2str(fileinfo.st_size));
      }

      fok = http_ssend(client_ctx, "<p><a href=/%s/%s>%s</a> %s</p>\n",
          path, entry[i]->d_name, entry[i]->d_name, buf);

    }

    if ( !fok ) {
      goto end;
    }
  }


  http_ssend(client_ctx,
        "</body>\r\n"
      "</html>\r\n");

end:

  if ( entry ) {
    for ( i = 0; i < n; ++i ) {
      free(entry[i]);
    }
    free(entry);
  }
}

bool http_get_directory_listing(struct http_request_handler ** pqh,
    struct http_client_ctx * client_ctx, const char * root, const char * path)
{
  send_directory_contents(root, path, client_ctx);
  *pqh = NULL;
  return false;
}
