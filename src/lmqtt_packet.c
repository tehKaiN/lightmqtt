#include <lightmqtt/packet.h>
#include <lightmqtt/types.h>
#include <string.h>
#include <assert.h>

#define LMQTT_CONNACK_RETURN_CODE_MAX 5
#define LMQTT_ERROR_CONNACK_BASE \
    (LMQTT_ERROR_CONNACK_UNACCEPTABLE_PROTOCOL_VERSION - 1)

#define LMQTT_FLAG_CLEAN_SESSION 0x02
#define LMQTT_FLAG_WILL_FLAG 0x04
#define LMQTT_FLAG_WILL_RETAIN 0x20
#define LMQTT_FLAG_PASSWORD_FLAG 0x40
#define LMQTT_FLAG_USER_NAME_FLAG 0x80

#define LMQTT_STRING_LEN_SIZE 2
#define LMQTT_PACKET_ID_SIZE 2
#define LMQTT_REMAINING_LENGTH_MAX_SIZE 4

#define STRING_LEN_BYTE(val, num) (((val) >> ((num) * 8)) & 0xff)

#define LMQTT_QOS_TO_CONNECT_WILL_QOS(x) ((x) << 3)
#define LMQTT_QOS_TO_SUBSCRIBE_REQUESTED_QOS(x) (x)
#define LMQTT_QOS_TO_PUBLISH_QOS(x) ((x) << 1)
#define QOS_TO_LMQTT_QOS(x) (x)
#define IS_VALID_LMQTT_QOS(x) ((x) >= LMQTT_QOS_0 && (x) <= LMQTT_QOS_2)

/******************************************************************************
 * GENERAL PRIVATE functions
 ******************************************************************************/

/* caller must guarantee buf is at least 4-bytes long! */
LMQTT_STATIC size_t encode_remaining_length(long len, unsigned char *buf)
{
    size_t result;

    assert(len >= 0 && len <= 0x0fffffff);

    result = 0;
    do {
        unsigned char b = len % 128;
        len /= 128;
        buf[result++] = len > 0 ? b | 0x80 : b;
    } while (len > 0);

    return result;
}

LMQTT_STATIC size_t calc_mqtt_packet_len(long payload_len)
{
    /* header's first byte + 'remaining length' field len + payload len */
    size_t packet_len = 1 + 1 + payload_len;

    /* increment for each 'remaining length' field's additional byte */
    while(payload_len /= 128)
        ++packet_len;

    return packet_len;
}

LMQTT_STATIC int kind_expects_response(lmqtt_kind_t kind)
{
    return kind != LMQTT_KIND_PUBLISH_0 && kind != LMQTT_KIND_PUBACK &&
        kind != LMQTT_KIND_PUBREC && kind != LMQTT_KIND_PUBCOMP &&
        kind != LMQTT_KIND_DISCONNECT;
}

LMQTT_STATIC unsigned char get_next_ws_xor(lmqtt_tx_buffer_t *tx_buffer)
{
    unsigned char *xor_table = tx_buffer->internal.ws_xor;
    unsigned char next_byte = xor_table[tx_buffer->internal.ws_xor_pos];
    if(++tx_buffer->internal.ws_xor_pos >= 4)
        tx_buffer->internal.ws_xor_pos = 0;
    return next_byte;
}

LMQTT_STATIC lmqtt_encode_result_t encode_ws_header(lmqtt_encode_buffer_t *encode_buffer,
    unsigned char *buf, size_t buf_len, size_t payload_len, size_t *bytes_written)
{
    /* Get tx buffer from encode buffer */
    *bytes_written = 0;
    lmqtt_tx_buffer_t *tx_buffer = (lmqtt_tx_buffer_t *)(
        (unsigned char*)encode_buffer -
        offsetof(lmqtt_tx_buffer_t, internal.buffer));
    if(!tx_buffer->ws_enabled) {
        return 0;
    }

    if(buf_len < 10) {
        return LMQTT_ENCODE_CONTINUE;
    }

    /* Set websocket header - final & binary frame, set masked bit */
    size_t pos = 0;
    buf[pos++] = 0x82;
    buf[pos] = 0x80;

    /* append size code */
    if(payload_len < 126) {
        buf[pos++] |= payload_len;
    }
    else if(payload_len < 0xFFFF) {
        /* code 16-bit size*/
        buf[pos++] |= 0x7E;
        buf[pos++] = (payload_len >> 8) & 0xFF;
        buf[pos++] = (payload_len >> 0) & 0xFF;
    }
    else {
        /* code for 64-bit size*/
        buf[pos++] |= 0x7F;
        buf[pos++] = 0;
        buf[pos++] = 0;
        buf[pos++] = 0;
        buf[pos++] = 0;
        buf[pos++] = (payload_len >> 24) & 0xFF;
        buf[pos++] = (payload_len >> 16) & 0xFF;
        buf[pos++] = (payload_len >>  8) & 0xFF;
        buf[pos++] = (payload_len >>  0) & 0xFF;
    }

    /* Append XOR cipher */
    if(tx_buffer->get_ws_xor != NULL)
        tx_buffer->get_ws_xor(tx_buffer->internal.ws_xor);
    buf[pos++] = tx_buffer->internal.ws_xor[0];
    buf[pos++] = tx_buffer->internal.ws_xor[1];
    buf[pos++] = tx_buffer->internal.ws_xor[2];
    buf[pos++] = tx_buffer->internal.ws_xor[3];

    *bytes_written = pos;
    return LMQTT_ENCODE_FINISHED;
}

/******************************************************************************
 * lmqtt_id_list_t PUBLIC functions
 ******************************************************************************/

void lmqtt_id_set_clear(lmqtt_id_set_t *id_set)
{
    id_set->count = 0;
}

int lmqtt_id_set_contains(lmqtt_id_set_t *id_set, lmqtt_packet_id_t id)
{
    int i;

    for (i = 0; i < id_set->count; i++) {
        if (id_set->items[i] == id)
            return 1;
    }

    return 0;
}

int lmqtt_id_set_put(lmqtt_id_set_t *id_set, lmqtt_packet_id_t id)
{
    int i;

    if (id_set->count >= id_set->capacity)
        return 0;

    for (i = 0; i < id_set->count; i++) {
        if (id_set->items[i] == id)
            return 0;
    }

    id_set->items[id_set->count++] = id;
    return 1;
}

int lmqtt_id_set_remove(lmqtt_id_set_t *id_set, lmqtt_packet_id_t id)
{
    int i;

    for (i = 0; i < id_set->count; i++) {
        if (id_set->items[i] == id) {
            memmove(&id_set->items[i], &id_set->items[i + 1],
                sizeof(&id_set->items[0]) * (id_set->count - i - 1));
            id_set->count--;
            return 1;
        }
    }

    return 0;
}

/******************************************************************************
 * lmqtt_encode_buffer_t PRIVATE functions
 ******************************************************************************/

LMQTT_STATIC lmqtt_encode_result_t encode_buffer_encode(
    lmqtt_encode_buffer_t *encode_buffer, lmqtt_store_value_t *value,
    encode_buffer_builder_t builder, size_t offset, unsigned char *buf,
    size_t buf_len, size_t *bytes_written)
{
    size_t cnt;
    int result;

    assert(buf_len >= 0);

    /* encode a packet using a builder */
    if (!encode_buffer->encoded) {
        builder(value, encode_buffer);
        encode_buffer->encoded = 1;
    }
    assert(encode_buffer->buf_len > 0 && offset < encode_buffer->buf_len);

    /* number of bytes to be actually outputed */
    cnt = encode_buffer->buf_len - offset;
    result = LMQTT_ENCODE_FINISHED;

    /* bytes don't fit in output buffer - we'll need to do it in parts */
    if (cnt > buf_len) {
        cnt = buf_len;
        result = LMQTT_ENCODE_CONTINUE;
    }

    memcpy(buf, &encode_buffer->buf[offset], cnt);
    *bytes_written = cnt;

    /* Get tx buffer from encode buffer */
	lmqtt_tx_buffer_t *tx_buffer = (lmqtt_tx_buffer_t *)(
        (unsigned char*)encode_buffer -
        offsetof(lmqtt_tx_buffer_t, internal.buffer));
    if(tx_buffer->ws_enabled)
        for(int i = 0; i < cnt; ++i) {
            buf[i] ^= get_next_ws_xor(tx_buffer);
        }

    if (result == LMQTT_ENCODE_FINISHED)
        memset(encode_buffer, 0, sizeof(*encode_buffer));

    return result;
}

LMQTT_STATIC void encode_buffer_encode_packet_id(
    lmqtt_encode_buffer_t *encode_buffer, int type, long remaining_length,
    lmqtt_packet_id_t packet_id)
{
    int i;
    size_t v;

    assert(sizeof(encode_buffer->buf) >= 5);

    v = encode_remaining_length(remaining_length, encode_buffer->buf + 1);
    assert(sizeof(encode_buffer->buf) >= v + LMQTT_PACKET_ID_SIZE + 1);

    encode_buffer->buf[0] = type;
    for (i = 0; i < LMQTT_PACKET_ID_SIZE; i++)
        encode_buffer->buf[v + i + 1] = STRING_LEN_BYTE(packet_id,
            LMQTT_PACKET_ID_SIZE - i - 1);

    encode_buffer->buf_len = v + LMQTT_PACKET_ID_SIZE + 1;
}

/******************************************************************************
 * lmqtt_string_t PRIVATE functions
 ******************************************************************************/

static lmqtt_string_result_t string_move(lmqtt_string_t *str,
    unsigned char *buf, size_t buf_len, lmqtt_io_callback_t callback,
    unsigned char *dst, unsigned char *src, size_t *bytes_moved, int *os_error)
{
    *bytes_moved = 0;
    *os_error = 0;

    if (callback && str->buf) {
        return LMQTT_STRING_INVALID_OBJECT;
    } else if (buf_len == 0) {
        return LMQTT_STRING_SUCCESS;
    } else if (callback) {
        /* TODO: we are trusting the callback will never return a nonzero bytes
           moved count with LMQTT_STRING_WOULD_BLOCK; perhaps we should validate
           this and return something like LMQTT_STRING_INVALID_CALLBACK? */
        switch (callback(str->data, buf, buf_len, bytes_moved, os_error)) {
            case LMQTT_IO_SUCCESS:
                return LMQTT_STRING_SUCCESS;
            case LMQTT_IO_WOULD_BLOCK:
                return LMQTT_STRING_WOULD_BLOCK;
            default:
                return LMQTT_STRING_OS_ERROR;
        }
    } else if (str->buf) {
        assert(str->internal.pos + buf_len <= str->len);

        memcpy(dst, src, buf_len);
        *bytes_moved = buf_len;
        str->internal.pos += buf_len;
        return LMQTT_STRING_SUCCESS;
    }

    return LMQTT_STRING_INVALID_OBJECT;
}

LMQTT_STATIC lmqtt_string_result_t string_read(lmqtt_string_t *str,
    unsigned char *buf, size_t buf_len, size_t *bytes_read, int *os_error)
{
    return string_move(str, buf, buf_len, str->read, buf,
        (unsigned char *) &str->buf[str->internal.pos], bytes_read, os_error);
}

LMQTT_STATIC lmqtt_string_result_t string_write(lmqtt_string_t *str,
    unsigned char *buf, size_t buf_len, size_t *bytes_written, int *os_error)
{
    return string_move(str, buf, buf_len, str->write, (unsigned char *)
        &str->buf[str->internal.pos], buf, bytes_written, os_error);
}

LMQTT_STATIC lmqtt_encode_result_t string_encode(lmqtt_string_t *str,
    int encode_len, int encode_if_empty, size_t offset, unsigned char *buf,
    size_t buf_len, size_t *bytes_written, lmqtt_encode_buffer_t *encode_buffer)
{
    long len = str->len;
    int result;
    size_t pos = 0;
    size_t offset_str = offset;
    int i;

    size_t read_cnt;
    lmqtt_string_result_t read_res;
    long remaining;

    /* Get tx buffer from encode buffer */
	lmqtt_tx_buffer_t *tx_buffer = (lmqtt_tx_buffer_t *)(
        (unsigned char*)encode_buffer -
        offsetof(lmqtt_tx_buffer_t, internal.buffer));

    *bytes_written = 0;
    encode_buffer->blocking_str = NULL;

    if (len == 0 && !encode_if_empty)
        return LMQTT_ENCODE_FINISHED;

    if (buf_len == 0)
        return LMQTT_ENCODE_CONTINUE;

    assert(buf_len > 0);
    assert(offset < len + (encode_len ? LMQTT_STRING_LEN_SIZE : 0));

    if (encode_len) {
        for (i = 0; i < LMQTT_STRING_LEN_SIZE; i++) {
            if (offset <= i) {
                buf[pos] = STRING_LEN_BYTE(len, LMQTT_STRING_LEN_SIZE - i - 1);
                if(tx_buffer->ws_enabled)
                    buf[pos] ^= get_next_ws_xor(tx_buffer);
                ++pos;
                *bytes_written += 1;
                if (pos >= buf_len) {
                    return pos >= LMQTT_STRING_LEN_SIZE && len == 0 ?
                        LMQTT_ENCODE_FINISHED : LMQTT_ENCODE_CONTINUE;
                }
            }
        }
        offset_str = offset <= LMQTT_STRING_LEN_SIZE ? 0 :
            offset - LMQTT_STRING_LEN_SIZE;
    }

    len -= (long) offset_str;
    remaining = str->len - (long) offset_str;

    if (len > (long) (buf_len - pos))
        len = (long) (buf_len - pos);
    if (offset_str == 0)
        memset(&str->internal, 0, sizeof(str->internal));

    read_res = string_read(str, buf + pos, (size_t) len, &read_cnt,
        &encode_buffer->os_error);
    *bytes_written += read_cnt;
    assert((long) read_cnt <= remaining);

    if(tx_buffer->ws_enabled)
        for(i = 0; i < read_cnt; ++i)
            buf[pos + i] ^= get_next_ws_xor(tx_buffer);

    if (read_res == LMQTT_STRING_WOULD_BLOCK) {
        encode_buffer->blocking_str = str;
        result = LMQTT_ENCODE_WOULD_BLOCK;
    } else if (read_res == LMQTT_STRING_SUCCESS && (long) read_cnt >= remaining) {
        result = LMQTT_ENCODE_FINISHED;
    } else if (read_res == LMQTT_STRING_SUCCESS && read_cnt > 0) {
        result = LMQTT_ENCODE_CONTINUE;
    } else {
        encode_buffer->error = LMQTT_ERROR_ENCODE_STRING;
        result = LMQTT_ENCODE_ERROR;
    }

    return result;
}

LMQTT_STATIC long string_calc_field_length(lmqtt_string_t *str)
{
    return str->len > 0 ? LMQTT_STRING_LEN_SIZE + str->len : 0;
}

LMQTT_STATIC int string_validate_field_length(lmqtt_string_t *str)
{
    return str->len >= 0 && str->len <= 0xffff;
}

/******************************************************************************
 * lmqtt_fixed_header_t PRIVATE functions
 ******************************************************************************/

LMQTT_STATIC lmqtt_decode_result_t websocket_header_decode(
    lmqtt_websocket_header_t *ws_header, unsigned char b, lmqtt_error_t *error)
{
    int result = LMQTT_DECODE_ERROR;
    *error = 0;

    if (ws_header->internal.error) {
        *error = ws_header->internal.error;
        return LMQTT_DECODE_ERROR;
    }

    if(ws_header->internal.bytes_read == 0) {
        /* Parse first byte of header */

        /* Fragmented WS packets are unsupported for now */
        if(!(b & 0x80))
            *error = LMQTT_ERROR_DECODE_WS_HEADER_NO_FINAL_BIT;
        else {
            /* Get packet type and validate it */
            ws_header->type = b & 0x0F;
            if((ws_header->type >= 3 && ws_header->type <= 7) ||
                    (ws_header->type >= 0xB && ws_header->type <= 0xF))
                *error = LMQTT_ERROR_DECODE_WS_HEADER_INVALID_TYPE;
            else {
                result = LMQTT_DECODE_CONTINUE;
            }
        }
    } else if(ws_header->internal.bytes_read == 1) {
        /* Packets from server shouldn't have 'masked' bit set */
        if(!(b & 0x80))
            *error = LMQTT_ERROR_DECODE_WS_HEADER_SERVER_MASKED;

        /* Read length code*/
        unsigned char size_code = b & 0x7F;
        if(size_code == 127) {
            /* size in 64-bit field */
            ws_header->internal.expected_size = 10;
            ws_header->packet_size = 0;
            result = LMQTT_DECODE_CONTINUE;
        } else if(size_code == 126) {
            /* size in 16-bit field */
            ws_header->internal.expected_size = 4;
            ws_header->packet_size = 0;
            result = LMQTT_DECODE_CONTINUE;
        } else {
            /* size in 7-bit 'code' field */
            ws_header->internal.expected_size = 2;
            ws_header->packet_size = size_code;
            result = LMQTT_DECODE_FINISHED;
        }
    } else {
        assert(ws_header->internal.bytes_read < ws_header->internal.expected_size);

        if(ws_header->internal.expected_size == 10 &&
                (ws_header->internal.bytes_read < 10 - sizeof(size_t)) && b)
            *error = LMQTT_ERROR_DECODE_WS_HEADER_SIZE_TOO_BIG;
        else {
            /* Size is in big endian so we can just shift bytes from right */
            ws_header->packet_size = (ws_header->packet_size << 8) | b;

            if(ws_header->internal.bytes_read == ws_header->internal.expected_size - 1)
                result = LMQTT_DECODE_FINISHED;
            else
                result = LMQTT_DECODE_CONTINUE;
        }
    }

    if (result == LMQTT_DECODE_ERROR)
        ws_header->internal.error = *error;
    else
        ws_header->internal.bytes_read += 1;
    return result;
}

LMQTT_STATIC lmqtt_decode_result_t fixed_header_decode(
    lmqtt_fixed_header_t *header, unsigned char b, lmqtt_error_t *error)
{
    int result = LMQTT_DECODE_ERROR;
    *error = 0;

    if (header->internal.error) {
        *error = header->internal.error;
        return LMQTT_DECODE_ERROR;
    }

    if (header->internal.bytes_read == 0) {
        int type = b >> 4;
        int flags = b & 0x0f;
        int bad_flags;

        switch (type) {
            case LMQTT_TYPE_PUBREL:
            case LMQTT_TYPE_SUBSCRIBE:
            case LMQTT_TYPE_UNSUBSCRIBE:
                bad_flags = flags != 2;
                break;
            case LMQTT_TYPE_PUBLISH:
                bad_flags = (flags & 6) == 6 || (flags & 14) == 8;
                break;
            default:
                bad_flags = flags != 0;
        }

        if (type < LMQTT_TYPE_MIN || type > LMQTT_TYPE_MAX) {
            *error = LMQTT_ERROR_DECODE_FIXED_HEADER_INVALID_TYPE;
            result = LMQTT_DECODE_ERROR;
        } else if (bad_flags) {
            *error = LMQTT_ERROR_DECODE_FIXED_HEADER_INVALID_FLAGS;
            result = LMQTT_DECODE_ERROR;
        } else {
            header->type = type;
            header->internal.remain_len_multiplier = 1;
            header->internal.remain_len_accumulator = 0;
            header->internal.remain_len_finished = 0;
            if (type == LMQTT_TYPE_PUBLISH) {
                header->dup = (flags & 8) >> 3;
                header->qos = (flags & 6) >> 1;
                header->retain = flags & 1;
            } else {
                header->dup = 0;
                header->qos = 0;
                header->retain = 0;
            }
            result = LMQTT_DECODE_CONTINUE;
        }
    } else {
        if ((header->internal.remain_len_multiplier > 128 * 128 && (b & 128) != 0) ||
                (header->internal.remain_len_multiplier > 1 && b == 0) ||
                header->internal.remain_len_finished) {
            *error = LMQTT_ERROR_DECODE_FIXED_HEADER_INVALID_REMAINING_LENGTH;
            result = LMQTT_DECODE_ERROR;
        } else {
            header->internal.remain_len_accumulator += (b & 127) *
                header->internal.remain_len_multiplier;
            header->internal.remain_len_multiplier *= 128;

            if (b & 128) {
                result = LMQTT_DECODE_CONTINUE;
            } else {
                header->remaining_length =
                    header->internal.remain_len_accumulator;
                header->internal.remain_len_finished = 1;
                result = LMQTT_DECODE_FINISHED;
            }
        }
    }

    if (result == LMQTT_DECODE_ERROR)
        header->internal.error = *error;
    else
        header->internal.bytes_read += 1;
    return result;
}

/******************************************************************************
 * lmqtt_connect_t PRIVATE functions
 ******************************************************************************/

#define LMQTT_CONNECT_HEADER_SIZE 10

LMQTT_STATIC lmqtt_encode_result_t encode_const_line(
     const char *line, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    size_t line_len = strlen(line);
    assert(buf_len >= line_len);
    memcpy(buf, line, line_len);
    *bytes_written = line_len;
    return LMQTT_ENCODE_FINISHED;
}

LMQTT_STATIC lmqtt_encode_result_t connect_encode_ws_hanshake_get(
     lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_const_line("GET /mqtt HTTP/1.1\r\n", encode_buffer, offset,
        buf, buf_len, bytes_written);
}

LMQTT_STATIC lmqtt_encode_result_t connect_encode_ws_hanshake_host(
     lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    static const char host[] = "Host: ";
    static const char crlf[] = "\r\n";
    lmqtt_connect_t *connect = (lmqtt_connect_t *)value->value;
    assert(buf_len >= strlen(host) + connect->websocket_addr.len + strlen(crlf));
    strcpy((char*)buf, host);
    strcat((char*)buf, connect->websocket_addr.buf);
    strcat((char*)buf, crlf);
    *bytes_written = strlen((char*)buf);
    return LMQTT_ENCODE_FINISHED;
}

LMQTT_STATIC lmqtt_encode_result_t connect_encode_ws_hanshake_upgrade(
     lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_const_line("Upgrade: websocket\r\n", encode_buffer, offset,
        buf, buf_len, bytes_written);
}

LMQTT_STATIC lmqtt_encode_result_t connect_encode_ws_hanshake_conn_upgrade(
     lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_const_line("Connection: Upgrade\r\n", encode_buffer, offset,
        buf, buf_len, bytes_written);
}

LMQTT_STATIC lmqtt_encode_result_t connect_encode_ws_hanshake_origin(
     lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    static const char origin[] = "Origin: http://";
    static const char crlf[] = "\r\n";
    lmqtt_connect_t *connect = (lmqtt_connect_t *)value->value;
    assert(buf_len >= strlen(origin) + connect->websocket_addr.len + strlen(crlf));
    strcpy((char*)buf, origin);
    strcat((char*)buf, connect->websocket_addr.buf);
    strcat((char*)buf, crlf);
    *bytes_written = strlen((char*)buf);
    return LMQTT_ENCODE_FINISHED;
}

LMQTT_STATIC lmqtt_encode_result_t connect_encode_ws_hanshake_key(
     lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    static const char pre[] = "Sec-WebSocket-Key: ";
    static const char crlf[] = "\r\n";
    lmqtt_connect_t *connect = (lmqtt_connect_t *)value->value;
    assert(buf_len >= strlen(pre) + connect->websocket_key.len + strlen(crlf));
    strcpy((char*)buf, pre);
    strcat((char*)buf, connect->websocket_key.buf);
    strcat((char*)buf, crlf);
    *bytes_written = strlen((char*)buf);
    return LMQTT_ENCODE_FINISHED;
}

LMQTT_STATIC lmqtt_encode_result_t connect_encode_ws_hanshake_version(
     lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_const_line("Sec-WebSocket-Version: 13\r\n", encode_buffer,
        offset, buf, buf_len, bytes_written);
}

LMQTT_STATIC lmqtt_encode_result_t connect_encode_ws_hanshake_protocol(
     lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_const_line("Sec-WebSocket-Protocol: mqtt\r\n", encode_buffer,
        offset, buf, buf_len, bytes_written);
}

LMQTT_STATIC lmqtt_encode_result_t connect_encode_ws_hanshake_end(
     lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_const_line("\r\n", encode_buffer, offset, buf, buf_len,
        bytes_written);
}

LMQTT_STATIC long connect_calc_remaining_length(lmqtt_connect_t *connect)
{
    return LMQTT_CONNECT_HEADER_SIZE +
        /* client_id is always present in payload */
        LMQTT_STRING_LEN_SIZE + connect->client_id.len +
        string_calc_field_length(&connect->will_topic) +
        string_calc_field_length(&connect->will_message) +
        string_calc_field_length(&connect->user_name) +
        string_calc_field_length(&connect->password);
}

LMQTT_STATIC lmqtt_encode_result_t connect_encode_ws_header(
     lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    lmqtt_connect_t *connect = value->value;
    size_t ws_payload_len = calc_mqtt_packet_len(
        connect_calc_remaining_length(connect));
    return encode_ws_header(encode_buffer, buf, buf_len, ws_payload_len,
        bytes_written);
}

LMQTT_STATIC void connect_build_fixed_header(lmqtt_store_value_t *value,
    lmqtt_encode_buffer_t *encode_buffer)
{
    size_t remain_len_size;
    lmqtt_connect_t *connect = value->value;

    assert(sizeof(encode_buffer->buf) >= LMQTT_REMAINING_LENGTH_MAX_SIZE + 1);

    remain_len_size = encode_remaining_length(
        connect_calc_remaining_length(connect), encode_buffer->buf + 1);

    encode_buffer->buf[0] = LMQTT_TYPE_CONNECT << 4;
    encode_buffer->buf_len = 1 + remain_len_size;
}

LMQTT_STATIC lmqtt_encode_result_t connect_encode_fixed_header(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_buffer_encode(encode_buffer, value,
        connect_build_fixed_header, offset, buf, buf_len, bytes_written);
}

LMQTT_STATIC void connect_build_variable_header(lmqtt_store_value_t *value,
    lmqtt_encode_buffer_t *encode_buffer)
{
    unsigned char flags;
    lmqtt_connect_t *connect = value->value;

    assert(sizeof(encode_buffer->buf) >= LMQTT_CONNECT_HEADER_SIZE);

    memcpy(encode_buffer->buf, "\x00\x04MQTT\x04", 7);

    flags = LMQTT_QOS_TO_CONNECT_WILL_QOS(connect->will_qos);
    if (connect->clean_session)
        flags |= LMQTT_FLAG_CLEAN_SESSION;
    if (connect->will_retain)
        flags |= LMQTT_FLAG_WILL_RETAIN;
    if (connect->will_topic.len > 0)
        flags |= LMQTT_FLAG_WILL_FLAG;
    if (connect->user_name.len > 0)
        flags |= LMQTT_FLAG_USER_NAME_FLAG;
    if (connect->password.len > 0)
        flags |= LMQTT_FLAG_PASSWORD_FLAG;
    encode_buffer->buf[7] = flags;

    encode_buffer->buf[8] = STRING_LEN_BYTE(connect->keep_alive, 1);
    encode_buffer->buf[9] = STRING_LEN_BYTE(connect->keep_alive, 0);
    encode_buffer->buf_len = 10;
}

LMQTT_STATIC lmqtt_encode_result_t connect_encode_variable_header(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_buffer_encode(encode_buffer, value,
        connect_build_variable_header, offset, buf, buf_len, bytes_written);
}

LMQTT_STATIC lmqtt_encode_result_t connect_encode_payload_client_id(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    lmqtt_connect_t *connect = value->value;

    return string_encode(&connect->client_id, 1, 1, offset, buf, buf_len,
        bytes_written, encode_buffer);
}

LMQTT_STATIC lmqtt_encode_result_t connect_encode_payload_will_topic(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    lmqtt_connect_t *connect = value->value;

    return string_encode(&connect->will_topic, 1, 0, offset, buf, buf_len,
        bytes_written, encode_buffer);
}

LMQTT_STATIC lmqtt_encode_result_t connect_encode_payload_will_message(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    lmqtt_connect_t *connect = value->value;

    return string_encode(&connect->will_message, 1, 0, offset, buf, buf_len,
        bytes_written, encode_buffer);
}

LMQTT_STATIC lmqtt_encode_result_t connect_encode_payload_user_name(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    lmqtt_connect_t *connect = value->value;

    return string_encode(&connect->user_name, 1, 0, offset, buf, buf_len,
        bytes_written, encode_buffer);
}

LMQTT_STATIC lmqtt_encode_result_t connect_encode_payload_password(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    lmqtt_connect_t *connect = value->value;

    return string_encode(&connect->password, 1, 0, offset, buf, buf_len,
        bytes_written, encode_buffer);
}

/******************************************************************************
 * lmqtt_connect_t PUBLIC functions
 ******************************************************************************/

int lmqtt_connect_validate(lmqtt_connect_t *connect)
{
    if (!string_validate_field_length(&connect->client_id) ||
            !string_validate_field_length(&connect->will_topic) ||
            !string_validate_field_length(&connect->will_message) ||
            !string_validate_field_length(&connect->user_name) ||
            !string_validate_field_length(&connect->password))
        return 0;

    if ((connect->will_topic.len == 0) ^ (connect->will_message.len == 0))
        return 0;

    if (connect->will_topic.len == 0 && connect->will_retain)
        return 0;

    if (connect->client_id.len == 0 && !connect->clean_session)
        return 0;

    if (connect->user_name.len == 0 && connect->password.len != 0)
        return 0;

    if (!IS_VALID_LMQTT_QOS(connect->will_qos))
        return 0;

    return 1;
}

/******************************************************************************
 * lmqtt_subscribe_t PRIVATE functions
 ******************************************************************************/

LMQTT_STATIC long subscribe_calc_remaining_length(lmqtt_subscribe_t *subscribe,
    int include_qos)
{
    int i;
    long result = LMQTT_PACKET_ID_SIZE;

    for (i = 0; i < subscribe->count; i++)
        result += subscribe->subscriptions[i].topic.len +
            LMQTT_STRING_LEN_SIZE + (include_qos ? 1 : 0);

    return result;
}

LMQTT_STATIC lmqtt_encode_result_t subscribe_encode_ws_header_subscribe(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    lmqtt_subscribe_t *subscribe = value->value;
    size_t payload_len = subscribe_calc_remaining_length(subscribe, 1);
    size_t ws_payload_len = calc_mqtt_packet_len(payload_len);
    return encode_ws_header(encode_buffer, buf, buf_len, ws_payload_len,
        bytes_written);
}

LMQTT_STATIC void subscribe_build_header_subscribe(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer)
{
    lmqtt_subscribe_t *subscribe = value->value;

    encode_buffer_encode_packet_id(
        encode_buffer, (LMQTT_TYPE_SUBSCRIBE << 4) | 0x02,
        subscribe_calc_remaining_length(subscribe, 1), value->packet_id);
}

LMQTT_STATIC lmqtt_encode_result_t subscribe_encode_header_subscribe(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_buffer_encode(encode_buffer, value,
        &subscribe_build_header_subscribe, offset, buf, buf_len, bytes_written);
}

LMQTT_STATIC lmqtt_encode_result_t subscribe_encode_ws_header_unsubscribe(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    lmqtt_subscribe_t *subscribe = value->value;
    size_t payload_len = subscribe_calc_remaining_length(subscribe, 0);
    size_t ws_payload_len = calc_mqtt_packet_len(payload_len);
    return encode_ws_header(encode_buffer, buf, buf_len, ws_payload_len,
        bytes_written);
}

LMQTT_STATIC void subscribe_build_header_unsubscribe(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer)
{
    lmqtt_subscribe_t *subscribe = value->value;

    encode_buffer_encode_packet_id(
        encode_buffer, (LMQTT_TYPE_UNSUBSCRIBE << 4) | 0x02,
        subscribe_calc_remaining_length(subscribe, 0), value->packet_id);
}

LMQTT_STATIC lmqtt_encode_result_t subscribe_encode_header_unsubscribe(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_buffer_encode(encode_buffer, value,
        &subscribe_build_header_unsubscribe, offset, buf, buf_len,
        bytes_written);
}

LMQTT_STATIC lmqtt_encode_result_t subscribe_encode_topic(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    lmqtt_subscribe_t *subscribe = value->value;

    return string_encode(&subscribe->internal.current->topic, 1, 1, offset, buf,
        buf_len, bytes_written, encode_buffer);
}

LMQTT_STATIC void subscribe_build_qos(lmqtt_store_value_t *value,
    lmqtt_encode_buffer_t *encode_buffer)
{
    lmqtt_subscribe_t *subscribe = value->value;

    encode_buffer->buf[0] = LMQTT_QOS_TO_SUBSCRIBE_REQUESTED_QOS(
        subscribe->internal.current->requested_qos);
    encode_buffer->buf_len = 1;
}

LMQTT_STATIC lmqtt_encode_result_t subscribe_encode_qos(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_buffer_encode(encode_buffer, value,
        subscribe_build_qos, offset, buf, buf_len, bytes_written);
}

/******************************************************************************
 * lmqtt_subscribe_t PUBLIC functions
 ******************************************************************************/

int lmqtt_subscribe_validate(lmqtt_subscribe_t *subscribe)
{
    int i;
    int cnt = subscribe->count;

    if (cnt == 0 || !subscribe->subscriptions)
        return 0;

    for (i = 0; i < cnt; i++) {
        lmqtt_subscription_t *sub = &subscribe->subscriptions[i];
        if (!string_validate_field_length(&sub->topic))
            return 0;
        if (sub->topic.len == 0 || !IS_VALID_LMQTT_QOS(sub->requested_qos))
            return 0;
    }

    return 1;
}

/******************************************************************************
 * lmqtt_publish_t PRIVATE functions
 ******************************************************************************/

LMQTT_STATIC long publish_calc_remaining_length(lmqtt_publish_t *publish)
{
    return LMQTT_STRING_LEN_SIZE + (long) publish->topic.len +
        (publish->qos == LMQTT_QOS_0 ? 0 : LMQTT_PACKET_ID_SIZE) +
        (long) publish->payload.len;
}

LMQTT_STATIC lmqtt_encode_result_t publish_encode_ws_header(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    lmqtt_publish_t *publish = value->value;
    size_t payload_len = publish_calc_remaining_length(publish);
    size_t ws_payload_len = calc_mqtt_packet_len(payload_len);
    return encode_ws_header(encode_buffer, buf, buf_len, ws_payload_len,
        bytes_written);
}

LMQTT_STATIC void publish_build_fixed_header(lmqtt_store_value_t *value,
    lmqtt_encode_buffer_t *encode_buffer)
{
    size_t v;
    unsigned char type;
    lmqtt_publish_t *publish = value->value;

    /* lmqtt_client_t is supposed to validate packet length */
    assert(lmqtt_publish_validate(publish));

    v = encode_remaining_length(publish_calc_remaining_length(publish),
        encode_buffer->buf + 1);

    type = LMQTT_TYPE_PUBLISH << 4;
    type |= publish->retain ? 0x01 : 0x00;
    type |= LMQTT_QOS_TO_PUBLISH_QOS(publish->qos);
    type |= publish->internal.encode_count > 0 ? 0x08 : 0x00;
    encode_buffer->buf[0] = type;
    encode_buffer->buf_len = 1 + v;
}

LMQTT_STATIC lmqtt_encode_result_t publish_encode_fixed_header(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_buffer_encode(encode_buffer, value,
        publish_build_fixed_header, offset, buf, buf_len, bytes_written);
}

LMQTT_STATIC lmqtt_encode_result_t publish_encode_topic(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    lmqtt_publish_t *publish = value->value;

    return string_encode(&publish->topic, 1, 1, offset, buf, buf_len,
        bytes_written, encode_buffer);
}

LMQTT_STATIC void publish_build_packet_id(lmqtt_store_value_t *value,
    lmqtt_encode_buffer_t *encode_buffer)
{
    int i;

    for (i = 0; i < LMQTT_PACKET_ID_SIZE; i++)
        encode_buffer->buf[i] = STRING_LEN_BYTE(value->packet_id,
            LMQTT_PACKET_ID_SIZE - i - 1);

    encode_buffer->buf_len = LMQTT_PACKET_ID_SIZE;
}

LMQTT_STATIC lmqtt_encode_result_t publish_encode_packet_id(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_buffer_encode(encode_buffer, value,
        publish_build_packet_id, offset, buf, buf_len, bytes_written);
}

LMQTT_STATIC lmqtt_encode_result_t publish_encode_payload(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    lmqtt_publish_t *publish = value->value;
    return string_encode(&publish->payload, 0, 0, offset, buf, buf_len,
        bytes_written, encode_buffer);
}

/******************************************************************************
 * lmqtt_publish_t PUBLIC functions
 ******************************************************************************/

int lmqtt_publish_validate(lmqtt_publish_t *publish)
{
    return string_validate_field_length(&publish->topic) &&
        publish->topic.len > 0 && IS_VALID_LMQTT_QOS(publish->qos) &&
        publish_calc_remaining_length(publish) <= 0xfffffff;
}

/******************************************************************************
 * (puback) PUBLIC functions
 ******************************************************************************/

LMQTT_STATIC lmqtt_encode_result_t puback_encode_ws_header(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    size_t ws_payload_len = calc_mqtt_packet_len(LMQTT_PACKET_ID_SIZE);
    return encode_ws_header(encode_buffer, buf, buf_len, ws_payload_len,
        bytes_written);
}

LMQTT_STATIC void puback_build(lmqtt_store_value_t *value,
    lmqtt_encode_buffer_t *encode_buffer)
{
    encode_buffer_encode_packet_id(encode_buffer,
        LMQTT_TYPE_PUBACK << 4, LMQTT_PACKET_ID_SIZE, value->packet_id);
}

LMQTT_STATIC lmqtt_encode_result_t puback_encode_fixed_header(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_buffer_encode(encode_buffer, value, &puback_build, offset,
        buf, buf_len, bytes_written);
}

/******************************************************************************
 * (pubrec) PUBLIC functions
 ******************************************************************************/

LMQTT_STATIC lmqtt_encode_result_t pubrec_encode_ws_header(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    size_t ws_payload_len = calc_mqtt_packet_len(LMQTT_PACKET_ID_SIZE);
    return encode_ws_header(encode_buffer, buf, buf_len, ws_payload_len,
        bytes_written);
}

LMQTT_STATIC void pubrec_build(lmqtt_store_value_t *value,
    lmqtt_encode_buffer_t *encode_buffer)
{
    encode_buffer_encode_packet_id(encode_buffer,
        LMQTT_TYPE_PUBREC << 4, LMQTT_PACKET_ID_SIZE, value->packet_id);
}

LMQTT_STATIC lmqtt_encode_result_t pubrec_encode_fixed_header(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_buffer_encode(encode_buffer, value, &pubrec_build, offset,
        buf, buf_len, bytes_written);
}

/******************************************************************************
 * (pubrel) PUBLIC functions
 ******************************************************************************/

LMQTT_STATIC lmqtt_encode_result_t pubrel_encode_ws_header(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    size_t ws_payload_len = calc_mqtt_packet_len(LMQTT_PACKET_ID_SIZE);
    return encode_ws_header(encode_buffer, buf, buf_len, ws_payload_len,
        bytes_written);
}

LMQTT_STATIC void pubrel_build(lmqtt_store_value_t *value,
    lmqtt_encode_buffer_t *encode_buffer)
{
    encode_buffer_encode_packet_id(encode_buffer, (LMQTT_TYPE_PUBREL << 4) |
        0x02, LMQTT_PACKET_ID_SIZE, value->packet_id);
}

LMQTT_STATIC lmqtt_encode_result_t pubrel_encode_fixed_header(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_buffer_encode(encode_buffer, value, &pubrel_build, offset,
        buf, buf_len, bytes_written);
}

/******************************************************************************
 * (pubcomp) PUBLIC functions
 ******************************************************************************/

LMQTT_STATIC lmqtt_encode_result_t pubcomp_encode_ws_header(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    size_t ws_payload_len = calc_mqtt_packet_len(LMQTT_PACKET_ID_SIZE);
    return encode_ws_header(encode_buffer, buf, buf_len, ws_payload_len,
        bytes_written);
}

LMQTT_STATIC void pubcomp_build(lmqtt_store_value_t *value,
    lmqtt_encode_buffer_t *encode_buffer)
{
    encode_buffer_encode_packet_id(encode_buffer, LMQTT_TYPE_PUBCOMP << 4,
        LMQTT_PACKET_ID_SIZE, value->packet_id);
}

LMQTT_STATIC lmqtt_encode_result_t pubcomp_encode_fixed_header(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_buffer_encode(encode_buffer, value, &pubcomp_build, offset,
        buf, buf_len, bytes_written);
}

/******************************************************************************
 * (pingreq) PUBLIC functions
 ******************************************************************************/

LMQTT_STATIC lmqtt_encode_result_t pingreq_encode_ws_header(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    size_t ws_payload_len = calc_mqtt_packet_len(2);
    return encode_ws_header(encode_buffer, buf, buf_len, ws_payload_len,
        bytes_written);
}

LMQTT_STATIC void pingreq_build(lmqtt_store_value_t *value,
    lmqtt_encode_buffer_t *encode_buffer)
{
    assert(sizeof(encode_buffer->buf) >= 2);

    encode_buffer->buf[0] = LMQTT_TYPE_PINGREQ << 4;
    encode_buffer->buf[1] = 0;
    encode_buffer->buf_len = 2;
}

LMQTT_STATIC lmqtt_encode_result_t pingreq_encode_fixed_header(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_buffer_encode(encode_buffer, value, pingreq_build, offset,
        buf, buf_len, bytes_written);
}

/******************************************************************************
 * (disconnect) PUBLIC functions
 ******************************************************************************/

LMQTT_STATIC lmqtt_encode_result_t disconnect_encode_ws_header(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    size_t ws_payload_len = calc_mqtt_packet_len(2);
    return encode_ws_header(encode_buffer, buf, buf_len, ws_payload_len,
        bytes_written);
}

LMQTT_STATIC void disconnect_build(lmqtt_store_value_t *value,
    lmqtt_encode_buffer_t *encode_buffer)
{
    assert(sizeof(encode_buffer->buf) >= 2);

    encode_buffer->buf[0] = LMQTT_TYPE_DISCONNECT << 4;
    encode_buffer->buf[1] = 0;
    encode_buffer->buf_len = 2;
}

LMQTT_STATIC lmqtt_encode_result_t disconnect_encode_fixed_header(
    lmqtt_store_value_t *value, lmqtt_encode_buffer_t *encode_buffer,
    size_t offset, unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    return encode_buffer_encode(encode_buffer, value, disconnect_build, offset,
        buf, buf_len, bytes_written);
}

/******************************************************************************
 * lmqtt_tx_buffer_t PRIVATE functions
 ******************************************************************************/

LMQTT_STATIC lmqtt_encoder_t tx_buffer_finder_ws_connect(
    lmqtt_tx_buffer_t *tx_buffer, lmqtt_store_value_t *value)
{
    switch (tx_buffer->internal.pos) {
        case 0: return &connect_encode_ws_hanshake_get;
        case 1: return &connect_encode_ws_hanshake_host;
        case 2: return &connect_encode_ws_hanshake_upgrade;
        case 3: return &connect_encode_ws_hanshake_conn_upgrade;
        case 4: return &connect_encode_ws_hanshake_origin;
        case 5: return &connect_encode_ws_hanshake_key;
        case 6: return &connect_encode_ws_hanshake_version;
        case 7: return &connect_encode_ws_hanshake_protocol;
        case 8: return &connect_encode_ws_hanshake_end;
    }
    return 0;
}

LMQTT_STATIC lmqtt_encoder_t tx_buffer_finder_connect(
    lmqtt_tx_buffer_t *tx_buffer, lmqtt_store_value_t *value)
{
    switch (tx_buffer->internal.pos) {
        case 0: return &connect_encode_ws_header;
        case 1: return &connect_encode_fixed_header;
        case 2: return &connect_encode_variable_header;
        case 3: return &connect_encode_payload_client_id;
        case 4: return &connect_encode_payload_will_topic;
        case 5: return &connect_encode_payload_will_message;
        case 6: return &connect_encode_payload_user_name;
        case 7: return &connect_encode_payload_password;
    }
    return 0;
}

LMQTT_STATIC lmqtt_encoder_t tx_buffer_finder_subscribe(
    lmqtt_tx_buffer_t *tx_buffer, lmqtt_store_value_t *value)
{
    lmqtt_subscribe_t *subscribe = value->value;
    int p = tx_buffer->internal.pos;

    if (p == 0) {
        return &subscribe_encode_ws_header_subscribe;
    } else if (p == 1) {
        subscribe->internal.current = 0;
        return &subscribe_encode_header_subscribe;
    }

    p -= 2;
    if (p >= 0 && p < subscribe->count * 2) {
        subscribe->internal.current = &subscribe->subscriptions[p / 2];
        return p % 2 == 0 ? &subscribe_encode_topic : &subscribe_encode_qos;
    }

    return 0;
}

LMQTT_STATIC lmqtt_encoder_t tx_buffer_finder_unsubscribe(
    lmqtt_tx_buffer_t *tx_buffer, lmqtt_store_value_t *value)
{
    lmqtt_subscribe_t *subscribe = value->value;
    int p = tx_buffer->internal.pos;
        return &subscribe_encode_ws_header_unsubscribe;
    if (p == 0) {

    } else if (p == 1) {
        subscribe->internal.current = 0;
        return &subscribe_encode_header_unsubscribe;
    }

    p -= 2;
    if (p >= 0 && p < subscribe->count) {
        subscribe->internal.current = &subscribe->subscriptions[p];
        return &subscribe_encode_topic;
    }

    return 0;
}

LMQTT_STATIC lmqtt_encoder_t tx_buffer_finder_publish(
    lmqtt_tx_buffer_t *tx_buffer, lmqtt_store_value_t *value)
{
    lmqtt_publish_t *publish = value->value;

    if (publish->qos == LMQTT_QOS_0) {
        switch (tx_buffer->internal.pos) {
            case 0: return &publish_encode_ws_header;
            case 1: return &publish_encode_fixed_header;
            case 2: return &publish_encode_topic;
            case 3: return &publish_encode_payload;
        }
    } else {
        switch (tx_buffer->internal.pos) {
            case 0: return &publish_encode_ws_header;
            case 1: return &publish_encode_fixed_header;
            case 2: return &publish_encode_topic;
            case 3: return &publish_encode_packet_id;
            case 4: return &publish_encode_payload;
        }
    }

    publish->internal.encode_count++;
    return 0;
}

LMQTT_STATIC lmqtt_encoder_t tx_buffer_finder_puback(
    lmqtt_tx_buffer_t *tx_buffer, lmqtt_store_value_t *value)
{
    switch (tx_buffer->internal.pos) {
        case 0: return &puback_encode_ws_header;
        case 1: return &puback_encode_fixed_header;
    }

    return 0;
}

LMQTT_STATIC lmqtt_encoder_t tx_buffer_finder_pubrec(
    lmqtt_tx_buffer_t *tx_buffer, lmqtt_store_value_t *value)
{
    switch (tx_buffer->internal.pos) {
        case 0: return &pubrec_encode_ws_header;
        case 1: return &pubrec_encode_fixed_header;
    }

    return 0;
}

LMQTT_STATIC lmqtt_encoder_t tx_buffer_finder_pubrel(
    lmqtt_tx_buffer_t *tx_buffer, lmqtt_store_value_t *value)
{
    switch (tx_buffer->internal.pos) {
        case 0: return &pubrel_encode_ws_header;
        case 1: return &pubrel_encode_fixed_header;
    }

    return 0;
}

LMQTT_STATIC lmqtt_encoder_t tx_buffer_finder_pubcomp(
    lmqtt_tx_buffer_t *tx_buffer, lmqtt_store_value_t *value)
{
    switch (tx_buffer->internal.pos) {
        case 0: return &pubcomp_encode_ws_header;
        case 1: return &pubcomp_encode_fixed_header;
    }

    return 0;
}

LMQTT_STATIC lmqtt_encoder_t tx_buffer_finder_pingreq(
    lmqtt_tx_buffer_t *tx_buffer, lmqtt_store_value_t *value)
{
    switch (tx_buffer->internal.pos) {
        case 0: return &pingreq_encode_ws_header;
        case 1: return &pingreq_encode_fixed_header;
    }

    return 0;
}

LMQTT_STATIC lmqtt_encoder_t tx_buffer_finder_disconnect(
    lmqtt_tx_buffer_t *tx_buffer, lmqtt_store_value_t *value)
{
    switch (tx_buffer->internal.pos) {
        case 0: return &disconnect_encode_ws_header;
        case 1: return &disconnect_encode_fixed_header;
    }

    return 0;
}

static lmqtt_encoder_finder_t tx_buffer_finder_by_kind_impl(
    lmqtt_kind_t kind)
{
    switch (kind) {
        case LMQTT_KIND_CONNECT: return &tx_buffer_finder_connect;
        case LMQTT_KIND_SUBSCRIBE: return &tx_buffer_finder_subscribe;
        case LMQTT_KIND_UNSUBSCRIBE: return &tx_buffer_finder_unsubscribe;
        case LMQTT_KIND_PUBLISH_0:
        case LMQTT_KIND_PUBLISH_1:
        case LMQTT_KIND_PUBLISH_2: return &tx_buffer_finder_publish;
        case LMQTT_KIND_PUBACK: return &tx_buffer_finder_puback;
        case LMQTT_KIND_PUBREC: return &tx_buffer_finder_pubrec;
        case LMQTT_KIND_PUBREL: return &tx_buffer_finder_pubrel;
        case LMQTT_KIND_PUBCOMP: return &tx_buffer_finder_pubcomp;
        case LMQTT_KIND_PINGREQ: return &tx_buffer_finder_pingreq;
        case LMQTT_KIND_DISCONNECT: return &tx_buffer_finder_disconnect;
        case LMQTT_KIND_WS_CONNECT: return &tx_buffer_finder_ws_connect;
    }
    return NULL;
}

/* Enable mocking of tx_buffer_finder_by_kind() */
LMQTT_STATIC lmqtt_encoder_finder_t (*tx_buffer_finder_by_kind)(
    lmqtt_kind_t) = &tx_buffer_finder_by_kind_impl;

LMQTT_STATIC lmqtt_io_result_t tx_buffer_fail(lmqtt_tx_buffer_t *state,
    lmqtt_error_t error, int os_error)
{
    state->internal.error = error;
    state->internal.os_error = os_error;
    return LMQTT_IO_ERROR;
}

/******************************************************************************
 * lmqtt_tx_buffer_t PUBLIC functions
 ******************************************************************************/

void lmqtt_tx_buffer_reset(lmqtt_tx_buffer_t *state)
{
    state->closed = 0;
    memset(&state->internal, 0, sizeof(state->internal));
}

void lmqtt_tx_buffer_finish(lmqtt_tx_buffer_t *state)
{
    state->closed = 1;
}

lmqtt_error_t lmqtt_tx_buffer_get_error_impl(lmqtt_tx_buffer_t *state,
    int *os_error)
{
    *os_error = state->internal.os_error;
    return state->internal.error;
}

/* Enable mocking of lmqtt_tx_buffer_get_error() */
lmqtt_error_t (*lmqtt_tx_buffer_get_error)(lmqtt_tx_buffer_t *, int *) =
    &lmqtt_tx_buffer_get_error_impl;

static lmqtt_io_result_t lmqtt_tx_buffer_encode_impl(lmqtt_tx_buffer_t *state,
    unsigned char *buf, size_t buf_len, size_t *bytes_written)
{
    size_t offset = 0;
    int kind;
    lmqtt_store_value_t value;
    *bytes_written = 0;

    if (state->internal.error)
        return LMQTT_IO_ERROR;

    while (!state->closed && lmqtt_store_peek(state->store, &kind, &value)) {
        lmqtt_encoder_finder_t finder = tx_buffer_finder_by_kind(kind);
        assert(finder);

        while (1) {
            int result;
            size_t cur_bytes;
            lmqtt_encoder_t encoder = finder(state, &value);

            if (!encoder) {
                /* We're done with that packet */
                if (!kind_expects_response(kind)) {
                    lmqtt_store_drop_current(state->store);

                    if (kind == LMQTT_KIND_DISCONNECT) {
                        lmqtt_tx_buffer_finish(state);
                        break;
                    } else if (value.callback &&
                            !value.callback(value.callback_data, value.value)) {
                        assert(kind == LMQTT_KIND_PUBLISH_0);
                        return tx_buffer_fail(state,
                            LMQTT_ERROR_CALLBACK_PUBLISH, 0);
                    }
                } else {
                    lmqtt_store_mark_current(state->store);
                }
                lmqtt_tx_buffer_reset(state);
                break;
            }

            /* Encode packet using current encoder */
            result = encoder(&value, &state->internal.buffer,
                state->internal.offset, buf + offset, buf_len - offset,
                &cur_bytes);
            if (result == LMQTT_ENCODE_WOULD_BLOCK)
                return LMQTT_IO_WOULD_BLOCK;
            if (result == LMQTT_ENCODE_CONTINUE)
                state->internal.offset += cur_bytes;
            if (result == LMQTT_ENCODE_CONTINUE || result == LMQTT_ENCODE_FINISHED)
                *bytes_written += cur_bytes;
            if (result == LMQTT_ENCODE_CONTINUE)
                return LMQTT_IO_SUCCESS;
            if (result == LMQTT_ENCODE_ERROR)
                return tx_buffer_fail(state, state->internal.buffer.error,
                    state->internal.buffer.os_error);

            offset += cur_bytes;
            state->internal.pos += 1;
            state->internal.offset = 0;
        }
    }

    return *bytes_written > 0 || state->closed ?
        LMQTT_IO_SUCCESS : LMQTT_IO_WOULD_BLOCK;
}

/* Enable mocking of lmqtt_tx_buffer_encode() */
lmqtt_io_result_t (*lmqtt_tx_buffer_encode)(lmqtt_tx_buffer_t *,
    unsigned char *, size_t, size_t *) = &lmqtt_tx_buffer_encode_impl;

lmqtt_string_t *lmqtt_tx_buffer_get_blocking_str(lmqtt_tx_buffer_t *state)
{
    return state->internal.buffer.blocking_str;
}

/******************************************************************************
 * lmqtt_rx_buffer_t PRIVATE functions
 ******************************************************************************/

static
lmqtt_message_on_publish_allocate_t rx_buffer_publish_part_topic_get_allocate(
    lmqtt_message_callbacks_t *message)
{
    return message->on_publish_allocate_topic;
}

static lmqtt_string_t *rx_buffer_publish_part_topic_get_string(
    lmqtt_rx_buffer_t *state)
{
    return &state->internal.publish.topic;
}

static
lmqtt_message_on_publish_allocate_t rx_buffer_publish_part_payload_get_allocate(
    lmqtt_message_callbacks_t *message)
{
    return message->on_publish_allocate_payload;
}

static lmqtt_string_t *rx_buffer_publish_part_payload_get_string(
    lmqtt_rx_buffer_t *state)
{
    return &state->internal.publish.payload;
}

struct _lmqtt_publish_part_t {
    lmqtt_message_on_publish_allocate_t (*get_allocate)(
        lmqtt_message_callbacks_t *);
    lmqtt_string_t *(*get_string)(lmqtt_rx_buffer_t *);
    lmqtt_error_t allocate_error;
    lmqtt_error_t string_error;
};

static struct _lmqtt_publish_part_t rx_buffer_publish_part_topic = {
    &rx_buffer_publish_part_topic_get_allocate,
    &rx_buffer_publish_part_topic_get_string,
    LMQTT_ERROR_DECODE_PUBLISH_TOPIC_ALLOCATE_FAILED,
    LMQTT_ERROR_DECODE_PUBLISH_TOPIC_WRITE_FAILED
};

static struct _lmqtt_publish_part_t rx_buffer_publish_part_payload = {
    &rx_buffer_publish_part_payload_get_allocate,
    &rx_buffer_publish_part_payload_get_string,
    LMQTT_ERROR_DECODE_PUBLISH_PAYLOAD_ALLOCATE_FAILED,
    LMQTT_ERROR_DECODE_PUBLISH_PAYLOAD_WRITE_FAILED
};

LMQTT_STATIC lmqtt_io_result_t rx_buffer_fail(lmqtt_rx_buffer_t *state,
    lmqtt_error_t error, int os_error)
{
    state->internal.error = error;
    state->internal.os_error = os_error;
    return LMQTT_IO_ERROR;
}

LMQTT_STATIC int rx_buffer_allocate_write(lmqtt_rx_buffer_t *state, long when,
    struct _lmqtt_publish_part_t *publish_part, size_t len,
    lmqtt_decode_bytes_t *bytes)
{
    const long rem_pos = state->internal.remain_buf_pos + 1;
    lmqtt_publish_t *publish = &state->internal.publish;
    lmqtt_message_callbacks_t *message = state->message_callbacks;
    /* We may receive a buffer longer than what should be written with
       string_write(), in the case of a topic followed by the packet id and
       payload, or a payload followed by data from other packets; therefore the
       actual value should be capped before continuing */
    size_t max_len = len - (rem_pos - when);
    size_t buf_len = bytes->buf_len > max_len ? max_len : bytes->buf_len;

    lmqtt_message_on_publish_allocate_t allocate =
        publish_part->get_allocate(message);
    lmqtt_string_t *str = publish_part->get_string(state);

    if (!state->internal.ignore_publish && rem_pos == when && allocate) {
        switch (allocate(message->on_publish_data, publish, len)) {
            case LMQTT_ALLOCATE_SUCCESS:
                state->internal.ignore_publish = 0;
                break;
            case LMQTT_ALLOCATE_IGNORE:
                state->internal.ignore_publish = 1;
                break;
            default:
                rx_buffer_fail(state, publish_part->allocate_error, 0);
                return 0;
        }
    }

    if (!state->internal.ignore_publish) {
        int os_error = 0;
        state->internal.blocking_str = NULL;
        switch (string_write(str, bytes->buf, buf_len, bytes->bytes_written,
                &os_error)) {
            case LMQTT_STRING_SUCCESS:
                return 1;
            case LMQTT_STRING_WOULD_BLOCK:
                state->internal.blocking_str = str;
                return 1;
            default:
                rx_buffer_fail(state, publish_part->string_error, os_error);
                return 0;
        }
    }
    *bytes->bytes_written += buf_len;
    return 1;
}

LMQTT_STATIC void rx_buffer_deallocate_publish(lmqtt_rx_buffer_t *state)
{
    lmqtt_message_callbacks_t *message = state->message_callbacks;

    if (!state->internal.ignore_publish && message->on_publish_deallocate)
        message->on_publish_deallocate(message->on_publish_data,
            &state->internal.publish);
}

LMQTT_STATIC lmqtt_decode_result_t rx_buffer_decode_connack(
    lmqtt_rx_buffer_t *state, lmqtt_decode_bytes_t *bytes)
{
    unsigned char b;
    lmqtt_connect_t *connect = (lmqtt_connect_t *) state->internal.value.value;

    assert(bytes->buf_len >= 1);
    b = bytes->buf[0];
    *bytes->bytes_written = 0;

    switch (state->internal.remain_buf_pos) {
        case 0:
            if (b & ~1) {
                rx_buffer_fail(state,
                    LMQTT_ERROR_DECODE_CONNACK_INVALID_ACKNOWLEDGE_FLAGS, 0);
                return LMQTT_DECODE_ERROR;
            }
            connect->response.session_present = b;
            *bytes->bytes_written += 1;
            return LMQTT_DECODE_CONTINUE;
        case 1:
            if (b > LMQTT_CONNACK_RETURN_CODE_MAX) {
                rx_buffer_fail(state,
                    LMQTT_ERROR_DECODE_CONNACK_INVALID_RETURN_CODE, 0);
                return LMQTT_DECODE_ERROR;
            } else if (b != 0) {
                rx_buffer_fail(state, LMQTT_ERROR_CONNACK_BASE + b, 0);
                *bytes->bytes_written += 1;
                return LMQTT_DECODE_ERROR;
            } else {
                *bytes->bytes_written += 1;
                return LMQTT_DECODE_FINISHED;
            }
        default:
            rx_buffer_fail(state, LMQTT_ERROR_DECODE_CONNACK_INVALID_LENGTH, 0);
            return LMQTT_DECODE_ERROR;
    }
}

LMQTT_STATIC lmqtt_decode_result_t rx_buffer_decode_publish(
    lmqtt_rx_buffer_t *state, lmqtt_decode_bytes_t *bytes)
{
    size_t *bytes_w;
    long rem_len = state->internal.header.remaining_length;
    long rem_pos = state->internal.remain_buf_pos + 1;
    lmqtt_store_value_t value;
    lmqtt_publish_t *publish = &state->internal.publish;
    lmqtt_message_callbacks_t *message = state->message_callbacks;
    lmqtt_qos_t qos = QOS_TO_LMQTT_QOS(state->internal.header.qos);
    static const long s_len = LMQTT_STRING_LEN_SIZE;
    long p_len = qos == LMQTT_QOS_0 ? 0 : LMQTT_PACKET_ID_SIZE;
    lmqtt_packet_id_t packet_id;

    assert(bytes->buf_len >= 1);
    bytes_w = bytes->bytes_written;
    *bytes_w = 0;

    if (rem_pos <= s_len) {
        state->internal.topic_len |= bytes->buf[0] << ((s_len - rem_pos) * 8);
        if (rem_pos == s_len && (state->internal.topic_len == 0 ||
                state->internal.topic_len + s_len + p_len > rem_len)) {
            rx_buffer_fail(state, LMQTT_ERROR_DECODE_PUBLISH_INVALID_LENGTH, 0);
            return LMQTT_DECODE_ERROR;
        }
        *bytes_w += 1;
    } else {
        long t_len = (long) state->internal.topic_len;
        long p_start = s_len + t_len;

        if (rem_pos == s_len + 1 && (!message->on_publish ||
                !message->on_publish_allocate_topic ||
                !message->on_publish_allocate_payload))
            state->internal.ignore_publish = 1;

        if (rem_pos <= p_start) {
            if (!rx_buffer_allocate_write(state, s_len + 1,
                    &rx_buffer_publish_part_topic, t_len, bytes)) {
                rx_buffer_deallocate_publish(state);
                return LMQTT_DECODE_ERROR;
            }
        } else if (rem_pos <= p_start + p_len) {
            state->internal.packet_id |= (bytes->buf[0] << ((p_len - rem_pos +
                    p_start) * 8));
            *bytes_w += 1;
        } else {
            if (!rx_buffer_allocate_write(state, p_start + p_len + 1,
                    &rx_buffer_publish_part_payload, rem_len - p_len - p_start,
                    bytes)) {
                rx_buffer_deallocate_publish(state);
                return LMQTT_DECODE_ERROR;
            }
        }
    }

    if (state->internal.blocking_str)
        return LMQTT_DECODE_WOULD_BLOCK;
    if (rem_len >= rem_pos + *bytes_w)
        return LMQTT_DECODE_CONTINUE;

    packet_id = state->internal.packet_id;

    if (qos != LMQTT_QOS_0) {
        memset(&value, 0, sizeof(value));
        value.packet_id = packet_id;
        lmqtt_store_append(state->store, qos == LMQTT_QOS_2 ?
            LMQTT_KIND_PUBREC : LMQTT_KIND_PUBACK, &value);
    }

    if (qos != LMQTT_QOS_2 || !lmqtt_id_set_contains(&state->id_set, packet_id)) {
        if (qos == LMQTT_QOS_2 && !lmqtt_id_set_put(&state->id_set, packet_id)) {
            rx_buffer_deallocate_publish(state);
            rx_buffer_fail(state, LMQTT_ERROR_DECODE_PUBLISH_ID_SET_FULL, 0);
            return LMQTT_DECODE_ERROR;
        }

        publish->qos = qos;
        publish->retain = state->internal.header.retain;

        if (!state->internal.ignore_publish && message->on_publish &&
                !message->on_publish(message->on_publish_data, publish)) {
            rx_buffer_deallocate_publish(state);
            rx_buffer_fail(state,
                LMQTT_ERROR_DECODE_PUBLISH_MESSAGE_CALLBACK_FAILED, 0);
            return LMQTT_DECODE_ERROR;
        }
    }

    rx_buffer_deallocate_publish(state);
    return LMQTT_DECODE_FINISHED;
}

LMQTT_STATIC lmqtt_decode_result_t rx_buffer_decode_suback(
    lmqtt_rx_buffer_t *state, lmqtt_decode_bytes_t *bytes)
{
    unsigned char b;
    lmqtt_subscribe_t *subscribe =
        (lmqtt_subscribe_t *) state->internal.value.value;
    long pos = state->internal.remain_buf_pos - LMQTT_PACKET_ID_SIZE;

    assert(bytes->buf_len >= 1);
    b = bytes->buf[0];
    *bytes->bytes_written = 0;

    if (pos == 0) {
        long len = state->internal.header.remaining_length -
            LMQTT_PACKET_ID_SIZE;
        if (len != (long) subscribe->count) {
            rx_buffer_fail(state, LMQTT_ERROR_DECODE_SUBACK_COUNT_MISMATCH, 0);
            return LMQTT_DECODE_ERROR;
        }
    }

    if (b > 2 && b != 0x80) {
        rx_buffer_fail(state, LMQTT_ERROR_DECODE_SUBACK_INVALID_RETURN_CODE, 0);
        return LMQTT_DECODE_ERROR;
    }

    subscribe->subscriptions[pos].return_code = b;
    *bytes->bytes_written += 1;
    return pos + 1 >= (long) subscribe->count ?
        LMQTT_DECODE_FINISHED : LMQTT_DECODE_CONTINUE;
}

/*
 * Return: 1 on success, 0 on failure
 */
static int rx_buffer_call_callback_impl(lmqtt_rx_buffer_t *state)
{
    lmqtt_store_value_t *value = &state->internal.value;

    if (value->callback)
        return value->callback(value->callback_data, value->value);

    return 1;
}

/* Enable mocking of rx_buffer_call_callback() */
LMQTT_STATIC int (*rx_buffer_call_callback)(
    lmqtt_rx_buffer_t *) = &rx_buffer_call_callback_impl;

static lmqtt_decode_result_t rx_buffer_decode_type_impl(
    lmqtt_rx_buffer_t *state, lmqtt_decode_bytes_t *bytes)
{
    if (!state->internal.decoder->decode_bytes) {
        *bytes->bytes_written = 0;
        rx_buffer_fail(state, LMQTT_ERROR_DECODE_NONZERO_REMAINING_LENGTH, 0);
        return LMQTT_DECODE_ERROR;
    }

    return state->internal.decoder->decode_bytes(state, bytes);
}

/* Enable mocking of rx_buffer_decode_type() */
LMQTT_STATIC lmqtt_decode_result_t (*rx_buffer_decode_type)(
    lmqtt_rx_buffer_t *, lmqtt_decode_bytes_t *) = &rx_buffer_decode_type_impl;

LMQTT_STATIC int rx_buffer_pop_packet(lmqtt_rx_buffer_t *state,
    lmqtt_packet_id_t packet_id)
{
    if (lmqtt_store_pop_marked_by(state->store,
            state->internal.decoder->kind, packet_id, &state->internal.value))
        return 1;

    rx_buffer_fail(state, LMQTT_ERROR_DECODE_NO_CORRESPONDING_REQUEST, 0);
    return 0;
}

LMQTT_STATIC int rx_buffer_is_packet_finished(lmqtt_rx_buffer_t *state)
{
    return state->internal.header_finished && state->internal.remain_buf_pos >=
        state->internal.header.remaining_length;
}

LMQTT_STATIC int rx_buffer_finish_packet(lmqtt_rx_buffer_t *state)
{
    if (state->internal.decoder->kind == LMQTT_KIND_PUBLISH_2) {
        /* in the case of a PUBREC this is going to be called imediatelly after
           removing the previous packet; therefore a failure in
           lmqtt_store_append() should not be possible */
        assert(lmqtt_store_append(state->store, LMQTT_KIND_PUBREL,
            &state->internal.value));
        lmqtt_rx_buffer_reset(state);
        return 1;
    }

    if (!rx_buffer_call_callback(state)) {
        lmqtt_error_t error = state->internal.decoder->callback_error;
        assert(error);
        rx_buffer_fail(state, error, 0);
        return 0;
    }

    lmqtt_rx_buffer_reset(state);
    return 1;
}

LMQTT_STATIC int rx_buffer_pop_packet_without_id(lmqtt_rx_buffer_t *state)
{
    return rx_buffer_pop_packet(state, 0);
}

LMQTT_STATIC int rx_buffer_pop_packet_with_id(lmqtt_rx_buffer_t *state)
{
    return rx_buffer_pop_packet(state, state->internal.packet_id);
}

LMQTT_STATIC int rx_buffer_pop_packet_ignore(lmqtt_rx_buffer_t *state)
{
    return 1;
}

LMQTT_STATIC int rx_buffer_pubrel(lmqtt_rx_buffer_t *state)
{
    static lmqtt_store_value_t value;
    lmqtt_packet_id_t packet_id = state->internal.packet_id;

    /* PUBCOMP should be always sent, even if the client has already released
       the message with the given packet id (see MQTT-4.3.3-2); so we ignore
       failed attempts to remove such packet from the queue */
    lmqtt_id_set_remove(&state->id_set, packet_id);

    memset(&value, 0, sizeof(value));
    value.packet_id = packet_id;
    if (lmqtt_store_append(state->store, LMQTT_KIND_PUBCOMP, &value))
        return 1;

    /* if the call to `lmqtt_id_set_remove` failed and the queue was full before
       that then `lmqtt_store_append` will also fail; there's nothing we can do
       in such case, other than signaling an error condition */
    rx_buffer_fail(state, LMQTT_ERROR_DECODE_PUBREL_ID_SET_FULL, 0);
    return 0;
}

LMQTT_STATIC lmqtt_decode_result_t rx_buffer_decode_remaining_without_id(
    lmqtt_rx_buffer_t *state, lmqtt_decode_bytes_t *bytes)
{
    int res = rx_buffer_decode_type(state, bytes);
    long rem_pos = state->internal.remain_buf_pos + *bytes->bytes_written;
    long rem_len = state->internal.header.remaining_length;

    if (res == LMQTT_DECODE_ERROR)
        return LMQTT_DECODE_ERROR;

    /* These conditions are guaranteed by either the `decode_bytes` callbacks
       or the minimum length check in `lmqtt_rx_buffer_decode` */
    assert((res != LMQTT_DECODE_FINISHED && rem_pos < rem_len) ||
        (res == LMQTT_DECODE_FINISHED && rem_pos == rem_len));

    return res;
}

LMQTT_STATIC lmqtt_decode_result_t rx_buffer_decode_remaining_with_id(
    lmqtt_rx_buffer_t *state, lmqtt_decode_bytes_t *bytes)
{
    long rem_pos = state->internal.remain_buf_pos + 1;
    static const long p_len = LMQTT_PACKET_ID_SIZE;

    if (rem_pos > p_len)
        return rx_buffer_decode_remaining_without_id(state, bytes);

    assert(bytes->buf_len >= 1);
    state->internal.packet_id |= bytes->buf[0] << ((p_len - rem_pos) * 8);
    *bytes->bytes_written = 1;

    if (rem_pos == p_len && !state->internal.decoder->pop_packet_with_id(state))
        return LMQTT_DECODE_ERROR;

    return LMQTT_DECODE_CONTINUE;
}

static const struct _lmqtt_rx_buffer_decoder_t rx_buffer_decoder_connack = {
    2,
    LMQTT_KIND_CONNECT,
    &rx_buffer_pop_packet_without_id,
    &rx_buffer_pop_packet_ignore,
    &rx_buffer_decode_remaining_without_id,
    &rx_buffer_decode_connack,
    LMQTT_ERROR_CALLBACK_CONNACK
};
static const struct _lmqtt_rx_buffer_decoder_t rx_buffer_decoder_publish = {
    3, /* this should be 3 or 5 depending on QoS, but rx_buffer_decode_publish()
          will also validate it */
    0, /* never used */
    &rx_buffer_pop_packet_ignore,
    &rx_buffer_pop_packet_ignore,
    &rx_buffer_decode_remaining_without_id,
    &rx_buffer_decode_publish,
    0
};
static const struct _lmqtt_rx_buffer_decoder_t rx_buffer_decoder_puback = {
    2,
    LMQTT_KIND_PUBLISH_1,
    &rx_buffer_pop_packet_ignore,
    &rx_buffer_pop_packet_with_id,
    &rx_buffer_decode_remaining_with_id,
    NULL,
    LMQTT_ERROR_CALLBACK_PUBLISH
};
static const struct _lmqtt_rx_buffer_decoder_t rx_buffer_decoder_pubrec = {
    2,
    LMQTT_KIND_PUBLISH_2,
    &rx_buffer_pop_packet_ignore,
    &rx_buffer_pop_packet_with_id,
    &rx_buffer_decode_remaining_with_id,
    NULL,
    0
};
static const struct _lmqtt_rx_buffer_decoder_t rx_buffer_decoder_pubrel = {
    2,
    0, /* never used */
    &rx_buffer_pop_packet_ignore,
    &rx_buffer_pubrel,
    &rx_buffer_decode_remaining_with_id,
    NULL,
    0
};
static const struct _lmqtt_rx_buffer_decoder_t rx_buffer_decoder_pubcomp = {
    2,
    LMQTT_KIND_PUBREL,
    &rx_buffer_pop_packet_ignore,
    &rx_buffer_pop_packet_with_id,
    &rx_buffer_decode_remaining_with_id,
    NULL,
    LMQTT_ERROR_CALLBACK_PUBLISH
};
static const struct _lmqtt_rx_buffer_decoder_t rx_buffer_decoder_suback = {
    3,
    LMQTT_KIND_SUBSCRIBE,
    &rx_buffer_pop_packet_ignore,
    &rx_buffer_pop_packet_with_id,
    &rx_buffer_decode_remaining_with_id,
    &rx_buffer_decode_suback,
    LMQTT_ERROR_CALLBACK_SUBACK
};
static const struct _lmqtt_rx_buffer_decoder_t rx_buffer_decoder_unsuback = {
    2,
    LMQTT_KIND_UNSUBSCRIBE,
    &rx_buffer_pop_packet_ignore,
    &rx_buffer_pop_packet_with_id,
    &rx_buffer_decode_remaining_with_id,
    NULL,
    LMQTT_ERROR_CALLBACK_UNSUBACK
};
static const struct _lmqtt_rx_buffer_decoder_t rx_buffer_decoder_pingresp = {
    0,
    LMQTT_KIND_PINGREQ,
    &rx_buffer_pop_packet_without_id,
    &rx_buffer_pop_packet_ignore,
    &rx_buffer_decode_remaining_without_id,
    NULL,
    0
};

static struct _lmqtt_rx_buffer_decoder_t const
        *rx_buffer_decoders[LMQTT_TYPE_MAX + 1] = {
    NULL,   /* 0 */
    NULL,   /* CONNECT */
    &rx_buffer_decoder_connack,
    &rx_buffer_decoder_publish,
    &rx_buffer_decoder_puback,
    &rx_buffer_decoder_pubrec,
    &rx_buffer_decoder_pubrel,
    &rx_buffer_decoder_pubcomp,
    NULL,   /* SUBSCRIBE */
    &rx_buffer_decoder_suback,
    NULL,   /* UNSUBSCRIBE */
    &rx_buffer_decoder_unsuback,
    NULL,   /* PINGREQ */
    &rx_buffer_decoder_pingresp,
    NULL    /* DISCONNECT */
};

/******************************************************************************
 * lmqtt_rx_buffer_t PUBLIC functions
 ******************************************************************************/

void lmqtt_rx_buffer_reset(lmqtt_rx_buffer_t *state)
{
    memset(&state->internal, 0, sizeof(state->internal));
}

void lmqtt_rx_buffer_finish(lmqtt_rx_buffer_t *state)
{
    rx_buffer_call_callback(state);
}

lmqtt_error_t lmqtt_rx_buffer_get_error_impl(lmqtt_rx_buffer_t *state,
    int *os_error)
{
    *os_error = state->internal.os_error;
    return state->internal.error;
}

/* Enable mocking of lmqtt_rx_buffer_get_error() */
lmqtt_error_t (*lmqtt_rx_buffer_get_error)(lmqtt_rx_buffer_t *, int *) =
    &lmqtt_rx_buffer_get_error_impl;

static lmqtt_io_result_t lmqtt_rx_buffer_decode_impl(lmqtt_rx_buffer_t *state,
    unsigned char *buf, size_t buf_len, size_t *bytes_read)
{
    int i = 0;
    *bytes_read = 0;

    if (state->internal.error)
        return LMQTT_IO_ERROR;

    while (i < buf_len) {
        if(state->ws_enabled && !state->ws_handshake_finished) {
            assert(state->ws_handshake_buffer != NULL);
            state->ws_handshake_buffer[state->internal.ws_handshake_pos++] = buf[i];
            if(state->internal.ws_handshake_pos >= state->ws_handshake_buffer_size) {
                return rx_buffer_fail(state, LMQTT_ERROR_WS_HANDSHAKE_LINE_TOO_LONG, 0);
            } else if(buf[i] == '\n') {
                /* Allow strcmp */
                state->ws_handshake_buffer[state->internal.ws_handshake_pos] = '\0';

                /* Compare with significant response lines */
                static const char *key_response_start = "Sec-WebSocket-Accept: ";
                if(!strcmp("HTTP/1.1 101 Switching Protocols\r\n", state->ws_handshake_buffer)) {
                    state->internal.ws_handshake_was_http_ok = 1;
                } else if(!strcmp("\r\n", state->ws_handshake_buffer)) {
                    if(!state->internal.ws_handshake_was_http_ok ||
                            !state->internal.ws_handshake_was_key_reply) {
                        return rx_buffer_fail(state,
                            LMQTT_ERROR_WS_HANDSHAKE_INCOMPLETE_REPLY, 0);
                    }
                    state->ws_handshake_finished = 1;
                    state->internal.value.callback(
                        state->internal.value.callback_data,
                        state->internal.value.value
                    );
                } else if(!memcmp(key_response_start, state->ws_handshake_buffer,
                        strlen(key_response_start))) {
                    lmqtt_store_pop_marked_by(state->store,
                        LMQTT_KIND_WS_CONNECT, 0, &state->internal.value);
                    lmqtt_connect_t *connect = state->internal.value.value;
                    char *key_start = &state->ws_handshake_buffer[strlen(key_response_start)];
                    size_t key_len = strlen(key_start) - strlen("\r\n");
                    if(connect->websocket_key_response.len != key_len || memcmp(
                            key_start, connect->websocket_key_response.buf,
                            key_len)) {
                        return rx_buffer_fail(state,
                            LMQTT_ERROR_WS_HANDSHAKE_INVALID_RESPONSE_KEY, 0);
                    }
                    state->internal.ws_handshake_was_key_reply = 1;
                }
                state->internal.ws_handshake_pos = 0;
            }
            i += 1;
            *bytes_read += 1;
            continue;
        } else if (state->ws_enabled && !state->internal.ws_header_finished) {
            lmqtt_error_t error;
            int res = websocket_header_decode(&state->internal.ws_header,
                buf[i], &error);

            if (res == LMQTT_DECODE_ERROR)
                return rx_buffer_fail(state, error, 0);

            i += 1;
            *bytes_read += 1;
            if (res != LMQTT_DECODE_FINISHED)
                continue;

            /* Now move further */
            state->internal.ws_header_finished = 1;
        } else if (state->ws_enabled && state->internal.ws_header_finished &&
                state->internal.ws_header.type != 0x2) {
            /* Handle special websocket frames */
            if(state->internal.ws_header.type == 8) {
                /* Connection closed by server */
                return rx_buffer_fail(state,
                    LMQTT_ERROR_WS_CONNECTION_CLOSED_BY_SERVER, 0);
            } else if(state->internal.ws_header.type == 9) {
                /* TODO: ping */
                /* Ping may contain payload which must be repeated in pong */
                return rx_buffer_fail(state,
                    LMQTT_ERROR_WS_UNSUPPORTED_FRAME_TYPE, 0);
            } else if(state->internal.ws_header.type == 10)
                return rx_buffer_fail(state, /* Pong - shouldn't happen */
                    LMQTT_ERROR_WS_UNSUPPORTED_FRAME_TYPE, 0);
        } else if (!state->internal.header_finished) {
            /* decode MQTT packet header byte by byte until decoded*/
            long rem_len;
            lmqtt_error_t error;

            int res = fixed_header_decode(&state->internal.header, buf[i],
                &error);

            if (res == LMQTT_DECODE_ERROR)
                return rx_buffer_fail(state, error, 0);

            i += 1;
            *bytes_read += 1;
            if (res != LMQTT_DECODE_FINISHED)
                continue;

            /* MQTT header processing done! Now we know which decoder to use */
            state->internal.header_finished = 1;
            state->internal.decoder =
                rx_buffer_decoders[state->internal.header.type];
            rem_len = state->internal.header.remaining_length;

            if (!state->internal.decoder)
                return rx_buffer_fail(state,
                    LMQTT_ERROR_DECODE_FIXED_HEADER_SERVER_SPECIFIC, 0);

            if (rem_len < state->internal.decoder->min_length)
                return rx_buffer_fail(state,
                    LMQTT_ERROR_DECODE_RESPONSE_TOO_SHORT, 0);

            if (!state->internal.decoder->pop_packet_without_id(state)) {
                assert(state->internal.error);
                return LMQTT_IO_ERROR;
            }
        } else {
            lmqtt_decode_result_t res;
            lmqtt_decode_bytes_t bytes;
            size_t cnt = 0;
            bytes.buf_len = buf_len - i;
            bytes.buf = &buf[i];
            bytes.bytes_written = &cnt;

            res = state->internal.decoder->decode_remaining(state, &bytes);
            if (res == LMQTT_DECODE_FINISHED || res == LMQTT_DECODE_CONTINUE) {
                i += cnt;
                *bytes_read += cnt;
                state->internal.remain_buf_pos += cnt;
            } else if (res == LMQTT_DECODE_WOULD_BLOCK) {
                assert(*bytes.bytes_written == 0);
                break;
            } else {
                assert(state->internal.error);
                return LMQTT_IO_ERROR;
            }
        }

        if (rx_buffer_is_packet_finished(state) &&
                !rx_buffer_finish_packet(state))
            return LMQTT_IO_ERROR;
    }

    if (*bytes_read > 0) {
        /* If decode_remaining() returns WOULD_BLOCK after we have successfully
           decoded other bytes we should not signal that some string is
           blocking, and instead wait until the decoder is called again */
        state->internal.blocking_str = NULL;
        return LMQTT_IO_SUCCESS;
    } else if (buf_len == 0) {
        return LMQTT_IO_SUCCESS;
    } else {
        return LMQTT_IO_WOULD_BLOCK;
    }
}

/* Enable mocking of lmqtt_rx_buffer_decode() */
lmqtt_io_result_t (*lmqtt_rx_buffer_decode)(lmqtt_rx_buffer_t *,
    unsigned char *, size_t, size_t *) = &lmqtt_rx_buffer_decode_impl;

lmqtt_string_t *lmqtt_rx_buffer_get_blocking_str(lmqtt_rx_buffer_t *state)
{
    return state->internal.blocking_str;
}
