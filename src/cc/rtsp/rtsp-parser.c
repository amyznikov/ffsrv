/*
 * rtsp-parser.c
 *
 *  Created on: Apr 19, 2016
 *      Author: amyznikov
 */

#define _GNU_SOURCE

#include <string.h>
#include <ctype.h>
#include <malloc.h>
#include <errno.h>
#include <sys/types.h>
#include "rtsp-parser.h"
#include "debug.h"

#define MAX_REQUEST_LINES   128


struct pmap {
  const char * param;
  const char ** value;
};

static const char * streol(const char * s, size_t size)
{
  while ( size-- && *s && *s != '\n' ) {
    ++s;
  }
  return s;
}

static inline char * strndcat(char * s1, const char * s2, size_t length)
{
  char * s;
  if ( !s1 ) {
    s = strndup(s2, length);
  }
  else {
    size_t l1 = strlen(s1);
    strncpy((s = realloc(s1, l1 + length + 1)) + l1, s2, length);
  }
  return s;
}


#define SSTRCMP(x,y)\
  strncmp(x,y, sizeof(y)-1)

static inline char * SSDUP(const char * s) {
  while (*s && isspace(*s) ) {
    ++s;
  }
  return strdup(s);
}


static inline char ** alloc_string_array(size_t size)
{
  return calloc(size, sizeof(char*));
}

static inline void free_string_array(char ** array, size_t size)
{
  for ( size_t i = 0; i < size; ++i ) {
    free(array[i]);
  }
  free(array);
}

static inline void free_strings(char ** array, size_t size)
{
  for ( size_t i = 0; i < size; ++i ) {
    if ( array[i] ) {
      free(array[i]);
      array[i] = NULL;
    }
  }
}


static void parse_header(struct rtsp_parser * p, const struct pmap map[] )
{
  char * line;
  char * param;
  char * value;

  for ( size_t i = 1, n = p->nb_lines; i < n; ++i ) {

    if ( !(line = p->lines[i]) ) {
      break;
    }

    if ( !(param = strtok(line, " :")) ) {
      break;
    }

    if ( !(value = strtok(NULL, " \r\n")) ) {
      break;
    }

    for ( uint j = 0; map[j].param; ++j ) {
      if ( strcasecmp(param, map[j].param) == 0 ) {
        *map[j].value = value;
        break;
      }
    }
  }
}


static bool on_rtsp_parse_options(struct rtsp_parser * p, const char * url, const char * proto)
{
  struct rtsp_parser_callback_args c = {
    .url = url,
    .proto = proto
  };

  bool fok = false;

  parse_header(p, (struct pmap[] ) {
        { "CSeq", &c.cseq },
        { "User-Agent", &c.user_agent },
        { NULL }
      });

  if ( c.cseq ) {
    fok = p->cb->on_options(p->cookie, &c);
  }

  return fok;
}

static bool on_rtsp_parse_describe(struct rtsp_parser * p, const char * url, const char * proto)
{
  struct rtsp_parser_callback_args c = {
    .url = url,
    .proto = proto
  };

  bool fok = false;

  parse_header(p, (struct pmap[] ) {
        { "CSeq", &c.cseq },
        { "User-Agent", &c.user_agent },
        { "Accept", &c.accept},
        { NULL }
      });

  if ( c.cseq ) {
    fok = p->cb->on_describe(p->cookie, &c);
  }

  return fok;
}

static bool on_rtsp_parse_setup(struct rtsp_parser * p, const char * url, const char * proto)
{
  struct rtsp_parser_callback_args c = {
    .url = url,
    .proto = proto
  };

  bool fok = false;

  parse_header(p, (struct pmap[] ) {
        { "CSeq", &c.cseq },
        { "User-Agent", &c.user_agent },
        { "Transport", &c.transport },
        { NULL }
      });

  if ( c.cseq ) {
    fok = p->cb->on_setup(p->cookie, &c);
  }

  return fok;
}

static bool on_rtsp_parse_play(struct rtsp_parser * p, const char * url, const char * proto)
{
  struct rtsp_parser_callback_args c = {
    .url = url,
    .proto = proto
  };

  bool fok = false;

  parse_header(p, (struct pmap[] ) {
        { "CSeq", &c.cseq             },
        { "User-Agent", &c.user_agent },
        { "Session", &c.session       },
        { "Range", &c.range           },
        { NULL                        }
      });

  if ( c.cseq ) {
    fok = p->cb->on_play(p->cookie, &c);
  }

  return fok;
}


static bool on_rtsp_parse_announce(struct rtsp_parser * p, const char * url, const char * proto)
{
  struct rtsp_parser_callback_args c = {
    .url = url,
    .proto = proto
  };

  bool fok = false;

  parse_header(p, (struct pmap[] ) {
        { "CSeq", &c.cseq },
        { "User-Agent", &c.user_agent },
        { NULL }
      });

  if ( c.cseq ) {
    fok = p->cb->on_announce(p->cookie, &c);
  }

  return fok;
}



static bool on_rtsp_parse_pause(struct rtsp_parser * p, const char * url, const char * proto)
{
  struct rtsp_parser_callback_args c = {
    .url = url,
    .proto = proto
  };

  bool fok = false;

  parse_header(p, (struct pmap[] ) {
        { "CSeq", &c.cseq },
        { "User-Agent", &c.user_agent },
        { NULL }
      });

  if ( c.cseq ) {
    fok = p->cb->on_pause(p->cookie, &c);
  }

  return fok;
}


static bool on_rtsp_parse_record(struct rtsp_parser * p, const char * url, const char * proto)
{
  struct rtsp_parser_callback_args c = {
    .url = url,
    .proto = proto
  };

  bool fok = false;

  parse_header(p, (struct pmap[] ) {
        { "CSeq", &c.cseq },
        { "User-Agent", &c.user_agent },
        { NULL }
      });

  if ( c.cseq ) {
    fok = p->cb->on_record(p->cookie, &c);
  }

  return fok;
}

static bool on_rtsp_parse_redirect(struct rtsp_parser * p, const char * url, const char * proto)
{
  struct rtsp_parser_callback_args c = {
    .url = url,
    .proto = proto
  };

  bool fok = false;

  parse_header(p, (struct pmap[] ) {
        { "CSeq", &c.cseq },
        { "User-Agent", &c.user_agent },
        { NULL }
      });

  if ( c.cseq ) {
    fok = p->cb->on_redirect(p->cookie, &c);
  }

  return fok;
}


static bool on_rtsp_parse_get_parameter(struct rtsp_parser * p, const char * url, const char * proto)
{
  struct rtsp_parser_callback_args c = {
    .url = url,
    .proto = proto
  };

  bool fok = false;

  parse_header(p, (struct pmap[] ) {
        { "CSeq", &c.cseq },
        { "User-Agent", &c.user_agent },
        { NULL }
      });

  if ( c.cseq ) {
    fok = p->cb->on_get_parameter(p->cookie, &c);
  }

  return fok;
}


static bool on_rtsp_parse_set_parameter(struct rtsp_parser * p, const char * url, const char * proto)
{
  struct rtsp_parser_callback_args c = {
    .url = url,
    .proto = proto
  };

  bool fok = false;

  parse_header(p, (struct pmap[]  ) {
        { "CSeq", &c.cseq },
        { "User-Agent", &c.user_agent },
        { NULL }
      });

  if ( c.cseq ) {
    fok = p->cb->on_set_parameter(p->cookie, &c);
  }

  return fok;
}

static bool on_rtsp_parse_teardown(struct rtsp_parser * p, const char * url, const char * proto)
{
  struct rtsp_parser_callback_args c = {
    .url = url,
    .proto = proto
  };

  bool fok = false;

  parse_header(p, (struct pmap[] ) {
        { "CSeq", &c.cseq },
        { "User-Agent", &c.user_agent },
        { NULL }
      });

  if ( c.cseq ) {
    fok = p->cb->on_teardown(p->cookie, &c);
  }

  return fok;
}


static bool rtsp_parse_request(struct rtsp_parser * p)
{
  char * line;
  char * command = NULL;
  char * url = NULL;
  char * proto = NULL;
  const char delims[] = " \t\r\n";

  bool fok = false;

  static const struct {
    const char * command;
    bool (*parse)(struct rtsp_parser * p, const char * url, const char * proto);
  } commands[] = {
    { "OPTIONS", on_rtsp_parse_options},
    { "DESCRIBE", on_rtsp_parse_describe},
    { "SETUP", on_rtsp_parse_setup},
    { "PLAY", on_rtsp_parse_play},
    { "PAUSE", on_rtsp_parse_pause},
    { "RECORD", on_rtsp_parse_record},
    { "REDIRECT", on_rtsp_parse_redirect},
    { "GET_PARAMETER", on_rtsp_parse_get_parameter},
    { "SET_PARAMETER", on_rtsp_parse_set_parameter},
    { "TEARDOWN", on_rtsp_parse_teardown},
    { "ANNOUNCE", on_rtsp_parse_announce },
  };

  if ( p->nb_lines < 1 || !(line = p->lines[0]) ) {
    goto end;
  }

  if ( !(command = strtok(line, delims)) ) {
    goto end;
  }

  if ( !(url = strtok(NULL, delims) )) {
    goto end;
  }

  if ( !(proto = strtok(NULL, delims) )) {
    goto end;
  }

  for ( size_t i = 0; i < sizeof(commands) / sizeof(command[0]); ++i ) {
    if ( strcasecmp(command, commands[i].command) == 0 ) {
      fok = commands[i].parse(p, url, proto);
      break;
    }
  }

end:

  free_strings(p->lines, p->nb_lines);
  p->nb_lines = 0;

  return fok;
}


bool rtsp_parser_init(struct rtsp_parser * p, const struct rtsp_parser_callback * cb, void * cookie)
{
  memset(p, 0, sizeof(*p));
  p->cb = cb;
  p->cookie = cookie;
  p->lines = alloc_string_array(MAX_REQUEST_LINES);
  return true;
}


void rtsp_parser_cleanup(struct rtsp_parser * p)
{
  free_string_array(p->lines, MAX_REQUEST_LINES);
  free(p->current_line);
}

bool rtsp_parser_execute(struct rtsp_parser * p, const char * data, size_t size)
{
  const char * eol;
  bool eor = false;

  while ( size ) {

    if ( p->nb_lines >= MAX_REQUEST_LINES ) {
      errno = EMSGSIZE;
      return false;
    }

    eol = streol(data, size);
    p->current_line = strndcat(p->current_line, data, eol - data + 1);
    if ( *eol != '\n' ) {
      break;
    }

    eor = (strcmp(p->current_line, "\r\n") == 0 || strcmp(p->current_line, "\n") == 0);

    p->lines[p->nb_lines++] = p->current_line;
    p->current_line = NULL;

    size -= ((eol - data) + 1);
    data = eol +1;

    if ( eor && !rtsp_parse_request(p) ) {
      PDBG("rtsp_parse_request(p) fails");
      return false;
    }
  }

  return true;
}


static void rtsp_cleanup_transport_params(struct rtsp_transport_params * t)
{
  free(t->proto);
  free(t->profile);
  free(t->lower_transport);
  free(t->ccast);
  free(t->destination);
  free(t->interleaved);
  free(t->append);
  free(t->ttl);
  free(t->layers);
  free(t->port);
  free(t->client_port);
  free(t->server_port);
  free(t->ssrc);
  free(t->mode);
}


// https://www.ietf.org/rfc/rfc2326.txt [Page 58]
int rtsp_parse_transport_string(const char * s, struct rtsp_transport_params ** t)
{
  char * buf = strdup(s);
  char * pb = buf, * ts, * ps;
  struct rtsp_transport_params p;

  int n = 0;

  memset(&p, 0, sizeof(p));

  // Transports are comma separated, listed in order of preference.
  while ( (ts = strsep(&pb, ",")) ) {  //  ts = 'protocol/profile[/lower-transport];parameters'

    char * pp = strsep(&ts, ";"); //  pp = 'protocol/profile[/lower-transport]'
                                  //  ts = 'parameters'

    if ( !pp || !(ps = strsep(&pp, "/")) || !(p.proto = strdup(ps)) ) {
      break;
    }

    if ( !(ps = strsep(&pp, "/")) || !(p.profile = strdup(ps)) ) {
      break;
    }

    if ( !(ps = strsep(&pp, ";")) ) {
      if ( !(p.lower_transport = strdup("UDP")) ) {
        break;
      }
    }
    else if ( !(p.lower_transport = strdup(ps)) ) {
      break;
    }


    //  parameter  =  ( "unicast" | "multicast" ); parameter;
    while ( (ps = strsep(&ts, ";")) ) {

      if ( SSTRCMP(ps, "unicast") == 0 || SSTRCMP(ps, "multicast") == 0 ) {
        p.ccast = SSDUP(ps);
      }
      else if ( SSTRCMP(ps, "destination") == 0 ) {
        if ( (ps = strpbrk(ps, "=")) ) {
          p.destination = SSDUP(ps + 1);
        }
      }
      else if ( SSTRCMP(ps, "interleaved") == 0 ) {
        if ( (ps = strpbrk(ps, "=")) ) {
          p.interleaved = SSDUP(ps + 1);
        }
      }
      else if ( SSTRCMP(ps, "append") == 0 ) {
        p.append = SSDUP(ps);
      }
      else if ( SSTRCMP(ps, "ttl") == 0 ) {
        if ( (ps = strpbrk(ps, "=")) ) {
          p.ttl = SSDUP(ps + 1);
        }
      }
      else if ( SSTRCMP(ps, "layers") == 0 ) {
        if ( (ps = strpbrk(ps, "=")) ) {
          p.layers = SSDUP(ps+1);
        }
      }
      else if ( SSTRCMP(ps, "port") ==0 ) {
        if ( (ps = strpbrk(ps, "=")) ) {
          p.port = SSDUP(ps+1);
        }
      }
      else if ( SSTRCMP(ps, "client_port") ==0 ) {
        if ( (ps = strpbrk(ps, "=")) ) {
          p.client_port = SSDUP(ps+1);
        }
      }
      else if ( SSTRCMP(ps, "server_port") == 0 ) {
        if ( (ps = strpbrk(ps, "=")) ) {
          p.server_port = SSDUP(ps+1);
        }
      }
      else if ( SSTRCMP(ps, "ssrc") == 0 ) {
        if ( (ps = strpbrk(ps, "=")) ) {
          p.ssrc = SSDUP(ps+1);
        }
      }
      else if ( SSTRCMP(ps, "mode") == 0 ) {
        if ( (ps = strpbrk(ps, "=")) ) {
          p.mode = SSDUP(ps+1);
        }
      }
    }


    *t = realloc(*t, (n + 1) * sizeof(**t));
    (*t)[n++] = p;

    memset(&p, 0, sizeof(p));
  }

  rtsp_cleanup_transport_params(&p);

  free(buf);
  return n;
}

void rtsp_free_transport_params(struct rtsp_transport_params t[], int count)
{
  if ( t ) {
    for ( int i = 0; i < count; ++i ) {
      rtsp_cleanup_transport_params(&t[i]);
    }
    free(t);
  }
}

const char * rtsp_status_string(enum rtsp_status status)
{
  const char * smsg;

  switch ( status )
  {
    case _RTSP_STATUS_CONTINUE :
      smsg = "Continue";
    break;
    case _RTSP_STATUS_OK :
      smsg = "OK";
    break;
    case _RTSP_STATUS_CREATED :
      smsg = "Created";
    break;
    case _RTSP_STATUS_LOW_ON_STORAGE_SPACE :
      smsg = "Low on Storage Space";
    break;
    case _RTSP_STATUS_MULTIPLE_CHOICES :
      smsg = "Multiple Choices";
    break;
    case _RTSP_STATUS_MOVED_PERMANENTLY :
      smsg = "Moved Permanently";
    break;
    case _RTSP_STATUS_MOVED_TEMPORARILY :
      smsg = "Moved Temporarily";
    break;
    case _RTSP_STATUS_SEE_OTHER :
      smsg = "See Other";
    break;
    case _RTSP_STATUS_NOT_MODIFIED :
      smsg = "Not Modified";
    break;
    case _RTSP_STATUS_USE_PROXY :
      smsg = "Use Proxy";
    break;
    case _RTSP_STATUS_BAD_REQUEST :
      smsg = "Bad Request";
    break;
    case _RTSP_STATUS_UNAUTHORIZED :
      smsg = "Unauthorized";
    break;
    case _RTSP_STATUS_PAYMENT_REQUIRED :
      smsg = "Payment Required";
    break;
    case _RTSP_STATUS_FORBIDDEN :
      smsg = "Forbidden";
    break;
    case _RTSP_STATUS_NOT_FOUND :
      smsg = "Not Found";
    break;
    case _RTSP_STATUS_METHOD :
      smsg = "Method Not Allowed";
    break;
    case _RTSP_STATUS_NOT_ACCEPTABLE :
      smsg = "Not Acceptable";
    break;
    case _RTSP_STATUS_PROXY_AUTH_REQUIRED :
      smsg = "Proxy Authentication Required";
    break;
    case _RTSP_STATUS_REQ_TIME_OUT :
      smsg = "Request Time-out";
    break;
    case _RTSP_STATUS_GONE :
      smsg = "Gone";
    break;
    case _RTSP_STATUS_LENGTH_REQUIRED :
      smsg = "Length Required";
    break;
    case _RTSP_STATUS_PRECONDITION_FAILED :
      smsg = "Precondition Failed";
    break;
    case _RTSP_STATUS_REQ_ENTITY_2LARGE :
      smsg = "Request Entity Too Large";
    break;
    case _RTSP_STATUS_REQ_URI_2LARGE :
      smsg = "Request URI Too Large";
    break;
    case _RTSP_STATUS_UNSUPPORTED_MTYPE :
      smsg = "Unsupported Media Type";
    break;
    case _RTSP_STATUS_PARAM_NOT_UNDERSTOOD :
      smsg = "Parameter Not Understood";
    break;
    case _RTSP_STATUS_CONFERENCE_NOT_FOUND :
      smsg = "Conference Not Found";
    break;
    case _RTSP_STATUS_BANDWIDTH :
      smsg = "Not Enough Bandwidth";
    break;
    case _RTSP_STATUS_SESSION :
      smsg = "Session Not Found";
    break;
    case _RTSP_STATUS_STATE :
      smsg = "Method Not Valid in This State";
    break;
    case _RTSP_STATUS_INVALID_HEADER_FIELD :
      smsg = "Header Field Not Valid for Resource";
    break;
    case _RTSP_STATUS_INVALID_RANGE :
      smsg = "Invalid Range";
    break;
    case _RTSP_STATUS_RONLY_PARAMETER :
      smsg = "Parameter Is Read-Only";
    break;
    case _RTSP_STATUS_AGGREGATE :
      smsg = "Aggregate Operation no Allowed";
    break;
    case _RTSP_STATUS_ONLY_AGGREGATE :
      smsg = "Only Aggregate Operation Allowed";
    break;
    case _RTSP_STATUS_TRANSPORT :
      smsg = "Unsupported Transport";
    break;
    case _RTSP_STATUS_UNREACHABLE :
      smsg = "Destination Unreachable";
    break;
    case _RTSP_STATUS_INTERNAL :
      smsg = "Internal Server Error";
    break;
    case _RTSP_STATUS_NOT_IMPLEMENTED :
      smsg = "Not Implemented";
    break;
    case _RTSP_STATUS_BAD_GATEWAY :
      smsg = "Bad Gateway";
    break;
    case _RTSP_STATUS_SERVICE :
      smsg = "Service Unavailable";
    break;
    case _RTSP_STATUS_GATEWAY_TIME_OUT :
      smsg = "Gateway Time-out";
    break;
    case _RTSP_STATUS_VERSION :
      smsg = "RTSP Version not Supported";
    break;
    case _RTSP_STATUS_UNSUPPORTED_OPTION :
      smsg = "Option not supported";
    break;
    default :
      smsg = "Unknown error";
    break;
  };

  return smsg;
}



