/*
 * cdns.c
 *
 *  Created on: Jul 9, 2016
 *      Author: amyznikov
 */

#include "cdns.h"
#include "dns.h"
#include <errno.h>


static int cdns_status2errno(int status)
{
  switch ( status ) {
    case DNS_ENOBUFS :
      case DNS_EILLEGAL :
      case DNS_EORDER :
      case DNS_ESECTION :
      case DNS_EUNKNOWN :
      case DNS_EADDRESS :
      case DNS_ENOQUERY :
      case DNS_ENOANSWER :
      case DNS_EFETCHED :
      case DNS_ESERVICE :
      case DNS_EFAIL :
      case DNS_ENONAME :
      return ENODATA;

    default :
      break;
  }
  return status;
}


static struct dns_resolv_conf * getresconf(int * error)
{
  struct dns_resolv_conf * resconf = NULL;

  *error = 0;

  if (!(resconf = dns_resconf_open(error))) {
    goto end;
  }

  if ( (*error = dns_resconf_loadpath(resconf, "/etc/resolv.conf")) ) {
    goto end;
  }

  if ( (*error = dns_nssconf_loadpath(resconf, "/etc/nsswitch.conf")) ) {
    goto end;
  }

end:

  if ( *error && resconf ) {
    dns_resconf_close(resconf);
    resconf = NULL;
  }
  return resconf;
}


static struct dns_hosts * gethosts(int * error)
{
  return dns_hosts_local(error);
}



static struct dns_resolver * create_resolver(int * error)
{
  struct dns_resolver * R = NULL;
  struct dns_resolv_conf * resconf = NULL;
  struct dns_hosts * hosts = NULL;
  struct dns_hints * hints = NULL;

  *error = 0;

  if ( !(resconf = getresconf(error)) ) {
    goto end;
  }

  if ( !(hosts = gethosts(error)) ) {
    goto end;
  }

  if ( !(hints = dns_hints_local(resconf, error)) ) {
    goto end;
  }

  if ( !(R = dns_res_open(resconf, hosts, hints, NULL, dns_opts(), error)) ) {
    goto end;
  }

end : ;

  if ( hints ) {
    dns_hints_close(hints);
  }

  if ( hosts ) {
    dns_hosts_close(hosts);
  }

  if ( resconf ) {
    dns_resconf_close(resconf);
  }

  if ( *error && R ) {
    dns_res_close(R), R = NULL;
  }

  return R;
}


//  struct addrinfo ai_hints = {
//    .ai_family = PF_UNSPEC,
//    .ai_socktype = SOCK_STREAM,
//    .ai_flags = AI_CANONNAME
//  };
int cdns_query_submit(struct cdns_query ** pq, const char * name, const struct addrinfo * hints)
{
  struct dns_resolver * R = NULL;
  struct dns_addrinfo * ai = NULL;
  int error = 0;


  if ( !(R = create_resolver(&error)) ) {
    goto end;
  }

  if ( !(ai = dns_ai_open(name, NULL, DNS_T_A, hints, R, &error)) ) {
    goto end;
  }

end : ;

  if ( R ) {
    dns_res_close(R);
  }

  if ( error && ai ) {
    dns_ai_close(ai), ai = NULL;
    errno = cdns_status2errno(error);
  }

  * pq = (struct cdns_query *)ai;

  return error;
}


int cdns_query_pollfd(const struct cdns_query * q)
{
  return dns_ai_pollfd((struct dns_addrinfo*) q);
}

int cdns_query_fetch(const struct cdns_query * q, struct addrinfo ** ent)
{
  int status = dns_ai_nextent(ent, (struct dns_addrinfo*) q);
  if ( status ) {
    errno = cdns_status2errno(status);
  }

  return status;
}

void cdns_query_destroy(struct cdns_query ** q)
{
  if ( q && *q ) {
    dns_ai_close((struct dns_addrinfo *) *q);
    *q = NULL;
  }
}

const char * cdns_strerror(int status)
{
  return dns_strerror(status);
}
