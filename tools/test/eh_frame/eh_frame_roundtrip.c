/* Round-trip + byte-layout conformance test for the esp-hosted frame layer.
 *
 * Compiles the *upstream* eh_frame.c unchanged (only esp_err.h / linux/types.h /
 * sdkconfig.h are shimmed) and checks two things at once:
 *   1. encode -> decode round-trips the fields we care about, and
 *   2. the documented V1 wire layout really is what the encoder emits.
 */
#include <stdio.h>
#include <string.h>
#include "eh_frame.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("  FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    eh_frame_cfg_t cfg = EH_FRAME_CFG_HOST_MCU_SDIO_DEFAULT;
    uint8_t buf[256];
    uint8_t payload[8] = { 'P','4','-','C','6','!','!','!' };
    interface_buffer_handle_t in, out;
    uint8_t hdrlen;

    if (eh_frame_init(&cfg) != ESP_OK) { printf("init failed\n"); return 1; }
    printf("hdr_version=%u checksum_enabled=%d hdr_size=%u\n",
           cfg.hdr_version, eh_frame_checksum_enabled(), eh_frame_hdr_size());

    memset(buf, 0, sizeof(buf));
    memset(&in, 0, sizeof(in));
    in.if_type = 1; in.if_num = 2; in.flags = 0x40;
    in.pkt_type = 0x05; in.seq_num = 0x1234; in.throttle_cmd = 3;

    /* The payload must already sit at buf+hdr_len when encode() runs:
     * encode_v1() computes the checksum over header + payload_len, so encoding
     * into an otherwise-empty buffer would checksum a payload that is not there
     * yet and every frame would fail its check on the far side. */
    hdrlen = eh_frame_hdr_size();
    memcpy(buf + hdrlen, payload, sizeof(payload));
    hdrlen = eh_frame_encode(buf, &in, sizeof(payload));
    printf("encoded hdr_len=%u\n", hdrlen);

    CHECK(hdrlen == 12, "header must be 12 bytes, got %u", hdrlen);

    /* Documented V1 layout: if_type:4|if_num:4, flags, len(le16), offset(le16),
     * checksum(le16), seq_num(le16), throttle:2|rsvd:6, union byte. */
    CHECK((buf[0] & 0x0f) == 1, "byte0 low nibble = if_type");
    CHECK((buf[0] >> 4) == 2, "byte0 high nibble = if_num");
    CHECK(buf[1] == 0x40, "byte1 = flags");
    CHECK(buf[2] == 8 && buf[3] == 0, "byte2..3 = len(le16)=8");
    CHECK(buf[4] == 12 && buf[5] == 0, "byte4..5 = offset(le16)=hdr_len");
    CHECK(buf[8] == 0x34 && buf[9] == 0x12, "byte8..9 = seq_num(le16)");
    CHECK((buf[10] & 0x03) == 3, "byte10 low2 = throttle_cmd");
    CHECK(buf[11] == 0x05, "byte11 = union byte (pkt_type)");

    /* checksum covers the whole frame incl. header */
    {
        /* The checksum is computed with its own field zeroed: encode_v1() runs
         * the sum while buf[6..7] are still 0 (the header was just memset) and
         * only then stores the result. A verifier must do the same, otherwise
         * it is summing the value it is trying to check. */
        uint16_t stored = (uint16_t)(buf[6] | (buf[7] << 8));
        uint16_t calc;
        buf[6] = 0; buf[7] = 0;
        calc = eh_frame_checksum(buf, (uint16_t)(hdrlen + sizeof(payload)));
        buf[6] = (uint8_t)(stored & 0xff);
        buf[7] = (uint8_t)(stored >> 8);
        printf("checksum stored=0x%04x computed(field zeroed)=0x%04x\n", stored, calc);
        CHECK(stored == calc, "checksum computed with its field zeroed");
        CHECK(stored != 0, "checksum is non-zero");
    }

    memset(&out, 0, sizeof(out));
    {
        eh_frame_result_t r = eh_frame_decode(buf, (uint16_t)(hdrlen + sizeof(payload)), &out);
        printf("decode=%d payload_len=%u seq=0x%04x if_type=%u throttle=%u\n",
               r, out.payload_len, out.seq_num, out.if_type, out.throttle_cmd);
        CHECK(r == EH_FRAME_OK, "decode returns OK, got %d", r);
        CHECK(out.payload_len == sizeof(payload), "payload_len round-trips");
        CHECK(out.seq_num == 0x1234, "seq_num round-trips");
        CHECK(out.if_type == 1, "if_type round-trips");
        CHECK(out.throttle_cmd == 3, "throttle_cmd round-trips");
        CHECK(out.payload && memcmp(out.payload, payload, sizeof(payload)) == 0,
              "payload bytes survive");
    }

    /* corruption must be detected, not silently accepted */
    buf[14] ^= 0xff;
    memset(&out, 0, sizeof(out));
    {
        eh_frame_result_t r = eh_frame_decode(buf, (uint16_t)(hdrlen + sizeof(payload)), &out);
        printf("corrupted decode=%d\n", r);
        CHECK(r == EH_FRAME_CORRUPT, "corrupted payload -> EH_FRAME_CORRUPT, got %d", r);
    }

    printf(fails ? "\nRESULT: %d check(s) FAILED\n" : "\nRESULT: all checks passed\n", fails);
    return fails != 0;
}
