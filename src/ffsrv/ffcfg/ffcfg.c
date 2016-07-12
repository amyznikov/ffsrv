/*
 * ffsrv-conf.c
 *
 *  Created on: May 29, 2016
 *      Author: amyznikov
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <pwd.h>
#include <libavutil/log.h>
#include <libavutil/eval.h>
#include "ffcfg.h"
#include "strfuncs.h"
#include "getifaddrs.h"


#define SNCPY(dst,src) \
  strncpy(dst,src, sizeof(dst)-1)

#define SDUP(dst,src) \
    free(dst),dst=strdup(src);

struct ffsrv_config ffsrv = {

  .avloglevel = AV_LOG_WARNING,
  .ncpu = 1,

  .db = {
    .root = NULL
  },

  .http = {
    .rxbuf     = 64 * 1024,
    .txbuf     = 256 * 1024,
    .rcvtmo    = 20, // sec
    .sndtmo    = 20, // sec
  },

  .https = {
    .cert      = NULL,
    .key       = NULL,
    .rxbuf     = 64 * 1024,
    .txbuf     = 256 * 1024,
    .rcvtmo    = 20, // sec
    .sndtmo    = 20, // sec
  },

  .rtsp = {
    .rxbuf     = 64 * 1024,
    .txbuf     = 256 * 1024,
    .rcvtmo    = 20, // sec
    .sndtmo    = 20, // sec
  },

  .keepalive = {
    .enable    = true,
    .idle      = 5,
    .intvl     = 3,
    .probes    = 5,
  },

};


//AV_LOG_DEBUG
static int str2avll(const char * str)
{
  int ll = AV_LOG_WARNING;

  static const struct {
    const char * s;
    int ll;
  } avlls[] = {
    {"quiet", AV_LOG_QUIET},
    {"panic", AV_LOG_PANIC},
    {"fatal", AV_LOG_FATAL},
    {"error", AV_LOG_ERROR},
    {"warning", AV_LOG_WARNING},
    {"info", AV_LOG_INFO},
    {"verbose", AV_LOG_VERBOSE},
    {"debug", AV_LOG_DEBUG},
    {"trace", AV_LOG_TRACE},
  };

  if ( str ) {
    for ( uint i = 0; i < sizeof(avlls)/sizeof(avlls[0]); ++i ) {
      if ( strcasecmp(avlls[i].s, str) == 0 ) {
        ll = avlls[i].ll;
        break;
      }
    }
  }

  return ll;
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

static bool str2size(const char * s, size_t * size)
{
  char * suffix = NULL;
  double x;
  bool fok = true;

  if ( (x = strtod(s, &suffix)) < 0 ) {
    return false;
  }

  while ( *suffix && isspace(*suffix) ) {
    ++suffix;
  }

  if ( *suffix ) {

    static const struct {
      const char * suffix;
      size_t m;
    } s[] = {
      { "B", 1 },
      { "K", 1024 },
      { "KB", 1024 },
      { "M", 1024 * 1024 },
      { "MB", 1024 * 1024 },
      { "G", 1024ULL * 1024 * 1024 },
      { "GB", 1024ULL * 1024 * 1024 },
      { "T", 1024ULL * 1024ULL * 1024 * 1024 },
      { "TB", 1024ULL * 1024ULL * 1024 * 1024 },
    };

    fok = false;

    for ( size_t i = 0; i < sizeof(s) / sizeof(s[0]); ++i ) {
      if ( strcasecmp(suffix, s[i].suffix) == 0 ) {
        x *= s[i].m;
        fok = true;
        break;
      }
    }
  }

  if ( fok ) {
    *size = (size_t) (x);
  }

  return fok;
}



static void ffconfig_init(void)
{
  static bool ffconfig_initialized = false;

  if ( !ffconfig_initialized ) {

    ccarray_init(&ffsrv.http.faces, 32, sizeof(struct sockaddr_in));
    ccarray_init(&ffsrv.https.faces, 32, sizeof(struct sockaddr_in));
    ccarray_init(&ffsrv.rtsp.faces, 32, sizeof(struct sockaddr_in));

    ffsrv.db.root = strdup("/home/ffdb");


    ffconfig_initialized = true;
  }

}

static bool parse_listen_faces(char str[], ccarray_t * faces)
{
  static const char delims[] = " \t\n,;";
  char * tok;

  tok = strtok(str, delims);
  while ( tok && ccarray_size(faces) < ccarray_capacity(faces) ) {

    uint32_t address = 0;
    uint16_t port = 0;

    if ( getifaddr(tok, &address, &port) == -1 ) {
      fprintf(stderr, "FATAL: Can't get address for '%s': %s\n", tok, strerror(errno));
      fprintf(stderr, "Check if device name is valid and device is up\n");
      return false;
    }

    if ( !port ) {
      fprintf(stderr, "No port specified in '%s'\n", tok);
      return false;
    }

    ccarray_push_back(faces, &(struct sockaddr_in ) {
          .sin_family = AF_INET,
          .sin_addr.s_addr = htonl(address),
          .sin_port = htons(port),
          .sin_zero = { 0 }
        });

    tok = strtok(NULL, delims);
  }

  return true;
}


bool ffsrv_parse_option(char * keyname, char * keyvalue)
{
  size_t i;

  static const char * ignore[] =
    { "help", "-help", "--help", "version", "--version", "--config", "--no-daemon" };

  for ( i = 0; i < sizeof(ignore) / sizeof(ignore[0]); ++i ) {
    if ( strcmp(keyname, ignore[i]) == 0 ) {
      return true;
    }
  }


  ffconfig_init();

  ///////////
  if ( strcmp(keyname, "logfile") == 0 ) {
    SDUP(ffsrv.logfilename, keyvalue);
  }

  ///////////
  else if ( strcmp(keyname, "loglevel") == 0 ) {
    ffsrv.avloglevel = str2avll(keyvalue);
  }

  ///////////
  else if ( strcmp(keyname, "ncpu") == 0 ) {
    if ( *keyvalue && sscanf(keyvalue, "%d", &ffsrv.ncpu) != 1 ) {
      fprintf(stderr, "FATAL: Invalid key value: %s=%s\n", keyname, keyvalue);
      return false;
    }
  }

  ///////////
  else if ( strcmp(keyname, "tcp.keepalive") == 0 ) {
    if ( *keyvalue && !str2bool(keyvalue, &ffsrv.keepalive.enable) ) {
      fprintf(stderr, "FATAL: Invalid key value: %s=%s\n", keyname, keyvalue);
      return false;
    }
  }
  else if ( strcmp(keyname, "tcp.keepalive.time") == 0 ) {
    if ( *keyvalue && sscanf(keyvalue,"%d", &ffsrv.keepalive.idle) != 1 ) {
      fprintf(stderr, "FATAL: Invalid key value: %s=%s\n", keyname, keyvalue);
      return false;
    }
  }
  else if ( strcmp(keyname, "tcp.keepalive.intvl") == 0 ) {
    if ( *keyvalue && sscanf(keyvalue,"%d", &ffsrv.keepalive.intvl) != 1 ) {
      fprintf(stderr, "FATAL: Invalid key value: %s=%s\n", keyname, keyvalue);
      return false;
    }
  }
  else if ( strcmp(keyname, "tcp.keepalive.probes") == 0 ) {
    if ( *keyvalue && sscanf(keyvalue,"%d", &ffsrv.keepalive.probes) != 1 ) {
      fprintf(stderr, "FATAL: Invalid key value: %s=%s\n", keyname, keyvalue);
      return false;
    }
  }

  ///////////
  else if ( strcmp(keyname, "http.listen") == 0 ) {
    if( !parse_listen_faces(keyvalue, &ffsrv.http.faces) ) {
      return false;
    }
  }
  else if ( strcmp(keyname, "http.rxbuf") == 0 ) {
    if ( *keyvalue && !str2size(keyvalue, &ffsrv.http.rxbuf) ) {
      fprintf(stderr, "FATAL: Invalid key value: %s=%s\n", keyname, keyvalue);
      return false;
    }
  }
  else if ( strcmp(keyname, "http.txbuf") == 0 ) {
    if ( *keyvalue && !str2size(keyvalue, &ffsrv.http.txbuf) ) {
      fprintf(stderr, "FATAL: Invalid key value: %s=%s\n", keyname, keyvalue);
      return false;
    }
  }
  else if ( strcmp(keyname, "http.rcvtmo") == 0 ) {
    if ( *keyvalue && sscanf(keyvalue, "%d", &ffsrv.http.rcvtmo) != 1 ) {
      fprintf(stderr, "FATAL: Invalid key value: %s=%s\n", keyname, keyvalue);
      return false;
    }
  }
  else if ( strcmp(keyname, "http.sndtmo") == 0 ) {
    if ( *keyvalue && sscanf(keyvalue, "%d", &ffsrv.http.sndtmo) != 1 ) {
      fprintf(stderr, "FATAL: Invalid key value: %s=%s\n", keyname, keyvalue);
      return false;
    }
  }

  ///////////
  else if ( strcmp(keyname, "https.listen") == 0 ) {
    if( !parse_listen_faces(keyvalue, &ffsrv.https.faces) ) {
      return false;
    }
  }
  else if ( strcmp(keyname, "https.rxbuf") == 0 ) {
    if ( *keyvalue && !str2size(keyvalue, &ffsrv.https.rxbuf) ) {
      fprintf(stderr, "FATAL: Invalid key value: %s=%s\n", keyname, keyvalue);
      return false;
    }
  }
  else if ( strcmp(keyname, "https.txbuf") == 0 ) {
    if ( *keyvalue && !str2size(keyvalue, &ffsrv.https.txbuf) ) {
      fprintf(stderr, "FATAL: Invalid key value: %s=%s\n", keyname, keyvalue);
      return false;
    }
  }
  else if ( strcmp(keyname, "https.rcvtmo") == 0 ) {
    if ( *keyvalue && sscanf(keyvalue, "%d", &ffsrv.https.rcvtmo) != 1 ) {
      fprintf(stderr, "FATAL: Invalid key value: %s=%s\n", keyname, keyvalue);
      return false;
    }
  }
  else if ( strcmp(keyname, "https.sndtmo") == 0 ) {
    if ( *keyvalue && sscanf(keyvalue, "%d", &ffsrv.https.sndtmo) != 1 ) {
      fprintf(stderr, "FATAL: Invalid key value: %s=%s\n", keyname, keyvalue);
      return false;
    }
  }
  else if ( strcmp(keyname, "https.cert") == 0 ) {
    if ( *keyvalue ) {
      SDUP(ffsrv.https.cert, keyvalue);
    }
  }
  else if ( strcmp(keyname, "https.key") == 0 ) {
    if ( *keyvalue ) {
      SDUP(ffsrv.https.key, keyvalue);
    }
  }
  else if ( strcmp(keyname, "https.ciphers") == 0 ) {
    if ( *keyvalue ) {
      SDUP(ffsrv.https.ciphers, keyvalue);
    }
  }

  ///////////
  else if ( strcmp(keyname, "rtsp.listen") == 0 ) {
    if( !parse_listen_faces(keyvalue, &ffsrv.rtsp.faces) ) {
      return false;
    }
  }
  else if ( strcmp(keyname, "rtsp.rxbuf") == 0 ) {
    if ( *keyvalue && !str2size(keyvalue, &ffsrv.rtsp.rxbuf) ) {
      fprintf(stderr, "FATAL: Invalid key value: %s=%s\n", keyname, keyvalue);
      return false;
    }
  }
  else if ( strcmp(keyname, "rtsp.txbuf") == 0 ) {
    if ( *keyvalue && !str2size(keyvalue, &ffsrv.rtsp.txbuf) ) {
      fprintf(stderr, "FATAL: Invalid key value: %s=%s\n", keyname, keyvalue);
      return false;
    }
  }
  else if ( strcmp(keyname, "rtsp.rcvtmo") == 0 ) {
    if ( *keyvalue && sscanf(keyvalue, "%d", &ffsrv.rtsp.rcvtmo) != 1 ) {
      fprintf(stderr, "FATAL: Invalid key value: %s=%s\n", keyname, keyvalue);
      return false;
    }
  }
  else if ( strcmp(keyname, "rtsp.sndtmo") == 0 ) {
    if ( *keyvalue && sscanf(keyvalue, "%d", &ffsrv.rtsp.sndtmo) != 1 ) {
      fprintf(stderr, "FATAL: Invalid key value: %s=%s\n", keyname, keyvalue);
      return false;
    }
  }


  ///////////
  else if ( strcmp(keyname, "db.root") == 0 ) {
    SDUP(ffsrv.db.root, keyvalue);
    strtrim(ffsrv.db.root, "/");
  }

  ///////////
  else {
    fprintf(stderr, "FATAL:Invalid or unknown parameter %s:%s\n", keyname, keyvalue);
    return false;
  }

  return true;
}


bool ffsrv_read_config_file(const char * fname)
{
  FILE * fp = NULL;
  char line[1024] = "";
  int line_index = 0;
  bool fok = 0;

  if ( !(fp = fopen(fname, "r")) ) {
    fprintf(stderr, "FATAL: Can't open '%s': %s\n", fname, strerror(errno));
    goto end;
  }

  fok = 1;

  while ( fgets(line, sizeof(line), fp) ) {

    char keyname[256] = "", keyvalue[256] = "";

    ++line_index;

    if ( sscanf(line, " %255[A-Za-z1-9_:-.] = %255[^#\n]", keyname, keyvalue) >= 1 && *keyname != '#' ) {
      if ( !(fok = ffsrv_parse_option(keyname, keyvalue)) ) {
        fprintf(stderr, "Error in config file '%s' at line %d: '%s'\n", fname, line_index, line);
        break;
      }
    }
  }

end : ;

  if ( fp ) {
    fclose(fp);
  }

  return fok;
}

const char * ffsrv_find_config_file(void)
{
  struct passwd * pw;
  uid_t uid;

  static char config_file_name[PATH_MAX];

  if ( *config_file_name ) {
    goto end;
  }

  snprintf(config_file_name, sizeof(config_file_name) - 1, "./ffsrv.cfg");
  if ( access(config_file_name, F_OK) == 0 ) {
    goto end;
  }

  if ( (uid = geteuid()) != 0 && (pw = getpwuid(uid)) != NULL ) {
    snprintf(config_file_name, sizeof(config_file_name) - 1, "%s/.config/ffsrv/ffsrv.cfg", pw->pw_dir);
    if ( access(config_file_name, F_OK) == 0 ) {
      goto end;
    }
  }

  snprintf(config_file_name, sizeof(config_file_name) - 1, "/var/lib/ffsrv/ffsrv.cfg");
  if ( access(config_file_name, F_OK) == 0 ) {
    goto end;
  }

  snprintf(config_file_name, sizeof(config_file_name) - 1, "/usr/local/etc/ffsrv/ffsrv.cfg");
  if ( access(config_file_name, F_OK) == 0 ) {
    goto end;
  }

  snprintf(config_file_name, sizeof(config_file_name) - 1, "/etc/ffsrv/ffsrv.cfg");
  if ( access(config_file_name, F_OK) == 0 ) {
    goto end;
  }

  *config_file_name = 0;

end: ;

  return *config_file_name ? config_file_name : NULL;
}
