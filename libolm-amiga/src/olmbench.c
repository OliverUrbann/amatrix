/* olmbench: times the libolm operations a Matrix client needs, so we know
 * whether E2EE is fast enough on a given Amiga.
 *
 *   olmbench [scale]     scale multiplies the iteration counts (default 1)
 *
 * Everything runs through the public C API (olm.h), exactly as a client
 * would use it.  Random bytes come from a simple PRNG: this is a benchmark,
 * not a key generator.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include "olm/olm.h"
#include "olm/outbound_group_session.h"
#include "olm/inbound_group_session.h"
#include "olm/sas.h"

#ifdef __amigaos__
unsigned long __stack = 128 * 1024;
#endif

static uint32_t rng_state = 0x12345678;
static void fill_random(void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n--) {
        rng_state = rng_state * 1103515245u + 12345u;
        *p++ = (uint8_t)(rng_state >> 16);
    }
}

static void *rnd(size_t n)
{
    void *p = malloc(n ? n : 1);
    fill_random(p, n);
    return p;
}

static int failures = 0;
static void check(size_t r, const char *what, const char *err)
{
    if (r == olm_error()) {
        printf("  FAIL %s: %s\n", what, err);
        failures++;
    }
}

static clock_t t0;
static void start(void) { t0 = clock(); }
static void stop(const char *name, int n)
{
    double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    if (s <= 0) s = 1.0 / CLOCKS_PER_SEC;
    printf("  %-34s %6d x  %8.2f ms each\n", name, n, s * 1000.0 / n);
    fflush(stdout);
}

static OlmAccount *new_account(void)
{
    OlmAccount *a = olm_account(malloc(olm_account_size()));
    size_t n = olm_create_account_random_length(a);
    void *r = rnd(n);
    check(olm_create_account(a, r, n), "create_account", olm_account_last_error(a));
    free(r);
    return a;
}

int main(int argc, char **argv)
{
    int scale = argc > 1 ? atoi(argv[1]) : 1;
    int i;
    uint8_t major, minor, patch;
    if (scale < 1) scale = 1;

    olm_get_library_version(&major, &minor, &patch);
    printf("olmbench: libolm %d.%d.%d, scale %d, CLOCKS_PER_SEC %ld\n",
           major, minor, patch, scale, (long)CLOCKS_PER_SEC);

    /* --- account: Curve25519 + Ed25519 key generation --------------------- */
    printf("Account / keys\n");
    {
        int n = 5 * scale;
        OlmAccount *a = NULL;
        start();
        for (i = 0; i < n; i++) {
            if (a) { olm_clear_account(a); free(a); }
            a = new_account();
        }
        stop("create account (2 keypairs)", n);

        n = 20 * scale;
        size_t r_len = olm_account_generate_one_time_keys_random_length(a, n);
        void *r = rnd(r_len);
        start();
        check(olm_account_generate_one_time_keys(a, n, r, r_len), "gen otk",
              olm_account_last_error(a));
        stop("one-time key (Curve25519)", n);
        free(r);

        const char *msg = "{\"algorithms\":[\"m.olm.v1.curve25519-aes-sha2\"],\"device_id\":\"AMIGA\"}";
        size_t sig_len = olm_account_signature_length(a);
        char *sig = malloc(sig_len + 1);
        n = 20 * scale;
        start();
        for (i = 0; i < n; i++)
            check(olm_account_sign(a, msg, strlen(msg), sig, sig_len), "sign",
                  olm_account_last_error(a));
        stop("Ed25519 sign", n);

        /* verify with the account's own ed25519 key */
        size_t ik_len = olm_account_identity_keys_length(a);
        char *ik = malloc(ik_len + 1);
        olm_account_identity_keys(a, ik, ik_len);
        ik[ik_len] = 0;
        char *ed = strstr(ik, "\"ed25519\":\"");
        char edkey[64] = {0};
        if (ed) { ed += 11; memcpy(edkey, ed, 43); }
        OlmUtility *u = olm_utility(malloc(olm_utility_size()));
        char *sigcopy = malloc(sig_len);
        start();
        for (i = 0; i < n; i++) {
            memcpy(sigcopy, sig, sig_len); /* verify may scribble on it */
            check(olm_ed25519_verify(u, edkey, 43, msg, strlen(msg), sigcopy, sig_len),
                  "verify", olm_utility_last_error(u));
        }
        stop("Ed25519 verify (device keys)", n);

        n = 50 * scale;
        char hash[64];
        size_t big_len = 4096;
        char *big = rnd(big_len);
        start();
        for (i = 0; i < n; i++)
            olm_sha256(u, big, big_len, hash, olm_sha256_length(u));
        stop("SHA-256 of 4 KB", n);

        free(big); free(sigcopy); free(ik); free(sig);
        olm_clear_utility(u); free(u);
        olm_clear_account(a); free(a);
    }

    /* --- Olm: 1:1 sessions between devices (room-key delivery) ------------- */
    printf("Olm (to-device, room key delivery)\n");
    {
        OlmAccount *alice = new_account(), *bob = new_account();
        size_t r_len = olm_account_generate_one_time_keys_random_length(bob, 1);
        void *r = rnd(r_len);
        olm_account_generate_one_time_keys(bob, 1, r, r_len);
        free(r);

        char bob_ik[256], bob_otk[256];
        size_t l = olm_account_identity_keys(bob, bob_ik, sizeof bob_ik);
        bob_ik[l] = 0;
        l = olm_account_one_time_keys(bob, bob_otk, sizeof bob_otk);
        bob_otk[l] = 0;
        char bob_curve[64] = {0}, otk[64] = {0};
        memcpy(bob_curve, strstr(bob_ik, "\"curve25519\":\"") + 14, 43);
        memcpy(otk, strstr(bob_otk, "\"AAAAAQ\":\"") + 10, 43);

        OlmSession *as = olm_session(malloc(olm_session_size()));
        r_len = olm_create_outbound_session_random_length(as);
        r = rnd(r_len);
        start();
        check(olm_create_outbound_session(as, alice, bob_curve, 43, otk, 43, r, r_len),
              "outbound session", olm_session_last_error(as));
        stop("create outbound session (3xDH)", 1);
        free(r);

        /* a typical m.room_key payload is ~300 bytes */
        char plain[320];
        memset(plain, 'k', sizeof plain);
        size_t ct_len = olm_encrypt_message_length(as, sizeof plain);
        char *ct = malloc(ct_len + 1);
        r_len = olm_encrypt_random_length(as);
        r = rnd(r_len);
        check(olm_encrypt(as, plain, sizeof plain, r, r_len, ct, ct_len), "encrypt",
              olm_session_last_error(as));
        free(r);

        OlmSession *bs = olm_session(malloc(olm_session_size()));
        char *tmp = malloc(ct_len);
        memcpy(tmp, ct, ct_len);
        start();
        check(olm_create_inbound_session(bs, bob, tmp, ct_len), "inbound session",
              olm_session_last_error(bs));
        stop("create inbound session (3xDH)", 1);
        char out[512];
        memcpy(tmp, ct, ct_len);
        size_t pl = olm_decrypt(bs, OLM_MESSAGE_TYPE_PRE_KEY, tmp, ct_len, out, sizeof out);
        check(pl, "decrypt prekey", olm_session_last_error(bs));
        if (pl != sizeof plain || memcmp(out, plain, pl)) {
            printf("  FAIL: Olm round trip mismatch\n");
            failures++;
        }

        /* ping-pong: every turn of direction is a new DH ratchet step */
        int n = 10 * scale;
        start();
        for (i = 0; i < n; i++) {
            OlmSession *s = (i & 1) ? as : bs, *d = (i & 1) ? bs : as;
            size_t mt = olm_encrypt_message_type(s);
            size_t cl = olm_encrypt_message_length(s, sizeof plain);
            char *c = malloc(cl);
            r_len = olm_encrypt_random_length(s);
            r = rnd(r_len);
            check(olm_encrypt(s, plain, sizeof plain, r, r_len, c, cl), "enc",
                  olm_session_last_error(s));
            free(r);
            check(olm_decrypt(d, mt, c, cl, out, sizeof out), "dec",
                  olm_session_last_error(d));
            free(c);
        }
        stop("Olm encrypt+decrypt (ratchet)", n);

        free(tmp); free(ct);
        olm_clear_session(as); free(as);
        olm_clear_session(bs); free(bs);
        olm_clear_account(alice); free(alice);
        olm_clear_account(bob); free(bob);
    }

    /* --- Megolm: the actual room messages ---------------------------------- */
    printf("Megolm (room messages)\n");
    {
        OlmOutboundGroupSession *og =
            olm_outbound_group_session(malloc(olm_outbound_group_session_size()));
        size_t r_len = olm_init_outbound_group_session_random_length(og);
        void *r = rnd(r_len);
        olm_init_outbound_group_session(og, r, r_len);
        free(r);

        size_t kl = olm_outbound_group_session_key_length(og);
        uint8_t *key = malloc(kl);
        olm_outbound_group_session_key(og, key, kl);
        OlmInboundGroupSession *ig =
            olm_inbound_group_session(malloc(olm_inbound_group_session_size()));
        check(olm_init_inbound_group_session(ig, key, kl), "inbound group",
              olm_inbound_group_session_last_error(ig));

        /* a typical chat line as event JSON */
        const char *msg = "{\"type\":\"m.room.message\",\"content\":{\"msgtype\":\"m.text\","
                          "\"body\":\"Hallo von der Vampire! Megolm laeuft.\"},"
                          "\"room_id\":\"!abcdefghijklmnop:example.org\"}";
        size_t ml = strlen(msg);
        int n = 50 * scale;
        size_t cl = olm_group_encrypt_message_length(og, ml);
        char **cts = malloc(n * sizeof *cts);
        start();
        for (i = 0; i < n; i++) {
            cts[i] = malloc(cl);
            check(olm_group_encrypt(og, (const uint8_t *)msg, ml, (uint8_t *)cts[i], cl),
                  "group enc", olm_outbound_group_session_last_error(og));
        }
        stop("Megolm encrypt (sign + AES)", n);

        uint8_t out[512];
        uint32_t idx;
        start();
        for (i = 0; i < n; i++) {
            size_t pl = olm_group_decrypt(ig, (uint8_t *)cts[i], cl, out, sizeof out, &idx);
            check(pl, "group dec", olm_inbound_group_session_last_error(ig));
            if (pl == ml && memcmp(out, msg, ml)) failures++;
            free(cts[i]);
        }
        stop("Megolm decrypt (verify + AES)", n);

        /* history: jump far ahead in the ratchet (old messages, key backup) */
        n = 5 * scale;
        start();
        for (i = 0; i < n; i++) {
            OlmInboundGroupSession *j =
                olm_inbound_group_session(malloc(olm_inbound_group_session_size()));
            olm_init_inbound_group_session(j, key, kl);
            /* encrypt at index ~1000 is too slow to set up; export at a high index instead */
            size_t el = olm_export_inbound_group_session_length(j);
            uint8_t *ex = malloc(el);
            check(olm_export_inbound_group_session(j, ex, el, 1000 + i * 7919),
                  "export", olm_inbound_group_session_last_error(j));
            olm_clear_inbound_group_session(j); free(j); free(ex);
        }
        stop("Megolm ratchet advance ~1000-40000", n);

        free(cts); free(key);
        olm_clear_inbound_group_session(ig); free(ig);
        olm_clear_outbound_group_session(og); free(og);
    }

    /* --- SAS (emoji verification) ------------------------------------------ */
    printf("SAS (emoji verification)\n");
    {
        OlmSAS *a = olm_sas(malloc(olm_sas_size())), *b = olm_sas(malloc(olm_sas_size()));
        size_t r_len = olm_create_sas_random_length(a);
        void *r = rnd(r_len);
        start();
        olm_create_sas(a, r, r_len);
        stop("create SAS keypair", 1);
        free(r);
        r = rnd(r_len);
        olm_create_sas(b, r, r_len);
        free(r);
        char pa[64], pb[64];
        olm_sas_get_pubkey(a, pa, sizeof pa);
        olm_sas_get_pubkey(b, pb, sizeof pb);
        start();
        check(olm_sas_set_their_key(a, pb, olm_sas_pubkey_length(a)), "sas", olm_sas_last_error(a));
        check(olm_sas_set_their_key(b, pa, olm_sas_pubkey_length(b)), "sas", olm_sas_last_error(b));
        stop("SAS key agreement (x2)", 1);
        uint8_t ba[6], bb[6];
        olm_sas_generate_bytes(a, "INFO", 4, ba, 6);
        olm_sas_generate_bytes(b, "INFO", 4, bb, 6);
        if (memcmp(ba, bb, 6)) { printf("  FAIL: SAS mismatch\n"); failures++; }
        olm_clear_sas(a); free(a); olm_clear_sas(b); free(b);
    }

    printf(failures ? "RESULT: %d FAILURES\n" : "RESULT: OK\n", failures);
    return failures ? 20 : 0;
}
