/*
 * ffdb-txtfile.c
 *
 *  Created on: May 25, 2016
 *      Author: amyznikov
 *
 *
 * <input cam1
 *   source = rtsp://cam1.sis.lan
 *   ctxopts = "-rtsp_transport tcp"
 *   idle_timeout = 5s
 *   re = -1,
 *   genpts = true
 *   decopts = -maxthreads 1
 * />
 *
 * <input cam2
 *   source = rtsp://cam2.sis.lan
 *   opts = "-rtsp_transport tcp"
 *   idle_timeout = 5s
 *   re = -1,
 *   genpts = true
 * />
 *
 * <mix webcam
 *   source = cam1 cam2
 *   smap = 0:1 1:0
 * />
 *
 *
 * <output cam2/640x480
 *   source = cam2
 *   opts = -c:v libx264 -c:a aac -g 16 -crf 32 -s 640x480
 *   format = matroska
 * />
 *
 * <output cam2/800x600
 *  source = cam2
 *  opts = -c:v libx264 -c:a aac -g 16 -crf 32 -s 800x600
 *  format = asf
 * />
 *
 * <alias
 *   webcam/640x480.asf  webcam/640x480?fmt=asf
 *   webcam/800x600.mkv  webcam/800x600?fmt=matroska
 * />
 *
 *
 */

#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <malloc.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include "ffcfg.h"
#include "ffdb-txtfile.h"
#include "debug.h"



bool ffdb_txtfile_init(void)
{
  char default_filename[PATH_MAX] = "";
  struct passwd * pw;
  uid_t uid;
  bool fok = false;

  if ( ffsrv.db.txtfile.name && *ffsrv.db.txtfile.name ) {
    return access(ffsrv.db.txtfile.name, F_OK) == 0;
  }

  if ( !fok ) {
    snprintf(default_filename, sizeof(default_filename) - 1, "./ffdb.dat");
    fok = access(default_filename, F_OK) == 0;
  }

  if ( (uid = geteuid()) != 0 && (pw = getpwuid(uid)) != NULL ) {
    snprintf(default_filename, sizeof(default_filename) - 1, "%s/.config/ffsrv/ffdb.dat", pw->pw_dir);
    fok = access(default_filename, F_OK) == 0;
  }

  if ( !fok ) {
    snprintf(default_filename, sizeof(default_filename) - 1, "/var/lib/ffsrv/ffdb.dat");
    fok = access(default_filename, F_OK) == 0;
  }

  if ( !fok ) {
    snprintf(default_filename, sizeof(default_filename) - 1, "/etc/ffdb.dat");
    fok = access(default_filename, F_OK) == 0;
  }

  if ( !fok ) {
    PDBG("Can not access ffdb.dat");
  }
  else {
    free(ffsrv.db.txtfile.name);
    ffsrv.db.txtfile.name = strdup(default_filename);
  }

  return fok;
}



static int sstrncmp(const char * s1, const char * s2, size_t n)
{
  while ( isspace(*s1) ) {
    ++s1;
  }

  while ( isspace(*s2) ) {
    ++s2;
  }

  return strncmp(s1, s2, n);
}


static bool get_param(const char * line, char key[128], char value[512])
{
  memset(key, 0, 128);
  memset(value, 0, 512);
  return sscanf(line, " %127[A-Za-z1-9_:-.] = %511[^#\n]", key, value) >= 1 && *key != '#';
}



bool ffdb_txtfile_find_object(const char * name, enum ffobject_type * __type, ffobj_params * params)
{
  FILE * fp = NULL;

  char line[1024] = "";

  char stype[64] = "";
  char sname[512] = "";

  char key[128] = "";
  char value[512] = "";

  bool fok = false;
  bool found = false;
  enum ffobject_type type = object_type_unknown;

  memset(params, 0, sizeof(*params));

  if ( !(fp = fopen(ffsrv.db.txtfile.name, "r")) ) {
    PDBG("fopen('%s') fails", ffsrv.db.txtfile.name);
    goto end;
  }

  while ( fgets(line, sizeof(line), fp) ) {
    if ( sscanf(line, " <%63s %511s", stype, sname) == 2 && strcmp(sname, name) == 0 ) {
      found = true;
      PDBG("FOUND CONFING FOR %s %s", stype, sname);
      break;
    }
  }


  if ( !found || (type = str2objtype(stype)) == object_type_unknown ) {
    PDBG("str2objtype(%s): %d", stype, type);
    goto end;
  }

  switch ( type ) {

    case object_type_input : {

      while ( fgets(line, sizeof(line), fp) ) {

        if ( sstrncmp(line, "/>", 2) == 0 ) {
          fok = true;
          break;
        }

        if ( get_param(line, key, value) ) {
          if ( strcmp(key, "source") == 0 ) {
            params->input.source = *value ? strdup(value) : NULL;
          }
          else if ( strcmp(key, "opts") == 0 ) {
            params->input.opts = *value ? strdup(value) : NULL;
          }
          else if ( strcmp(key, "re") == 0 ) {
            sscanf(value, "%d", &params->input.re);
          }
          else if ( strcmp(key, "genpts") == 0 ) {
            sscanf(value, "%d", &params->input.genpts);
          }
          else {
            // unknown property
          }
        }
      }
    }
    break;

    case object_type_encoder : {

      while ( fgets(line, sizeof(line), fp) ) {

        if ( sstrncmp(line, "/>", 2) == 0 ) {
          fok = true;
          break;
        }

        if ( get_param(line, key, value) ) {
          if ( strcmp(key, "source") == 0 ) {
            params->encoder.source = strdup(value);
          }
          else if ( strcmp(key, "opts") == 0 ) {
            params->encoder.opts = strdup(value);
          }
          else {
            // unknown property
          }
        }
      }
    }

    break;

    case object_type_mixer : {

      while ( fgets(line, sizeof(line), fp) ) {

        if ( sstrncmp(line, "/>", 2) == 0 ) {
          fok = true;
          break;
        }

        if ( get_param(line, key, value) ) {
          if ( strcmp(key, "smap") == 0 ) {
            params->mixer.smap = strdup(value);
          }
          else if ( strcmp(key, "sources") == 0 ) {
            char * p1 = value, *p2;
            while ( (p2 = strsep(&p1, " \t,;")) ) {
              params->mixer.sources = realloc(params->mixer.sources, (params->mixer.nb_sources + 1) * sizeof(char*));
              params->mixer.sources[params->mixer.nb_sources++] = strdup(p2);
            }
          }
          else {
            // unknown property
          }
        }
      }
    }

    break;

    default :
      break;
  }


end:

  if ( fp ) {
    fclose(fp);
  }

  if ( found && !fok ) {
    ffdb_cleanup_object_params(type, params);
    type = object_type_unknown;
  }

  if ( type == object_type_unknown ) {
    errno = ENOENT;
  }

  *__type = type;

  return fok;
}


