/*
 * rtsp-port.c
 *
 *  Created on: Apr 2, 2016
 *      Author: amyznikov
 */
#define _GNU_SOURCE

#include "ffms.h"
#include "libffms.h"
#include "rtsp-parser.h"
#include "sockopt.h"
#include "ipaddrs.h"
#include "url-parser.h"
#include <time.h>
#include "debug.h"


#define RTSP_RXBUF_SIZE (4*1024)
#define RTSP_CLIENT_STACK_SIZE (RTSP_RXBUF_SIZE + 1024*1024)

struct rtsp_server_ctx {
  int so;
};




struct rtsp_client_ctx {
  struct ffinput * input;
  struct ffoutput * output;
  struct cosocket * tcp;
  struct rtsp_parser rstp;
  uint64_t rtsp_session_id;
  int so;
  int status;
};




////////////////////////////////////////////////////////////////////////////////////////////////////////

static struct rtsp_server_ctx * create_rtsp_server_ctx(uint32_t address, uint16_t port);
static void destroy_rtsp_server_ctx(struct rtsp_server_ctx * server_ctx);
static void on_rtsp_server_error(struct rtsp_server_ctx * server_ctx);
static void on_rtsp_server_accept(struct rtsp_server_ctx * server_ctx, int so);
static int rtsp_server_io_callback(void * cookie, uint32_t epoll_events);



static struct rtsp_client_ctx * create_rtsp_client_ctx(int so);
static void destroy_rtsp_client_ctx(struct rtsp_client_ctx * client_ctx);
static void rtsp_client_thread(void * arg);

static bool rtsp_send_responce(struct rtsp_client_ctx * client_ctx, enum rtsp_status status, const char * cseq,
    const char * format, ...);

static bool rtsp_send_responce_v(struct rtsp_client_ctx * client_ctx, enum rtsp_status status, const char * cseq,
    const char * format, va_list arglist);


static bool on_rtsp_options(void * cookie, const struct rtsp_parser_callback_args * args);
static bool on_rtsp_describe(void * cookie, const struct rtsp_parser_callback_args * args);
static bool on_rtsp_play(void * cookie, const struct rtsp_parser_callback_args * args);
static bool on_rtsp_pause(void * cookie, const struct rtsp_parser_callback_args * args);
static bool on_rtsp_record(void * cookie, const struct rtsp_parser_callback_args * args);
static bool on_rtsp_redirect(void * cookie, const struct rtsp_parser_callback_args * args);
static bool on_rtsp_setup(void * cookie, const struct rtsp_parser_callback_args * args);
static bool on_rtsp_announce(void * cookie, const struct rtsp_parser_callback_args * args);
static bool on_rtsp_get_parameter(void * cookie, const struct rtsp_parser_callback_args * args);
static bool on_rtsp_set_parameter(void * cookie, const struct rtsp_parser_callback_args * args);
static bool on_rtsp_teardown(void * cookie, const struct rtsp_parser_callback_args * args);


static int rtsp_send_pkt(void * cookie, int stream_index, uint8_t * buf, int buf_size);

////////////////////////////////////////////////////////////////////////////////////////////////////////

static struct rtsp_server_ctx * create_rtsp_server_ctx(uint32_t address, uint16_t port)
{
  struct rtsp_server_ctx * server_ctx = NULL;
  int so = -1;

  bool fok = false;

  if ( (so = so_tcp_listen(address, port)) == -1 ) {
    PDBG("so_tcp_listen() fails: %s", strerror(errno));
    goto end;
  }

  so_set_noblock(so, true);

  if ( !(server_ctx = calloc(1, sizeof(*server_ctx))) ) {
    PDBG("calloc(server_ctx) fails: %s", strerror(errno));
    goto end;
  }

  server_ctx->so = so;

  if ( !(fok = ffms_schedule_io(so, rtsp_server_io_callback, server_ctx, EPOLLIN, 32 * 1024)) ) {
    PDBG("ffms_schedule_io(rtsp_server_io_callback) fails: %s", strerror(errno));
    goto end;
  }

end: ;

  if ( !fok ) {

    if ( so != -1 ) {
      close(so);
    }

    if ( server_ctx ) {
      free(server_ctx);
      server_ctx = NULL;
    }
  }

  return server_ctx;
}

static void destroy_rtsp_server_ctx(struct rtsp_server_ctx * server_ctx)
{
  if ( server_ctx ) {
    int so = server_ctx->so;
    if ( so != -1 ) {
      close(so);
    }
    free(server_ctx);
    PDBG("[so=%d] DESTROYED", so);
  }
}

static void on_rtsp_server_error(struct rtsp_server_ctx * server_ctx)
{
  PDBG("[so=%d] SERVER ERROR", server_ctx->so);
  destroy_rtsp_server_ctx(server_ctx);
}

static void on_rtsp_server_accept(struct rtsp_server_ctx * server_ctx, int so)
{
  (void)(server_ctx);
  if ( !create_rtsp_client_ctx(so) ) {
    so_close_connection(so, 1);
  }
}

static int rtsp_server_io_callback(void * cookie, uint32_t epoll_events)
{
  struct rtsp_server_ctx * server_ctx = cookie;
  int so;

  int status = 0;

  if ( epoll_events & EPOLLERR ) {
    status = -1;
  }
  else if ( epoll_events & EPOLLIN ) {

    struct sockaddr_in addrs;
    socklen_t addrslen = sizeof(addrs);

    while ( (so = accept(server_ctx->so, (struct sockaddr*) &addrs, &addrslen)) != -1 ) {
      char ss[64] = "";
      PDBG("ACCEPTED SO=%d from %s", so, saddr2str(&addrs, ss));

      PDBG("C on_rtsp_server_accept(server_ctx, so)");
      on_rtsp_server_accept(server_ctx, so);
      PDBG("R on_rtsp_server_accept(server_ctx, so)");

      // co_yield();
    }

    if ( errno != EAGAIN ) {
      PDBG("ACCEPT(so=%d) FAILS: %s", server_ctx->so, strerror(errno));
      status = -1;
    }
  }
  else {
    status = -1;
  }

  if ( status ) {
    on_rtsp_server_error(server_ctx);
  }

  return status;
}



////////////////////////////////////////////////////////////////////////////////////////////////////////

static struct rtsp_client_ctx * create_rtsp_client_ctx(int so)
{
  struct rtsp_client_ctx * client_ctx = NULL;
  bool fok = false;

  static const struct rtsp_parser_callback rtsp_callback = {
    .on_options = on_rtsp_options,
    .on_describe = on_rtsp_describe,
    .on_play  = on_rtsp_play,
    .on_pause = on_rtsp_pause,
    .on_record = on_rtsp_record,
    .on_redirect = on_rtsp_redirect,
    .on_setup = on_rtsp_setup,
    .on_announce = on_rtsp_announce,
    .on_get_parameter = on_rtsp_get_parameter,
    .on_set_parameter = on_rtsp_set_parameter,
    .on_teardown = on_rtsp_teardown,
  };



  PDBG("enter");

  if ( !(client_ctx = calloc(1, sizeof(*client_ctx))) ) {
    PDBG("calloc(client_ctx) fails: %s", strerror(errno));
    goto end;
  }

  so_set_noblock(client_ctx->so = so, true);

  if ( so_set_recvbuf(so, 64 * 1024) != 0 ) {
    PDBG("[so=%d] so_set_recvbuf() fails: %s", so, strerror(errno));
    goto end;
  }

  if ( so_set_sendbuf(so, 4 * 1024) != 0 ) {
    PDBG("[so=%d] so_set_sendbuf() fails: %s", so, strerror(errno));
    goto end;
  }

  if ( !(client_ctx->tcp = cosocket_create(so)) ) {
    PDBG("[so=%d] create_cosock() fails: %s", so, strerror(errno));
    goto end;
  }

  rtsp_parser_init(&client_ctx->rstp, &rtsp_callback, client_ctx);


  PDBG("C ffms_start_cothread");

  if ( !(fok = ffms_start_cothread(rtsp_client_thread, client_ctx, RTSP_CLIENT_STACK_SIZE)) ) {
    PDBG("ffms_schedule_io(rtsp_client_thread) fails: %s", strerror(errno));
    goto end;
  }

  PDBG("R ffms_start_cothread");

end: ;

  if ( !fok && client_ctx ) {
    cosocket_delete(&client_ctx->tcp);
    free(client_ctx);
    client_ctx = NULL;
  }

  PDBG("leave");

  return client_ctx;
}

static void destroy_rtsp_client_ctx(struct rtsp_client_ctx * client_ctx)
{
  if ( client_ctx ) {

    int so = client_ctx->so;

    if ( client_ctx->output ) {
      ff_delete_output_stream(client_ctx->output);
    }

    if ( so != -1 ) {
      so_close_connection(so, 0);
      cosocket_delete(&client_ctx->tcp);
    }

    free(client_ctx);

    PDBG("[so=%d] DESTROYED", so);
  }
}

static void rtsp_client_thread(void * arg)
{
  struct rtsp_client_ctx * client_ctx = arg;
  char rx[RTSP_RXBUF_SIZE];
  ssize_t size;

  PDBG("[so=%d] STARTED", client_ctx->so);

  while ( (size = cosocket_recv(client_ctx->tcp, rx, sizeof(rx) - 1, 0)) > 0 ) {

    rx[size] = 0;
    PDBG("\nS <- C:\n%s\n", rx);

    if ( !rtsp_parser_execute(&client_ctx->rstp, rx, size) ) {
      PDBG("rtsp_parser_execute() fails");
      break;
    }
  }

  PDBG("[so=%d] FINISHED", client_ctx->so);

  destroy_rtsp_client_ctx(client_ctx);
}


static void rtsp_output_thread(void * arg)
{
  struct rtsp_client_ctx * client_ctx = arg;
  int so = client_ctx->so;

  PDBG("[so=%d] STARTED", so);
  ff_run_output_stream(client_ctx->output);
  PDBG("[so=%d] FINISHED", so);
}


static int rtsp_send_pkt(void * cookie, int stream_index, uint8_t * buf, int buf_size)
{
  struct rtsp_client_ctx * client_ctx = cookie;

  struct pkt {
    uint8_t m;
    uint8_t idx;
    uint16_t buf_size;
    uint8_t buf[buf_size];
  } pkt;

  size_t pktsize;
  ssize_t sent;

  pkt.m = '$';
  pkt.idx = stream_index * 2 + 1;
  pkt.buf_size = htons(buf_size);
  memcpy(pkt.buf, buf, buf_size);


  pktsize = offsetof(struct pkt, buf) + buf_size;
  sent = cosocket_send(client_ctx->tcp, &pkt, pktsize, 0);

  // PDBG("st=%d buf_size=%d sent=%zd", stream_index, buf_size, sent);

  return sent > 0 ? 0 : AVERROR(errno);
}


static bool rtsp_send_responce_v(struct rtsp_client_ctx * client_ctx, enum rtsp_status status, const char * cseq,
    const char * format, va_list arglist)
{
  char * resp = NULL;
  char * msg = NULL;
  char date[32];
  time_t t;

  int n;

  t = time(NULL);
  strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S", gmtime(&t));

  if ( format && *format ) {
    vasprintf(&msg, format, arglist);
  }

  n = asprintf(&resp,
      "RTSP/1.0 %d %s\r\n"
      "CSeq: %s\r\n"
      "Date: %s GMT\r\n"
      "%s",
      status,
      rtsp_status_string(status),
      cseq,
      date,
      msg ? msg  : "\r\n"
      );

  if ( n > 0 ) {

    PDBG("----\nS -> C:\n%s\n----\n", resp);

    n = cosocket_send(client_ctx->tcp, resp, n, 0);
  }

  free(msg);
  free(resp);

  return n > 0;
}

static bool rtsp_send_responce(struct rtsp_client_ctx * client_ctx, enum rtsp_status status, const char * cseq,
    const char * format, ...)
{
  bool fok;

  if ( !format || !*format ) {
    fok = rtsp_send_responce_v(client_ctx, status, cseq, NULL, NULL);
  }
  else {
    va_list arglist;
    va_start(arglist, format);
    fok = rtsp_send_responce_v(client_ctx, status, cseq, format, arglist);
    va_end(arglist);
  }

  return fok;
}

static bool rtsp_send_error(struct rtsp_client_ctx * client_ctx, enum rtsp_status status, const char * cseq)
{
  return rtsp_send_responce(client_ctx, status, cseq, NULL);
}


static enum rtsp_status get_rtsp_status(int ff_status)
{
  enum rtsp_status rtsp_status;

  switch ( ff_status ) {
    case AVERROR(ENOENT):
      rtsp_status = RTSP_STATUS_NOT_FOUND;
    break;
    case AVERROR(EPERM):
      rtsp_status = RTSP_STATUS_FORBIDDEN;
    break;
    default :
      rtsp_status = RTSP_STATUS_INTERNAL;
    break;
  }

  return rtsp_status;
}


static bool on_rtsp_options(void * cookie, const struct rtsp_parser_callback_args * c)
{
  struct rtsp_client_ctx * client_ctx = cookie;

  rtsp_send_responce(client_ctx,
      RTSP_STATUS_OK,
      c->cseq,
      "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN\r\n"
      "\r\n");

  return true;
}



static bool on_rtsp_describe(void * cookie, const struct rtsp_parser_callback_args * c)
{
  struct rtsp_client_ctx * client_ctx = cookie;
  int ff_status;
  char sdp[2048] = "";
  int sdp_length;

  char name[256] = "";
  char * output_name;

  parse_url(c->url, NULL, 0, NULL, 0, NULL, 0, NULL, name, sizeof(name));

  if ( *(output_name = name) == '/' ) {
    ++output_name;
  }

  PDBG("C ff_create_output_stream()");
  ff_status = ff_create_output_stream(&client_ctx->output,
      output_name,
      &(struct ff_start_output_args ) {
            .format = "rtp",
            .cookie = client_ctx,
            .onsendpkt = rtsp_send_pkt
          });

  PDBG("R ff_create_output_stream(): status=%s", av_err2str(ff_status));

  if ( ff_status == 0 ) {

    PDBG("C ff_get_output_sdp()");

    ff_status = ff_get_output_sdp(client_ctx->output, sdp,
        sizeof(sdp) - 1);

    PDBG("R ff_get_output_sdp(): %s", av_err2str(ff_status));
  }

  if ( ff_status ) {

    rtsp_send_error(client_ctx,
        get_rtsp_status(ff_status),
        c->cseq);
  }
  else {

    sdp_length = strlen(sdp);

    rtsp_send_responce(client_ctx,
        RTSP_STATUS_OK,
        c->cseq,
        "Content-Base: %s\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        c->url,
        sdp_length,
        sdp);
  }

  return true;
}

static bool on_rtsp_setup(void * cookie, const struct rtsp_parser_callback_args * c)
{
  struct rtsp_client_ctx * client_ctx = cookie;
  const char * s;
  int stream_id = 0;

  struct rtsp_transport_params * tp = NULL;
  int ntp;
  int selected_transport_index = -1;

  PDBG("transport: %s", c->transport);

  ntp = rtsp_parse_transport_string(c->transport, &tp);
  if ( ntp < 1 ) {
    rtsp_send_error(client_ctx,
        RTSP_STATUS_BAD_REQUEST,
        c->cseq);
    goto end;
  }

  for ( int i = 0; i < ntp; ++i ) {
    PDBG("\nTRANSPORT[%d]:\n"
    "proto=%s\n"
    "profile=%s\n"
    "lower_transport=%s\n"
    "ccast=%s\n"
    "destination=%s\n"
    "interleaved=%s\n"
    "append=%s\n"
    "ttl=%s\n"
    "layers=%s\n"
    "port=%s\n"
    "client_port=%s\n"
    "server_port=%s\n"
    "ssrc=%s\n"
    "mode=%s\n",
    i,
    tp[i].proto,
    tp[i].profile,
    tp[i].lower_transport,
    tp[i].ccast,
    tp[i].destination,
    tp[i].interleaved,
    tp[i].append,
    tp[i].ttl,
    tp[i].layers,
    tp[i].port,
    tp[i].client_port,
    tp[i].server_port,
    tp[i].ssrc,
    tp[i].mode
    );

    if ( strcasecmp(tp[i].lower_transport, "TCP") == 0 ) {
      selected_transport_index = i;
    }
  }

  if ( selected_transport_index < 0 ) {
    rtsp_send_error(client_ctx,
        RTSP_STATUS_TRANSPORT,
        c->cseq);
    goto end;
  }


  if ( !(s = strstr(c->url, "/streamid=")) ) {
    rtsp_send_error(client_ctx,
        RTSP_STATUS_BAD_REQUEST,
        c->cseq);
    goto end;
  }



  stream_id = atoi(s + 10);
  client_ctx->rtsp_session_id = (((uint64_t) rand()) << 32) | (((uint64_t) rand()));

  rtsp_send_responce(client_ctx,
      RTSP_STATUS_OK,
      c->cseq,
      "Transport: RTP/AVP/TCP;interleaved=%d-%d\r\n"
          "Session: 0x%.16llX\r\n"
          "\r\n",
      stream_id * 2,
      stream_id * 2 + 1,
      client_ctx->rtsp_session_id);

end:

  rtsp_free_transport_params(tp, ntp);

  return true;
}

static bool on_rtsp_play(void * cookie, const struct rtsp_parser_callback_args * c)
{
  struct rtsp_client_ctx * client_ctx = cookie;
  bool fok;

  if ( !(fok = ffms_start_cothread(rtsp_output_thread, client_ctx, 128 * 1024)) ) {
    rtsp_send_error(client_ctx,
        AVERROR(errno),
        c->cseq);
  }
  else {
    rtsp_send_responce(client_ctx,
        RTSP_STATUS_OK,
        c->cseq,
        "Session: 0x%.16llX\r\n"
        "\r\n",
        client_ctx->rtsp_session_id);
  }

  return true;
}

static bool on_rtsp_pause(void * cookie, const struct rtsp_parser_callback_args * args)
{
  return 0;
}

static bool on_rtsp_record(void * cookie, const struct rtsp_parser_callback_args * args)
{
  return 0;
}

static bool on_rtsp_redirect(void * cookie, const struct rtsp_parser_callback_args * args)
{
  return 0;
}


static bool on_rtsp_announce(void * cookie, const struct rtsp_parser_callback_args * args)
{
  return 0;
}

static bool on_rtsp_get_parameter(void * cookie, const struct rtsp_parser_callback_args * args)
{
  return 0;
}

static bool on_rtsp_set_parameter(void * cookie, const struct rtsp_parser_callback_args * args)
{
  return 0;
}

static bool on_rtsp_teardown(void * cookie, const struct rtsp_parser_callback_args * args)
{
  return 0;
}



////////////////////////////////////////////////////////////////////////////////////////////////////////

bool ffms_add_rtsp_port(uint32_t addrs, uint16_t port)
{
  return create_rtsp_server_ctx(addrs, port) != NULL;
}

