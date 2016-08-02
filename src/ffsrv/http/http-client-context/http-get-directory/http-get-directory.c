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



static bool send_directory_contents(const char * root, const char * path, struct http_client_ctx * client_ctx)
{
  struct dirent ** entry = NULL;
  struct stat fileinfo;
  char buf[PATH_MAX];
  char * base;

  int i, n = 0;

  bool fok = true;

  sprintf(buf, "%s/%s", root, path);

  if ( (n = scandir(buf, &entry, NULL, alphasort)) < 0 ) {
    fok = http_send_404_not_found(client_ctx);
    goto end;
  }

  if ( !(fok = http_send_200_OK_ncl(client_ctx, "text/html; charset=utf-8", NULL)) ) {
    goto end;
  }

  if ( !(fok = http_ssend(client_ctx, "<html>\n<body>\n")) ) {
    goto end;
  }

  for ( i = 0; i < n; ++i ) {

    if ( strendswith(entry[i]->d_name, "~") ) {
      continue;
    }

    if ( entry[i]->d_type == DT_DIR ) {

      if ( strcmp(entry[i]->d_name, ".") == 0 ) {
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

    else if ( entry[i]->d_type == DT_REG && entry[i]->d_name[0] != '.' ) {

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

  fok = http_ssend(client_ctx,"</body>\n</html>\n");

end:

  if ( entry ) {
    for ( i = 0; i < n; ++i ) {
      free(entry[i]);
    }
    free(entry);
  }

  return fok;
}

bool http_get_directory_listing(struct http_request_handler ** pqh,
    struct http_client_ctx * client_ctx, const char * root, const char * path)
{
  *pqh = NULL;
  send_directory_contents(root, path, client_ctx);
  return false; // fime: content-length is not known, i force need close connection now;
}
