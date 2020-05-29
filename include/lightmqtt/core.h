#ifndef _LIGHTMQTT_CORE_H_
#define _LIGHTMQTT_CORE_H_

#ifdef HAVE_CONFIG_H
#   include <config.h>
#endif

#include <stddef.h>

#ifdef  __cplusplus
extern "C" {
#endif

typedef enum {
    LMQTT_IO_SUCCESS = 0,
    LMQTT_IO_WOULD_BLOCK,
    LMQTT_IO_ERROR
} lmqtt_io_result_t;

typedef enum {
    /* [OS error] error reading string to build outgoing packet */
    LMQTT_ERROR_ENCODE_STRING = 1,
    /* invalid upper nibble in fixed header of incoming packet */
    LMQTT_ERROR_DECODE_FIXED_HEADER_INVALID_TYPE,
    /* invalid lower nibble in fixed header of incoming packet */
    LMQTT_ERROR_DECODE_FIXED_HEADER_INVALID_FLAGS,
    /* invalid remaining length value */
    LMQTT_ERROR_DECODE_FIXED_HEADER_INVALID_REMAINING_LENGTH,
    /* server-specific packet (CONNECT, SUBSCRIBE etc.)*/
    LMQTT_ERROR_DECODE_FIXED_HEADER_SERVER_SPECIFIC,
    /* a packet without variable header or payload (PINGRESP) has a
       non-zero remaining length */
    LMQTT_ERROR_DECODE_NONZERO_REMAINING_LENGTH,
    /* a response has arrived for which no request was sent */
    LMQTT_ERROR_DECODE_NO_CORRESPONDING_REQUEST,
    /* remaining length of packet is smaller than minimum specified for type */
    LMQTT_ERROR_DECODE_RESPONSE_TOO_SHORT,
    /* invalid flags in first byte of CONNACK */
    LMQTT_ERROR_DECODE_CONNACK_INVALID_ACKNOWLEDGE_FLAGS,
    /* invalid return code in second byte of CONNACK */
    LMQTT_ERROR_DECODE_CONNACK_INVALID_RETURN_CODE,
    /* CONNACK contains more than 2 bytes of remaining length */
    LMQTT_ERROR_DECODE_CONNACK_INVALID_LENGTH,
    /* return code count in SUBACK does not match original SUBSCRIBE */
    LMQTT_ERROR_DECODE_SUBACK_COUNT_MISMATCH,
    /* at least one return code in SUBACK is invalid */
    LMQTT_ERROR_DECODE_SUBACK_INVALID_RETURN_CODE,
    /* remaining length is too short for a PUBLISH packet */
    LMQTT_ERROR_DECODE_PUBLISH_INVALID_LENGTH,
    /* id set has no space available to process incoming PUBLISH packet id */
    LMQTT_ERROR_DECODE_PUBLISH_ID_SET_FULL,
    /* topic allocate callback returned an error */
    LMQTT_ERROR_DECODE_PUBLISH_TOPIC_ALLOCATE_FAILED,
    /* [OS error] error writing publish topic using callback */
    LMQTT_ERROR_DECODE_PUBLISH_TOPIC_WRITE_FAILED,
    /* payload allocate callback returned an error */
    LMQTT_ERROR_DECODE_PUBLISH_PAYLOAD_ALLOCATE_FAILED,
    /* [OS error] error writing publish payload using callback */
    LMQTT_ERROR_DECODE_PUBLISH_PAYLOAD_WRITE_FAILED,
    /* message callback returned 0 */
    LMQTT_ERROR_DECODE_PUBLISH_MESSAGE_CALLBACK_FAILED,
    /* id set has no space available to respond incoming PUBREL with PUBCOMP */
    LMQTT_ERROR_DECODE_PUBREL_ID_SET_FULL,
    /* [OS error] error reading from connection socket */
    LMQTT_ERROR_CONNECTION_READ,
    /* [OS error] error writing to connection socket */
    LMQTT_ERROR_CONNECTION_WRITE,
    /* time without any I/O exceeded configured timeout */
    LMQTT_ERROR_TIMEOUT,
    /* client has already been finalized by user */
    LMQTT_ERROR_CLOSED,
    /* CONNACK failed with return code 1 (unacceptable protocol version) */
    LMQTT_ERROR_CONNACK_UNACCEPTABLE_PROTOCOL_VERSION,
    /* CONNACK failed with return code 2 (identifier rejected) */
    LMQTT_ERROR_CONNACK_IDENTIFIER_REJECTED,
    /* CONNACK failed with return code 3 (server unavailable) */
    LMQTT_ERROR_CONNACK_SERVER_UNAVAILABLE,
    /* CONNACK failed with return code 4 (bad user name or password) */
    LMQTT_ERROR_CONNACK_BAD_USER_NAME_OR_PASSWORD,
    /* CONNACK failed with return code 5 (not authorized) */
    LMQTT_ERROR_CONNACK_NOT_AUTHORIZED,
    /* CONNACK callback returned 0 */
    LMQTT_ERROR_CALLBACK_CONNACK,
    /* SUBACK callback returned 0 */
    LMQTT_ERROR_CALLBACK_SUBACK,
    /* UNSUBACK callback returned 0 */
    LMQTT_ERROR_CALLBACK_UNSUBACK,
    /* PUBLISH callback returned 0 */
    LMQTT_ERROR_CALLBACK_PUBLISH,
    /* Websocket header indicates fragmented packet -unimplemented */
    LMQTT_ERROR_DECODE_WS_HEADER_NO_FINAL_BIT,
    /* Websocket header has invalid (reserved) type*/
    LMQTT_ERROR_DECODE_WS_HEADER_INVALID_TYPE,
    /* Received server frame has 'masked' bit set which shouldn't occur */
    LMQTT_ERROR_DECODE_WS_HEADER_SERVER_MASKED,
    /* Received server frame has too big size set - bigger than size_t */
    LMQTT_ERROR_DECODE_WS_HEADER_SIZE_TOO_BIG,
    /* Unsupported websocket frame type*/
    LMQTT_ERROR_WS_UNSUPPORTED_FRAME_TYPE,
		LMQTT_ERROR_WS_HANDSHAKE_INVALID_RESPONSE_KEY,
		LMQTT_ERROR_WS_HANDSHAKE_LINE_TOO_LONG
} lmqtt_error_t;

typedef lmqtt_io_result_t (*lmqtt_io_callback_t)(void *, void *, size_t,
    size_t *, int *);

/* write randomly generated 4 bytes for XORing websocket packets */
typedef void (*lmqtt_get_websocket_xor_cipher_t)(unsigned char *destination);

#ifdef  __cplusplus
}
#endif

#ifdef LMQTT_TEST
    #define LMQTT_STATIC
#else
    #define LMQTT_STATIC static
#endif

#endif
