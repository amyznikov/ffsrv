/*
 * ffcfg.h
 *
 *  Created on: May 29, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ffcfg_h__
#define __ffcfg_h__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "ccarray.h"


#ifdef __cplusplus
extern "C" {
#endif

extern struct ffsrv_config {

  char * logfilename;

  int avloglevel;

  int ncpu;

  struct {
    char * root;
  } db;


  struct {
    int idle;
    int intvl;
    int probes;
    bool enable;
  } keepalive;

  struct {
    ccarray_t faces;
    size_t    rxbuf;
    size_t    txbuf;
    int       rcvtmo;
    int       sndtmo;
  } http;

  struct {
    ccarray_t faces;
    char *    cert;
    char *    key;
    size_t    rxbuf;
    size_t    txbuf;
    int       rcvtmo;
    int       sndtmo;
  } https;

} ffsrv;


const char * ffsrv_find_config_file(void);
bool ffsrv_read_config_file(const char * fname);
bool ffsrv_parse_option(char * keyname, char * keyvalue);



#ifdef __cplusplus
}
#endif

#endif /* __ffcfg_h__ */
