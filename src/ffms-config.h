/*
 * ffcfg.h
 *
 *  Created on: May 25, 2016
 *      Author: amyznikov
 */

//#pragma once

#ifndef __ffcfg_h__
#define __ffcfg_h__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include "ccarray.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ffconfig {

  char * logfilename;

  int avloglevel;

  int ncpu;

  struct {
    enum {ffmsdb_txtfile, ffmsdb_sqlite3, ffmsdb_pg} type;
    struct {
      char * name;
    } txtfile;
    struct {
      char * name;
    } sqlite3;
    struct {
      char * host;
      char * port;
      char * db;
      char * user;
      char * psw;
      char * options;
      char * tty;
    } pg;
  } db;


  struct {
    int idle;
    int intvl;
    int probes;
    bool enable;
  } keepalive;

  struct {
    ccarray_t faces;
//    uint32_t  address;
//    uint16_t  port;
    size_t    rxbuf;
    size_t    txbuf;
    int       rcvtmo;
    int       sndtmo;
  } http;

  struct {
    ccarray_t faces;
    //uint32_t  address;
    //uint16_t  port;
    char *    cert;
    char *    key;
    size_t    rxbuf;
    size_t    txbuf;
    int       rcvtmo;
    int       sndtmo;
  } https;


};

extern struct ffconfig ffms;

bool ffms_parse_option(char * keyname, char * keyvalue);
bool ffms_read_config_file(const char * fname);

#ifdef __cplusplus
}
#endif

#endif /* __ffcfg_h__ */
