/*
 * rtsp-parser.h
 *
 *  Created on: Apr 19, 2016
 *      Author: amyznikov
 */


#pragma once
#ifndef __ffms_rtsp_parser_h__
#define __ffms_rtsp_parser_h__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


struct rtsp_parser;

enum rtsp_status {
    RTSP_STATUS_CONTINUE =100,
    RTSP_STATUS_OK =200,
    RTSP_STATUS_CREATED =201,
    RTSP_STATUS_LOW_ON_STORAGE_SPACE =250,
    RTSP_STATUS_MULTIPLE_CHOICES =300,
    RTSP_STATUS_MOVED_PERMANENTLY =301,
    RTSP_STATUS_MOVED_TEMPORARILY =302,
    RTSP_STATUS_SEE_OTHER =303,
    RTSP_STATUS_NOT_MODIFIED =304,
    RTSP_STATUS_USE_PROXY =305,
    RTSP_STATUS_BAD_REQUEST =400,
    RTSP_STATUS_UNAUTHORIZED =401,
    RTSP_STATUS_PAYMENT_REQUIRED =402,
    RTSP_STATUS_FORBIDDEN =403,
    RTSP_STATUS_NOT_FOUND =404,
    RTSP_STATUS_METHOD =405,
    RTSP_STATUS_NOT_ACCEPTABLE =406,
    RTSP_STATUS_PROXY_AUTH_REQUIRED =407,
    RTSP_STATUS_REQ_TIME_OUT =408,
    RTSP_STATUS_GONE =410,
    RTSP_STATUS_LENGTH_REQUIRED =411,
    RTSP_STATUS_PRECONDITION_FAILED =412,
    RTSP_STATUS_REQ_ENTITY_2LARGE =413,
    RTSP_STATUS_REQ_URI_2LARGE =414,
    RTSP_STATUS_UNSUPPORTED_MTYPE =415,
    RTSP_STATUS_PARAM_NOT_UNDERSTOOD =451,
    RTSP_STATUS_CONFERENCE_NOT_FOUND =452,
    RTSP_STATUS_BANDWIDTH =453,
    RTSP_STATUS_SESSION =454,
    RTSP_STATUS_STATE =455,
    RTSP_STATUS_INVALID_HEADER_FIELD =456,
    RTSP_STATUS_INVALID_RANGE =457,
    RTSP_STATUS_RONLY_PARAMETER =458,
    RTSP_STATUS_AGGREGATE =459,
    RTSP_STATUS_ONLY_AGGREGATE =460,
    RTSP_STATUS_TRANSPORT =461,
    RTSP_STATUS_UNREACHABLE =462,
    RTSP_STATUS_INTERNAL =500,
    RTSP_STATUS_NOT_IMPLEMENTED =501,
    RTSP_STATUS_BAD_GATEWAY =502,
    RTSP_STATUS_SERVICE =503,
    RTSP_STATUS_GATEWAY_TIME_OUT =504,
    RTSP_STATUS_VERSION =505,
    RTSP_STATUS_UNSUPPORTED_OPTION =551,
};


struct rtsp_parser_callback_args {
  const char * proto;
  const char * url;
  const char * cseq;
  const char * user_agent;
  const char * accept;
  const char * transport;
  const char * session;
  const char * range;
};

struct rtsp_parser_callback {
  bool (*on_options)(void * cookie, const struct rtsp_parser_callback_args * args);
  bool (*on_describe)(void * cookie, const struct rtsp_parser_callback_args * args);
  bool (*on_setup)(void * cookie, const struct rtsp_parser_callback_args * args);
  bool (*on_play)(void * cookie, const struct rtsp_parser_callback_args * args);
  bool (*on_pause)(void * cookie, const struct rtsp_parser_callback_args * args);
  bool (*on_record)(void * cookie, const struct rtsp_parser_callback_args * args);
  bool (*on_redirect)(void * cookie, const struct rtsp_parser_callback_args * args);
  bool (*on_announce)(void * cookie, const struct rtsp_parser_callback_args * args);
  bool (*on_get_parameter)(void * cookie, const struct rtsp_parser_callback_args * args);
  bool (*on_set_parameter)(void * cookie, const struct rtsp_parser_callback_args * args);
  bool (*on_teardown)(void * cookie, const struct rtsp_parser_callback_args * args);
};

struct rtsp_parser {
  const struct rtsp_parser_callback * cb;
  void * cookie;
  char * current_line;
  char ** lines;
  size_t nb_lines;
};


bool rtsp_parser_init(struct rtsp_parser * p, const struct rtsp_parser_callback * cb, void * cookie);
bool rtsp_parser_execute(struct rtsp_parser * p, const char * data, size_t size);
void rtsp_parser_cleanup(struct rtsp_parser * p);
const char * rtsp_status_string(enum rtsp_status status);


/** https://www.ietf.org/rfc/rfc2326.txt
 * [Page 58] */
struct rtsp_transport_params {
  char * proto;                 // "RTP"
  char * profile;               // "AVP"
  char * lower_transport;       // "TCP" | "UDP"
  char * ccast;                 // ( "unicast" | "multicast" )
  char * destination;           // "destination" [ "=" address ]
  char * interleaved;           // "interleaved" "=" channel [ "-" channel ]
  char * append;                // "append"
  char * ttl;                   // "ttl" "=" ttl
  char * layers;                // "layers" "=" 1*DIGIT
  char * port;                  // "port" "=" port [ "-" port ]
  char * client_port;           // "client_port" "=" port [ "-" port ]
  char * server_port;           // "server_port" "=" port [ "-" port ]
  char * ssrc;                  // "ssrc" "=" ssrc
  char * mode;                  // "mode" = <"> 1\#mode <">
};

int rtsp_parse_transport_string(const char * s, struct rtsp_transport_params ** t);
void rtsp_free_transport_params(struct rtsp_transport_params t[], int count);


#ifdef __cplusplus
}
#endif

#endif /* __ffms_rtsp_parser_h__ */
