/*
 * http-get-saved-streams.c
 *
 *  Created on: Jun 7, 2016
 *      Author: amyznikov
 */

#include "http-get-saved-streams.h"
#include "ffcfg.h"
#include "url-parser.h"
#include "debug.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

static void send_directory_contents(const char * path, struct http_client_ctx * client_ctx)
{
  char abspath[PATH_MAX] = "";
  const char * root = NULL;

  struct dirent ** entry = NULL;
  int i, n = 0;

  const struct http_request * q = &client_ctx->req;

  bool fok;

  PDBG("enter");

  if ( !(root = ffsrv.sinks.root) ) {
    http_ssend(client_ctx,
        "%s 404 Not Found\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<html>\r\n"
            "<body>\r\n"
            "<p>Saved streams path not configured</p>\r\n"
            "</body>\r\n"
            "</html>\r\n",
        q->proto);
    goto end;
  }

  if ( !*root ) {
    root = ".";
  }

  snprintf(abspath, sizeof(abspath)-1,"%s/%s", root, path);

  n = scandir(abspath, &entry, NULL, alphasort);
  if ( n < 0 ) {
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
        abspath,
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
    if ( entry[i]->d_type == DT_REG ) {
      if ( !http_ssend(client_ctx, "<p><a href=%s/%s>%s</a></p>", path, entry[i]->d_name, entry[i]->d_name) ) {
        goto end;
      }
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

  PDBG("leave");
  return;
}

bool create_http_get_saved_streams_context(struct http_request_handler ** pqh,
    struct http_client_ctx * client_ctx)
{
  const struct http_request * q = &client_ctx->req;
  const char * url = q->url;
  char path[PATH_MAX] = "";
  char params[256] = "";

  * pqh = NULL;

  while ( *url == '/' ) {
    ++url;
  }

  if ( strncmp(url, "store", 5) != 0 ) {
    goto end;
  }

  url += 5;
  while ( *url == '/' ) {
    ++url;
  }

  split_stream_path(url, path, sizeof(path), params, sizeof(params));
  send_directory_contents(path, client_ctx);

end:

  *pqh = NULL;
  return false;
}
