/* SculptForge additions to Mbed TLS's default configuration (cmake/WebRTC.cmake). libdatachannel's DTLS code
   refers to the DTLS-SRTP API even without media, so it must be compiled in. */
#define MBEDTLS_SSL_DTLS_SRTP
