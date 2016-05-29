#! /bin/bash
#
# http://stackoverflow.com/questions/10175812/how-to-create-a-self-signed-certificate-with-openssl
# https://habrahabr.ru/post/192446/
# https://www.madboa.com/geek/openssl/#how-do-i-generate-a-self-signed-certificate
#

domain=localhost

openssl req \
  -newkey rsa:2048 -nodes -keyout ${domain}.key \
  -x509 -days 365 -out ${domain}.crt \
  -subj "/C=UA/ST=HOME/L=ROOM/O=MYORGANIZATION/OU=MYORGANIZATIONUNIT/CN=${domain}"
