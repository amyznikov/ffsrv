/*
 * cossl.c
 *
 *  Created on: May 27, 2016
 *      Author: amyznikov
 */

#include "co-ssl.h"

#include <string.h>
#include <pthread.h>
#include <openssl/rand.h>
#include <openssl/bio.h>
#include <openssl/ecdh.h>

#include "debug.h"


static bool ssl_initialized = false;
static pthread_rwlock_t * ssl_locks;


/*********************************************************************************************************************
 * OpenSSL in threaded environment
 *  @see http://www.openssl.org/support/faq.html#PROG1
 *  @see http://www.openssl.org/docs/crypto/threads.html
 */

static void thread_id_callback(CRYPTO_THREADID * p) {
  p->ptr = NULL, p->val = pthread_self();
}

static void pthreads_locking_callback(int mode, int type, const char *file, int line)
{
  (void)(file);
  (void)(line);

  if ( !(mode & CRYPTO_LOCK) ) {
    // fprintf(stderr, "unlock %3d from %s:%d", type, file, line);
    pthread_rwlock_unlock(&ssl_locks[type]);
  }
  else if ( mode & CRYPTO_WRITE ) {
    // fprintf(stderr, "wrlock %3d from %s:%d", type, file, line);
    pthread_rwlock_wrlock(&ssl_locks[type]);
  }
  else if ( mode & CRYPTO_READ ) {
    // fprintf(stderr, "rdlock %3d from %s:%d", type, file, line);
    pthread_rwlock_rdlock(&ssl_locks[type]);
  }
  else {
    //errmsg("invalid lock mode 0x%X type=%d from %s:%d", mode, type, file, line);
  }
}

static void ssl_thread_setup(void)
{
  int i, n = CRYPTO_num_locks();
  ssl_locks = OPENSSL_malloc(n * sizeof(*ssl_locks));
  for ( i = 0; i < n; i++ ) {
    pthread_rwlock_init(&ssl_locks[i], NULL);
  }

  CRYPTO_THREADID_set_callback(thread_id_callback);
  CRYPTO_set_locking_callback(pthreads_locking_callback);
}

//static void ssl_thread_cleanup(void)
//{
//  int i, n;
//  CRYPTO_set_locking_callback(NULL);
//  for ( i = 0, n = CRYPTO_num_locks(); i < n; i++ ) {
//    pthread_rwlock_destroy(&ssl_locks[i]);
//  }
//  OPENSSL_free(ssl_locks);
//}

static bool ssl_init(void)
{
  if ( !ssl_initialized ) {

    struct timespec tp;

    PDBG("Doing OpenSSL Initialization");

    ssl_thread_setup();

    ERR_load_crypto_strings();
    OPENSSL_load_builtin_modules();
    //ENGINE_load_builtin_engines();
    OpenSSL_add_all_ciphers();
    OpenSSL_add_all_digests();
    SSL_library_init();

    clock_gettime(CLOCK_MONOTONIC, &tp);
    RAND_seed(&tp, sizeof(tp));

    ssl_initialized = true;
  }

  return ssl_initialized;
}


/*********************************************************************************************************************
 * SSL BIO for coscheduler
 */

static int bio_cosock_read(BIO * bio, char * buf, int size)
{
  return buf ? cosocket_recv(bio->ptr, buf, size, 0) : 0;
}

static int bio_cosock_write(BIO * bio, const char * buf, int size)
{
  return cosocket_send(bio->ptr, buf, size, 0);
}

static int bio_cosock_puts(BIO * bio, const char * str)
{
  return bio_cosock_write(bio, str, strlen(str));
}


static long bio_cosock_ctrl(BIO * bio, int cmd, long arg1, void *arg2)
{
  (void)(bio);

  long status = 1;

  switch ( cmd ) {
    case BIO_CTRL_PUSH :
    case BIO_CTRL_POP:
    case BIO_CTRL_FLUSH :
    break;

    default :
      PDBG("cmd=%d arg1=%ld arg2=%p", cmd, arg1, arg2);
      status = 0;
    break;
  }

  return status;
}

static BIO_METHOD methods_cosock = {
  .type = BIO_TYPE_NULL,
  .name = "cosocket",
  .bwrite = bio_cosock_write,
  .bread = bio_cosock_read,
  .bputs = bio_cosock_puts,
  .bgets = NULL,
  .ctrl = bio_cosock_ctrl,
  .create = NULL,
  .destroy = NULL,
  .callback_ctrl = NULL,
};


static BIO_METHOD * BIO_cosock(void)
{
  return &methods_cosock;
}

static BIO * BIO_cosock_new(struct cosocket * so)
{
  BIO * bio;

  if ( (bio = BIO_new(BIO_cosock())) ) {
    bio->ptr = so;
    bio->init= 1;
    bio->num = 0;
    bio->flags=0;
  }

  return bio;
}



#ifndef SSL_CTRL_SET_ECDH_AUTO
/**
 * See https://wiki.openssl.org/index.php/Diffie-Hellman_parameters
 */
static bool SSL_CTX_set_ecdh_auto(SSL_CTX * ctx, bool onoff)
{
  (void)(onoff);

  EC_KEY * ecdh = NULL;
  bool fok = false;

  if ( !(ecdh = EC_KEY_new_by_curve_name (NID_X9_62_prime256v1))) {
    PDBG("EC_KEY_new_by_curve_name(NID_X9_62_prime256v1) fails");
    PSSL();
    goto end;
  }

  if ( SSL_CTX_set_tmp_ecdh (ctx, ecdh) != 1 ) {
    PDBG("SSL_CTX_set_tmp_ecdh() fails");
    PSSL();
    goto end;
  }

  fok= true;

end:

  if ( ecdh ) {
    EC_KEY_free (ecdh);
  }

  return fok;
}

#endif


static void pdbg_ciphers(SSL_CTX * ssl_ctx)
{
  SSL * ssl;
  STACK_OF(SSL_CIPHER) * sk_cipher;
  const char * cipher;
  int n;

  if ( (ssl = SSL_new(ssl_ctx)) ) {
    sk_cipher = SSL_get_ciphers(ssl);
    n = sk_SSL_CIPHER_num(sk_cipher);
    for (int i = 0; i < n; ++i) {
      const SSL_CIPHER *c = sk_SSL_CIPHER_value(sk_cipher,i);
      cipher = SSL_CIPHER_get_name(c);
      PDBG("CIPHER[%d]: %s", i, cipher);
    }
    SSL_free(ssl);
  }
}


SSL_CTX * co_ssl_create_context(const char * certfile, const char * keyfile, const char * cipher_list)
{
  SSL_CTX * ssl_ctx = NULL;
  bool fok = false;

  ssl_init();

  if ( !(ssl_ctx = SSL_CTX_new(SSLv23_method())) ) {
    PDBG("SSL_CTX_new() fails");
    goto end;
  }

  SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
  SSL_CTX_set_ecdh_auto(ssl_ctx, true);


  // Tell SSL that we don't want to request client certificates for verification
  SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);


  if ( certfile && SSL_CTX_use_certificate_file(ssl_ctx, certfile, SSL_FILETYPE_PEM) != 1 ) {
    PDBG("SSL_CTX_use_certificate_file() fails");
    goto end;
  }

  if ( keyfile && SSL_CTX_use_PrivateKey_file(ssl_ctx, keyfile, SSL_FILETYPE_PEM) != 1 ) {
    PDBG("SSL_CTX_use_PrivateKey_file() fails");
    goto end;
  }

  if ( certfile && keyfile && SSL_CTX_check_private_key(ssl_ctx) != 1 ) {
    PDBG("SSL_CTX_check_private_key() fails");
    goto end;
  }


  if ( cipher_list && *cipher_list && SSL_CTX_set_cipher_list(ssl_ctx, cipher_list) != 1 ) {
    PDBG("SSL_CTX_set_cipher_list() fails");
    goto end;
  }

  pdbg_ciphers(ssl_ctx);

  fok = true;

end: ;

  if ( !fok && ssl_ctx ) {
    SSL_CTX_free(ssl_ctx);
    ssl_ctx = NULL;
  }

  return ssl_ctx;
}

void co_ssl_delete_context(SSL_CTX ** ssl_ctx)
{
  if ( ssl_ctx && *ssl_ctx ) {
    SSL_CTX_free(*ssl_ctx);
    *ssl_ctx = NULL;
  }
}

SSL * co_ssl_new(SSL_CTX * ssl_ctx, struct cosocket * so)
{
  SSL * ssl = NULL;
  BIO * bio = NULL;
  bool fok = false;

  if ( !(ssl = SSL_new(ssl_ctx)) ) {
    PDBG("SSL_new() fails");
    goto end;
  }

  if ( !(bio = BIO_cosock_new(so)) ) {
    PDBG("BIO_cosock_new() fails");
    goto end;
  }

  SSL_set_bio(ssl, bio, bio);
  fok = true;

end :
  if ( !fok ) {
    SSL_free(ssl);
    ssl = NULL;
  }

  return ssl;
}

void co_ssl_free(SSL ** ssl)
{
  if ( ssl && *ssl ) {
    SSL_free(*ssl);
    * ssl = NULL;
  }
}
