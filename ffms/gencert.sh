#! /bin/bash

domain=localhost

#openssl req -newkey rsa:2048 -nodes -keyout ${domain}.key -out ${domain}.csr
openssl req -newkey rsa:2048 -nodes -keyout ${domain}.key -x509 -days 365 -out ${domain}.crt
