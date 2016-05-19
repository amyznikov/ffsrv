/*
 * http-port.c
 *
 *  Created on: Apr 2, 2016
 *      Author: amyznikov
 */
#define _GNU_SOURCE

#include <unistd.h>
#include "ffms.h"
#include "ffoutput.h"
#include "http-request.h"
#include "http-responce.h"
#include "sockopt.h"
#include "ipaddrs.h"
#include "debug.h"


#define HTTP_RXBUF_SIZE (4*1024)
#define HTTP_CLIENT_STACK_SIZE (HTTP_RXBUF_SIZE + 1024*1024)


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct http_server_ctx {
  int so;
};


struct http_client_ctx {
  struct ffinput * input;
  struct ffoutput * output;
  struct cosocket * cosock;
  uint8_t * body;
  size_t body_capacity;
  size_t body_size;
  size_t body_pos;
  http_request req;
  int so;
  int status;
};




/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static struct http_server_ctx * create_http_server_ctx(uint32_t address, uint16_t port);
static void destroy_http_server_ctx(struct http_server_ctx * server_ctx);
static void on_http_server_error(struct http_server_ctx * server_ctx);
static void on_http_server_accept(struct http_server_ctx * server_ctx, int so);
static int http_server_io_callback(void * cookie, uint32_t epoll_events);

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


static struct http_client_ctx * create_http_client_ctx(int so);
static void destroy_http_client_ctx(struct http_client_ctx * client_ctx);
static void http_client_thread(void * arg);

static ssize_t http_read(struct http_client_ctx * client_ctx);
static bool http_send_responce(struct http_client_ctx * client_ctx, const char * format, ...);
static bool http_send_responce_v(struct http_client_ctx * client_ctx, const char * format, va_list arglist);


static int  on_http_headers_complete(void * arg);
static int  on_http_message_complete(void * arg);
static int  on_http_message_body(void * arg, const char *at, size_t length);

static int  on_http_sendpkt(void * cookie, int stream_index, uint8_t * buf, int buf_size);
static int  on_http_recvpkt(void * cookie, uint8_t *buf, int buf_size);
static void on_http_input_finished(void * cookie, int status);

static bool on_http_method_get(struct http_client_ctx * client_ctx);
static bool on_http_method_post(struct http_client_ctx * client_ctx);





/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


static struct http_server_ctx * create_http_server_ctx(uint32_t address, uint16_t port)
{
  struct http_server_ctx * server_ctx = NULL;
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
  if ( !(fok = co_schedule_io(so, EPOLLIN, http_server_io_callback, server_ctx, 4 * 1024)) ) {
    PDBG("ffms_schedule_io(http_server_io_callback) fails: %s", strerror(errno));
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


static void destroy_http_server_ctx(struct http_server_ctx * server_ctx)
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

static void on_http_server_error(struct http_server_ctx * server_ctx)
{
  PDBG("[so=%d] SERVER ERROR", server_ctx->so);
  destroy_http_server_ctx(server_ctx);
}

static void on_http_server_accept(struct http_server_ctx * server_ctx, int so)
{
  (void)(server_ctx);
  if ( !create_http_client_ctx(so) ) {
    so_close_connection(so, 1);
  }
}

static int http_server_io_callback(void * cookie, uint32_t epoll_events)
{
  struct http_server_ctx * server_ctx = cookie;
  int so;

  int status = 0;

  if ( epoll_events & EPOLLERR ) {
    status = -1;
  }
  else if ( epoll_events & EPOLLIN ) {

    struct sockaddr_in addrs;
    socklen_t addrslen = sizeof(addrs);

    while ( (so = accept(server_ctx->so, (struct sockaddr*) &addrs, &addrslen)) != -1 ) {
      PDBG("ACCEPTED SO=%d from %s", so, saddr2str(&addrs, NULL));
      on_http_server_accept(server_ctx, so);
      co_yield();
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
    on_http_server_error(server_ctx);
  }

  return status;
}







/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


static struct http_client_ctx * create_http_client_ctx(int so)
{
  struct http_client_ctx * client_ctx = NULL;
  bool fok = false;

  static const struct http_request_callback http_request_cb = {
      .on_headers_complete = on_http_headers_complete,
      .on_body = on_http_message_body,
      .on_message_complete = on_http_message_complete,
  };

  if ( !(client_ctx = calloc(1, sizeof(*client_ctx))) ) {
    PDBG("calloc(client_ctx) fails: %s", strerror(errno));
    goto end;
  }

  so_set_noblock(client_ctx->so = so, true);

  if ( so_set_recvbuf(so, 64 * 1024) != 0 ) {
    PDBG("[so=%d] so_set_recvbuf() fails: %s", so, strerror(errno));
    goto end;
  }

  if ( so_set_sendbuf(so, 256 * 1024) != 0 ) {
    PDBG("[so=%d] so_set_sendbuf() fails: %s", so, strerror(errno));
    goto end;
  }

  so_set_keepalive(so, true, 5, 3, 5);

  if ( !(client_ctx->cosock = cosocket_create(so)) ) {
    PDBG("[so=%d] create_cosock() fails: %s", so, strerror(errno));
    goto end;
  }

  http_request_init(&client_ctx->req, &http_request_cb, client_ctx);

  if ( !(fok = co_schedule(http_client_thread, client_ctx, HTTP_CLIENT_STACK_SIZE)) ) {
    PDBG("ffms_schedule_io(http_client_io_callback) fails: %s", strerror(errno));
    goto end;
  }


end: ;

  if ( !fok && client_ctx ) {
    cosocket_delete(&client_ctx->cosock);
    free(client_ctx);
    client_ctx = NULL;
  }

  return client_ctx;
}

static void destroy_http_client_ctx(struct http_client_ctx * client_ctx)
{
  if ( client_ctx ) {

    int so = client_ctx->so;

    if ( so != -1 ) {
      so_close_connection(so, 0);
      cosocket_delete(&client_ctx->cosock);
    }

    http_request_cleanup(&client_ctx->req);
    ffms_release_input(&client_ctx->input);
    ffms_delete_output(&client_ctx->output);
    free(client_ctx);

    PDBG("[so=%d] DESTROYED", so);
  }
}

static void http_client_thread(void * arg)
{
  struct http_client_ctx * client_ctx = arg;
  ssize_t size;

  //PDBG("[so=%d] STARTED", client_ctx->so);

  while ( (size = http_read(client_ctx)) > 0 ) {
    if ( client_ctx->output || client_ctx->input ) {
      break;
    }
  }

  if ( size >= 0 && strcmp(client_ctx->req.method, "GET") == 0 ) {
    ff_run_output_stream(client_ctx->output);
  }

  if ( !client_ctx->input ) {
    destroy_http_client_ctx(client_ctx);
  }

  PDBG("[so=%d] FINISHED", client_ctx->so);
}


static ssize_t http_read(struct http_client_ctx * client_ctx)
{
  ssize_t size;
  uint8_t rx[HTTP_RXBUF_SIZE];

  if ( (size = cosocket_recv(client_ctx->cosock, rx, sizeof(rx), 0)) >= 0 ) {
    if ( !http_request_parse(&client_ctx->req, rx, size) ) {
      PDBG("[so=%d] http parsing error", client_ctx->so);
      errno = EPROTO, size = -1;
    }
  }

  return size;
}

static bool http_send_responce_v(struct http_client_ctx * client_ctx, const char * format, va_list arglist)
{
  char * msg = NULL;
  int n;

  if ( (n = vasprintf(&msg, format, arglist)) > 0 ) {
    n = cosocket_send(client_ctx->cosock, msg, n, MSG_NOSIGNAL);
  }

  free(msg);
  return n > 0;
}

static bool http_send_responce(struct http_client_ctx * client_ctx, const char * format, ...)
{
  va_list arglist;
  bool fok;

  va_start(arglist, format);
  fok = http_send_responce_v(client_ctx, format, arglist);
  va_end(arglist);

  return fok;
}

static int on_http_headers_complete(void * cookie)
{
  struct http_client_ctx * client_ctx = cookie;
  const struct http_request * q = &client_ctx->req;

  bool fok = false;

  PDBG("[so=%d] '%s %s %s'", client_ctx->so, q->method, q->url, q->proto);

  //  for ( size_t i = 0, n = csmap_size(&q->parms); i < n; ++i ) {
  //    const csmap_entry * e = csmap_item(&q->parms, i);
  //    PDBG("[so=%d] %s : %s", client_ctx->so, e->key, e->value);
  //  }

  if ( strcmp(q->method, "GET") == 0 ) {
    fok = on_http_method_get(client_ctx);
  }
  else if ( strcmp(q->method, "POST") == 0 ) {
    fok = on_http_method_post(client_ctx);
  }
  else {
    http_send_responce(client_ctx,
        "%s 501 Not Implemented\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html>\r\n"
        "<body>\r\n"
        "<h1>This function is not supported</h1>\r\n"
        "</body>\r\n"
        "</html>\r\n",
        q->proto);
  }

  return fok ? 0 : -1;
}

static int on_http_message_complete(void * cookie)
{
  struct http_client_ctx * client_ctx = cookie;
  (void)(client_ctx);
  //PDBG("[so=%d] ON MESSAGE COMPLETE", client_ctx->so);
  return 0;
}


static int on_http_message_body(void * cookie, const char *at, size_t length)
{
  struct http_client_ctx * client_ctx = cookie;

  //PDBG("   [so=%d] BODY E", client_ctx->so);

  if ( !client_ctx->input ) {
    PDBG("[so=%d] BUG: UNEXPECTED ON BODY on NULL input", client_ctx->so);
    exit(1);
    return -1;
  }

  if ( client_ctx->body == NULL ) {
    size_t initial_capacity = HTTP_RXBUF_SIZE;
    client_ctx->body = malloc(client_ctx->body_capacity = initial_capacity);
    PDBG("ALLOCATED TO %zu", client_ctx->body_capacity);
  }

  if ( client_ctx->body != NULL ) {
    size_t capacity_required = (client_ctx->body_size + length);
    if ( capacity_required > client_ctx->body_capacity ) {
      capacity_required = (capacity_required + 1023) & ~1023UL;
      client_ctx->body = realloc(client_ctx->body, capacity_required);
      client_ctx->body_capacity = capacity_required;
      PDBG("REALLOCATED TO %zu", client_ctx->body_capacity);
    }
  }

  memcpy(client_ctx->body + client_ctx->body_size, at, length);
  client_ctx->body_size += length;

  //PDBG("   [so=%d] BODY L", client_ctx->so);
  return 0;
}

static int on_http_sendpkt(void * cookie, int stream_index,  uint8_t * buf, int buf_size)
{
  (void)(stream_index);
  ssize_t size = cosocket_send(((struct http_client_ctx *) cookie)->cosock, buf, buf_size, MSG_NOSIGNAL);
  return size == (ssize_t) (buf_size) ? 0 : AVERROR(errno);
}

static int on_http_recvpkt(void * cookie, uint8_t *buf, int buf_size)
{
  struct http_client_ctx * client_ctx = cookie;
  ssize_t size;
  int copy_size;

  if ( client_ctx->status ) {
    return client_ctx->status;
  }

  while ( !client_ctx->body_size ) {
    if ( (size = http_read(client_ctx)) <= 0 ) {
      return client_ctx->status = (size < 0 ? AVERROR(errno) : AVERROR_EOF);
    }
  }

  size = client_ctx->body_size - client_ctx->body_pos;
  copy_size = buf_size < size ? buf_size : size;

  memcpy(buf, client_ctx->body + client_ctx->body_pos, copy_size);

  if ( (client_ctx->body_pos += copy_size) == client_ctx->body_size ) {
    client_ctx->body_pos = client_ctx->body_size = 0;
  }

  return copy_size;
}


static void on_http_input_finished(void * cookie, int status)
{
  struct http_client_ctx * client_ctx = cookie;
  PDBG("FINISHED: status=%s", av_err2str(status));
  destroy_http_client_ctx(client_ctx);
}


static bool on_http_method_get(struct http_client_ctx * client_ctx)
{
  const http_request * q = &client_ctx->req;

  char name[256] = "";
  char opts[256] = "";

  const char * url = q->url;
  const char * ofmt = NULL;

  const char * mime_type= NULL;

  int status;

  while ( *url == '/' ) {
    ++url;
  }

  sscanf(url,"%255[^?]?%255s", name, opts);

  if ( (ofmt = strstr(opts, "fmt=")) ) {
    ofmt += 4;
  }

  status = ffms_create_output(&client_ctx->output, name,
      &(struct ffms_create_output_args ) {
            .format = ofmt && *ofmt ? ofmt : "matroska",
            .send_pkt = on_http_sendpkt,
            .cookie = client_ctx,
          });

  if ( status ) {

    char * errmsg;

    PDBG("ff_create_output_stream() fails: %s", av_err2str(status));

    switch ( status ) {
      case AVERROR(ENOENT):
        errmsg = "404 NOT FOUND";
      break;
      case AVERROR(EPERM):
        errmsg = "405 Not Allowed";
      break;
      default :
        errmsg = "500 Internal Server Error";
      break;
    }

    http_send_responce(client_ctx,
        "%s %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html>\r\n"
        "<body>\r\n"
        "<h1>ERROR</h1>\r\n"
        "<p>ff_start_output_stream('%s') FAILS</p>\r\n"
        "<p>status=%d (%s)</p>\r\n"
        "</body>\r\n"
        "</html>\r\n",
        q->proto,
        errmsg,
        name,
        status,
        av_err2str(status));

    goto end;
  }

//  PDBG("client_ctx=%p input=%p output=%p", client_ctx, client_ctx->input, client_ctx->output);

  mime_type = ff_get_output_mime_type(client_ctx->output);

  http_send_responce(client_ctx,
      "%s 200 OK\r\n"
      "Content-Type: %s\r\n"
      "Cache-Control: no-cache, no-store, must-revalidate\r\n"
      "Pragma: no-cache\r\n"
      "Expires: 0\r\n"
      "Connection: close\r\n"
      "Server: ffms\r\n"
      "\r\n",
      q->proto,
      mime_type);

end: ;

  return client_ctx->output != NULL;
}



static bool on_http_method_post(struct http_client_ctx * client_ctx)
{
  const http_request * q = &client_ctx->req;

  char name[256] = "";
  char opts[256] = "";

  const char * url = q->url;
  const char * ifmt = NULL;

  int status = 0;


  while ( *url == '/' ) {
    ++url;
  }

  sscanf(url,"%255[^?]?%255s", name, opts);

  if ( (ifmt = strstr(opts, "fmt=")) ) {
    ifmt += 4;
  }

  status = ffms_create_input(&client_ctx->input, name,
      &(struct ffms_create_input_args ) {
            .cookie = client_ctx,
            .recv_pkt = on_http_recvpkt,
            .on_finish = on_http_input_finished,
          });

  //status = AVERROR(EPERM);

  if ( status ) {

    char * errmsg;

    PDBG("ffms_create_input() fails: %s", av_err2str(status));

    switch ( errno ) {
      case AVERROR(ENOENT) :
        errmsg = "404 NOT FOUND";
      break;
      case AVERROR(EPERM):
        errmsg = "405 Not Allowed";
      break;
      default :
        errmsg = "500 Internal Server Error";
      break;
    }

    http_send_responce(client_ctx,
        "%s %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html>\r\n"
        "<body>\r\n"
        "<h1>ERROR</h1>\r\n"
        "<p>ff_start_input_stream('%s') FAILS</p>\r\n"
        "<p>status=%d (%s)</p>\r\n"
        "</body>\r\n"
        "</html>\r\n",
        q->proto,
        errmsg,
        name,
        status,
        av_err2str(status));

    goto end;
  }


end: ;

  return client_ctx->input != NULL;
}





/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool ffms_add_http_port(uint32_t addrs, uint16_t port)
{
  return create_http_server_ctx(addrs, port) != NULL;
}

