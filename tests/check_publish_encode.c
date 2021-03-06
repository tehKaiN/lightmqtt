#include "check_lightmqtt.h"

#define PREPARE \
    int res; \
    size_t bytes_w = 0xcccccccc; \
    unsigned char buf[256]; \
    lmqtt_publish_t publish; \
    lmqtt_store_value_t value; \
    lmqtt_encode_buffer_t encode_buffer; \
    char *topic; \
    memset(&buf, 0xcc, sizeof(buf)); \
    memset(&publish, 0, sizeof(publish)); \
    memset(&value, 0, sizeof(value)); \
    memset(&encode_buffer, 0, sizeof(encode_buffer)); \
    publish.qos = LMQTT_QOS_1; \
    value.value = &publish

#define INIT_TOPIC(val) \
    do { \
        publish.topic.buf = val; \
        publish.topic.len = strlen(val); \
    } while (0)

START_TEST(should_encode_fixed_header_with_empty_payload_and_qos_0)
{
    PREPARE;
    INIT_TOPIC("x");
    publish.qos = LMQTT_QOS_0;

    publish_build_fixed_header(&value, &encode_buffer);

    ck_assert_uint_eq(2,    encode_buffer.buf_len);
    ck_assert_uint_eq(0x30, encode_buffer.buf[0]);
    ck_assert_uint_eq(3,    encode_buffer.buf[1]);
}
END_TEST

START_TEST(should_encode_fixed_header_with_empty_payload_and_qos_2)
{
    PREPARE;
    INIT_TOPIC("x");
    publish.qos = LMQTT_QOS_2;

    publish_build_fixed_header(&value, &encode_buffer);

    ck_assert_uint_eq(2,    encode_buffer.buf_len);
    ck_assert_uint_eq(0x34, encode_buffer.buf[0]);
    ck_assert_uint_eq(5,    encode_buffer.buf[1]);
}
END_TEST

START_TEST(should_encode_fixed_header_with_large_payload)
{
    PREPARE;
    INIT_TOPIC("x");

    publish.payload.len = 2097152 - 5;

    publish_build_fixed_header(&value, &encode_buffer);

    ck_assert_uint_eq(5,    encode_buffer.buf_len);
    ck_assert_uint_eq(0x32, encode_buffer.buf[0]);
    ck_assert_uint_eq(0x80, encode_buffer.buf[1]);
    ck_assert_uint_eq(0x80, encode_buffer.buf[2]);
    ck_assert_uint_eq(0x80, encode_buffer.buf[3]);
    ck_assert_uint_eq(0x01, encode_buffer.buf[4]);
}
END_TEST

START_TEST(should_encode_retain_flag)
{
    PREPARE;
    INIT_TOPIC("x");

    publish.retain = 1;

    publish_build_fixed_header(&value, &encode_buffer);

    ck_assert_uint_eq(0x33, encode_buffer.buf[0]);
}
END_TEST

START_TEST(should_encode_qos)
{
    PREPARE;
    INIT_TOPIC("x");

    publish.qos = LMQTT_QOS_2;

    publish_build_fixed_header(&value, &encode_buffer);

    ck_assert_uint_eq(0x34, encode_buffer.buf[0]);
}
END_TEST

START_TEST(should_encode_dup)
{
    PREPARE;
    INIT_TOPIC("x");

    publish.internal.encode_count++;

    publish_build_fixed_header(&value, &encode_buffer);

    ck_assert_uint_eq(0x3a, encode_buffer.buf[0]);
}
END_TEST

START_TEST(should_encode_topic)
{
    PREPARE;
    INIT_TOPIC("abcd");

    res = publish_encode_topic(&value, &encode_buffer, 0, buf, sizeof(buf),
        &bytes_w);
    ck_assert_int_eq(LMQTT_ENCODE_FINISHED, res);
    ck_assert_int_eq(6, bytes_w);
    ck_assert_uint_eq(0, buf[0]);
    ck_assert_uint_eq(4, buf[1]);
    ck_assert_uint_eq((unsigned char) 'a', buf[2]);
    ck_assert_uint_eq((unsigned char) 'd', buf[5]);
}
END_TEST

START_TEST(should_encode_packet_id)
{
    PREPARE;

    value.packet_id = 0x0506;

    res = publish_encode_packet_id(&value, &encode_buffer, 0, buf,
        sizeof(buf), &bytes_w);
    ck_assert_int_eq(LMQTT_ENCODE_FINISHED, res);
    ck_assert_int_eq(2, bytes_w);
    ck_assert_uint_eq(0x05, buf[0]);
    ck_assert_uint_eq(0x06, buf[1]);
}
END_TEST

START_TEST(should_encode_payload)
{
    PREPARE;

    publish.payload.buf = "payload";
    publish.payload.len = strlen(publish.payload.buf);

    res = publish_encode_payload(&value, &encode_buffer, 0, buf,
        sizeof(buf), &bytes_w);
    ck_assert_int_eq(LMQTT_ENCODE_FINISHED, res);
    ck_assert_int_eq(7, bytes_w);
    ck_assert_uint_eq((unsigned char) 'p', buf[0]);
    ck_assert_uint_eq((unsigned char) 'd', buf[6]);
}
END_TEST

START_TEST(should_encode_empty_payload)
{
    PREPARE;

    res = publish_encode_payload(&value, &encode_buffer, 0, buf,
        sizeof(buf), &bytes_w);
    ck_assert_int_eq(LMQTT_ENCODE_FINISHED, res);
    ck_assert_int_eq(0, bytes_w);
}
END_TEST

START_TEST(should_encode_payload_from_offset)
{
    PREPARE;

    publish.payload.buf = "payload";
    publish.payload.len = strlen(publish.payload.buf);
    publish.payload.internal.pos = 5;

    res = publish_encode_payload(&value, &encode_buffer, 5, buf,
        sizeof(buf), &bytes_w);
    ck_assert_int_eq(LMQTT_ENCODE_FINISHED, res);
    ck_assert_int_eq(2, bytes_w);
    ck_assert_uint_eq((unsigned char) 'a', buf[0]);
    ck_assert_uint_eq((unsigned char) 'd', buf[1]);
}
END_TEST

START_TCASE("Publish encode")
{
    ADD_TEST(should_encode_fixed_header_with_empty_payload_and_qos_0);
    ADD_TEST(should_encode_fixed_header_with_empty_payload_and_qos_2);
    ADD_TEST(should_encode_fixed_header_with_large_payload);
    ADD_TEST(should_encode_retain_flag);
    ADD_TEST(should_encode_qos);
    ADD_TEST(should_encode_dup);
    ADD_TEST(should_encode_topic);
    ADD_TEST(should_encode_packet_id);
    ADD_TEST(should_encode_payload);
    ADD_TEST(should_encode_empty_payload);
    ADD_TEST(should_encode_payload_from_offset);
}
END_TCASE
