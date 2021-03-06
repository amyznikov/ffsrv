/*
 * ffdb.c
 *
 *  Created on: May 25, 2016
 *      Author: amyznikov
 */

#define _GNU_SOURCE

#include "ffdb.h"
#include "ffcfg.h"
#include "strfuncs.h"
#include <stdio.h>
#include <limits.h>
#include <malloc.h>
#include <string.h>
#include <errno.h>
#include <magic.h>
#include <sys/stat.h>

#include "debug.h"


enum ffobjtype str2objtype(const char * stype)
{
  if ( strcmp(stype, "input") == 0 ) {
    return ffobjtype_input;
  }

  if ( strcmp(stype, "enc") == 0 ) {
    return ffobjtype_encoder;
  }

  if ( strcmp(stype, "mix") == 0 ) {
    return ffobjtype_mixer;
  }

  if ( strcmp(stype, "segments") == 0 ) {
    return ffobjtype_segments;
  }

  return ffobjtype_unknown;
}

const char * objtype2str(enum ffobjtype type)
{
  switch ( type ) {
    case ffobjtype_input :
      return "input";
    case ffobjtype_mixer :
      return "mix";
    case ffobjtype_decoder :
      return "dec";
    case ffobjtype_encoder :
      return "enc";
    break;
    case ffobjtype_segments :
      return "segments";
    break;
    default :
      break;
  }

  return "unknown";
}




// GET user/path
bool ffurl_magic(const char * urlpath, char ** abspath, enum ffmagic * magic, char ** mime)
{
  struct stat st;
  static __thread magic_t mc = NULL;

  FILE * fp = NULL;
  const char * ext = NULL;
  const char * cmime = NULL;
  char buf[64] = "";


  bool fok = false;

  static const struct {
    const char * name;
    enum ffmagic type;
  } objtype_map[] = {
    {"input", ffmagic_input},
    {"enc", ffmagic_enc},
    {"segments", ffmagic_segments},
  };

  static const char * magic_files[] = {
    "/etc/ffsrv/magic.mgc",
    NULL,
    "/usr/share/misc/magic.mgc",
    "/usr/share/file/misc/magic.mgc",
  };

  bool magic_load_ok = false;





  * magic = ffmagic_unknown;
  * abspath = NULL;
  * mime = NULL;


  while ( *urlpath == '/' ) {
    ++urlpath;
  }

  if ( !(*abspath = strmkpath("%s/%s", ffsrv.db.root, urlpath)) ) {
    PDBG("strmkpath() fails: %s", strerror(errno));
    goto end;
  }

  if ( stat(*abspath, &st) != 0 ) {
    PDBG("stat(%s) fails: %s", *abspath, strerror(errno));
    goto end;
  }

  if ( S_ISDIR(st.st_mode) ) {
    * magic = ffmagic_directory;
    * mime = strdup("inode/directory");
    fok = true;
    goto end;
  }

  if ( !S_ISREG(st.st_mode) ) {
    PDBG("%s is not a regular file", *abspath);
    goto end;
  }

  * magic = ffmagic_file;



  /*
   * Special case for some specific media types incorrectly identified by magic
   * */
  if ( (ext = strrchr(*abspath, '.')) ) {

    if ( !(cmime = csmap_get(&ffsrv.magic.mime, ext)) ) {

      static const struct {
        const char * ext;
        const char * mime;
      } special_mimes[] = {
        // http://helixproducts.real.com/hmdp/documentation/helixserver/1512_LR/html/Content/HelixHelp/DASH_File_Names.htm
        { ".m3u8", "application/x-mpeg"   },
        { ".ts",   "video/MP2T"           },
        { ".mpd",  "application/dash+xml" },
        { ".m4s",  "video/mp4"            },
      };

      for ( uint i = 0; i < sizeof(special_mimes) / sizeof(special_mimes[0]); ++i ) {
        if ( strcmp(ext, special_mimes[i].ext) == 0 ) {
          cmime = special_mimes[i].mime;
          break;
        }
      }
    }

    if ( cmime ) {
      *mime = strdup(cmime);
      fok = true;
      goto end;
    }
  }


  if ( !mc ) {

    if ( !(mc = magic_open(MAGIC_SYMLINK | MAGIC_MIME | MAGIC_NO_CHECK_TAR)) ) {
      PDBG("magic_open() fails: %s", strerror(magic_errno(mc)));
      goto end;
    }

    if ( ffsrv.magic.mgc && *ffsrv.magic.mgc ) {
      magic_load_ok = (magic_load(mc, ffsrv.magic.mgc) == 0);
    }

    if ( !magic_load_ok ) {
      for ( size_t i = 0; i < sizeof(magic_files) / sizeof(magic_files[0]); ++i ) {
        if ( (magic_load_ok = (magic_load(mc, magic_files[i]) == 0)) ) {
          PDBG("magic_load() OK using  file=%s", magic_files[i]);
          break;
        }
      }
    }

    if ( !magic_load_ok ) {
      PDBG("magic_load() fails: %s %s", strerror(errno), strerror(magic_errno(mc)));
    }
  }

  if ( !mc || !(*mime = (char*) magic_file(mc, *abspath)) ) {
    PDBG("magic_file() fails: mc=%p %s", mc, mc ? strerror(magic_errno(mc)) : "");
    goto end;
  }

  *mime = strdup(*mime);
  fok = true;

  if ( strncmp(*mime, "text/plain", 10) != 0 ) {
    goto end;
  }

  if ( !(fp = fopen(*abspath, "r")) ) {
    PDBG("fopen('%s') fails: %s", *abspath, strerror(errno));
    goto end;
  }

  if ( fscanf(fp, "%63s",buf) != 1 ) {
    PDBG("fscanf('%s') fails: %s", *abspath, strerror(errno));
    goto end;
  }

  for ( size_t i = 0; i < sizeof(objtype_map) / sizeof(objtype_map[0]); ++i ) {
    if ( strcmp(objtype_map[i].name, buf) == 0 ) {
      *magic = objtype_map[i].type;
      break;
    }
  }

end:

  if ( fp ) {
    fclose(fp);
  }

//  if ( mc ) {
//    magic_close(mc);
//  }

//  PDBG("%s: %s", *abspath, *mime);

  return fok;
}





static bool get_param(const char * line, char key[128], char value[512])
{
  memset(key, 0, 128);
  memset(value, 0, 512);
  return sscanf(line, " %127[A-Za-z1-9_:-.] = %511[^#\n]", key, value) >= 1 && *key != '#';
}

static bool str2bool(const char * s, bool * v)
{
  static const char * yes[] = { "y", "yes", "true", "enable", "1" };
  static const char * no[] = { "n", "no", "false", "disable", "0" };

  for ( uint i = 0; i < sizeof(yes) / sizeof(yes[0]); ++i ) {
    if ( strcasecmp(s, yes[i]) == 0 ) {
      *v = true;
      return true;
    }
  }

  for ( uint i = 0; i < sizeof(no) / sizeof(no[0]); ++i ) {
    if ( strcasecmp(s, no[i]) == 0 ) {
      *v = false;
      return true;
    }
  }

  return false;
}

static char * convert_path(const char * curpath, const char * target)
{
  char * rp = NULL;
  char * temp = NULL;
  size_t len;

  if ( !target || !*target ) {
    return NULL;
  }

  if ( looks_like_url(target) ) {
    return strdup(target);
  }

  // fixme: enusre it will never point to outside of ffsrv.db.root
  if ( *target == '/' ) {
    rp = strmkpath("%s", target);
  }
  else {
    rp = strmkpath("%s/%s", curpath, target);
  }

  if ( rp ) {
    if ( strncmp(rp, ffsrv.db.root, len = strlen(ffsrv.db.root)) == 0 ) {
      for ( temp = rp + len; *temp == '/'; ) {
        ++temp;
      }
      temp = strdup(temp);
    }

    free(rp), rp = temp;
  }

  return rp;
}


static void mkdirname(char filename[])
{
  char * p = filename + strlen(filename);
  while ( p > filename && * p != '/' ) {
    --p;
  }
  *p = 0;
}

static int fgetline(char s[], uint n, FILE * fp)
{
  char * p = s;
  char * e = s + n - 1;
  int c = 0;

  while ( p < e && (c = fgetc(fp)) != EOF ) {

    *p++ = c;

    if ( c == '\n' ) {
      if ( p > s + 1 && *(p - 2) == '\\' ) {
        p -= 2;
      }
      else {
        break;
      }
    }
  }

  if ( p < e ) {
    *p = 0;
  }

  return p > s ? p - s : c == EOF ? EOF : 0;
}

bool ffdb_load_object_params(const char * urlpath, enum ffobjtype * objtype, ffobjparams * params)
{
  char * curpath = NULL;
  char line[1024] = "";

  char stype[64] = "";
  char key[128] = "";
  char value[512] = "";

  FILE * fp = NULL;

  bool fok = false;

  memset(params, 0, sizeof(*params));

  *objtype = ffobjtype_unknown;

  if ( !(curpath = strmkpath("%s/%s", ffsrv.db.root, urlpath)) ) {
    PDBG("strmkpath() fails: %s", strerror(errno));
    goto end;
  }

  if ( !(fp = fopen(curpath, "r")) ) {
    PDBG("fopen('%s') fails: %s", curpath, strerror(errno));
    goto end;
  }

  if ( !fgets(line, sizeof(line), fp) ) {
    PDBG("fgets('%s') fails: %s", curpath, strerror(errno));
    goto end;
  }

  if ( sscanf(line, "%63s", stype) != 1 || (*objtype = str2objtype(stype)) == ffobjtype_unknown ) {
    PDBG("invalid magic '%s' in '%s'", stype, curpath);
    errno = EINVAL;
    goto end;
  }

  mkdirname(curpath);
  fok = true;

  while ( fgetline(line, sizeof(line), fp) != EOF ) {

    if ( !get_param(line, key, value) ) {
      continue;
    }

    switch ( *objtype ) {

      case ffobjtype_input : {
        if ( strcmp(key, "source") == 0 ) {
          free(params->input.source);
          params->input.source = *value ? strdup(value) : NULL;
        }
        else if ( strcmp(key, "sink") == 0 ) {
          free(params->input.sink);
          params->input.sink = convert_path(curpath, value);
        }
        else if ( strcmp(key, "opts") == 0 ) {
          free(params->input.opts);
          params->input.opts = *value ? strdup(value) : NULL;
        }
        else if ( strcmp(key, "decopts") == 0 ) {
          free(params->input.decopts);
          params->input.decopts = *value ? strdup(value) : NULL;
        }
        else if ( strcmp(key, "re") == 0 ) {
          sscanf(value, "%d", &params->input.re);
        }
        else if ( strcmp(key, "genpts") == 0 ) {
          str2bool(value, &params->input.genpts);
        }
        else if ( strcmp(key, "itmo") == 0 ) {
          sscanf(value, "%d", &params->input.itmo);
        }
        else if ( strcmp(key, "rtmo") == 0 ) {
          sscanf(value, "%d", &params->input.rtmo);
        }
        else {
          // unknown property
        }
      }
      break;

      case ffobjtype_encoder : {
        if ( strcmp(key, "source") == 0 ) {
          free(params->encoder.source);
          params->encoder.source = convert_path(curpath, value);
        }
        else if ( strcmp(key, "opts") == 0 ) {
          free(params->encoder.opts);
          params->encoder.opts = *value ? strdup(value) : NULL;
        }
        else {
          // unknown property
        }
      }
      break;

      case ffobjtype_segments : {
        if ( strcmp(key, "source") == 0 ) {
          free(params->segments.source);
          params->segments.source = convert_path(curpath, value);
        }
        else if ( strcmp(key, "opts") == 0 ) {
          free(params->segments.opts);
          params->segments.opts = *value ? strdup(value) : NULL;
        }
        else if ( strcmp(key, "manifest") == 0 ) {
          char * p = value;
          while ( *p == '/' ) {
            ++p;
          }
          free(params->segments.manifest);
          params->segments.manifest = *p ? strdup(p) : NULL;
        }
        else if ( strcmp(key, "itmo") == 0 ) {
          sscanf(value, "%d", &params->segments.itmo);
        }
        else if ( strcmp(key, "rtmo") == 0 ) {
          sscanf(value, "%d", &params->segments.rtmo);
        }
        else {
          // unknown property
        }
      }
      break;
      default :
        break;
    }
  }

end:

  if ( fp ) {
    fclose(fp);
  }

  if ( curpath ) {
    free(curpath);
  }

  if ( !fok ) {
    ffdb_cleanup_object_params(*objtype, params);
  }

  return fok;
}



void ffdb_cleanup_object_params(enum ffobjtype objtype, ffobjparams * params)
{
  switch (objtype) {
    case ffobjtype_input:
      free(params->input.source);
      free(params->input.opts);
      free(params->input.decopts);
    break;
    case ffobjtype_encoder:
      free(params->encoder.source);
      free(params->encoder.opts);
    break;
    case ffobjtype_mixer:
      for ( size_t i = 0; i < params->mixer.nb_sources; ++i ) {
        free(params->mixer.sources[i]);
      }
      free(params->mixer.sources);
      free(params->mixer.smap);
    break;
    case ffobjtype_segments:
      free(params->segments.source);
      free(params->segments.opts);
      free(params->segments.manifest);
      break;
    default:
    break;
  }
  memset(params, 0, sizeof(*params));
}
