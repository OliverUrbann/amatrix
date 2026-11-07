/* Internal state and helpers shared by the core's source files. */
#ifndef AMX_INTERNAL_H
#define AMX_INTERNAL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "amatrix.h"
#include "olm/olm.h"
#include "olm/outbound_group_session.h"
#include "olm/inbound_group_session.h"
#include "cJSON.h"
#include "canonjson.h"
#include "http.h"
#include "platform.h"

#define OLM_ALG    "m.olm.v1.curve25519-aes-sha2"
#define MEGOLM_ALG "m.megolm.v1.aes-sha2"

/* Fixed key for pickles at rest for now; a passphrase comes later (P1). */
#define PICKLE_KEY "amatrix-prototype-pickle-key"
#define PK PICKLE_KEY, (sizeof PICKLE_KEY - 1)

typedef struct { char *sender_key; OlmSession *s; } OlmSess;
typedef struct { char *room_id, *session_id, *sender_key, *ed25519; OlmInboundGroupSession *s; } GroupSess;

struct AmxClient {
    char *hs, *store;
    char *user_id, *device_id, *token;
    OlmAccount *acct;
    char *curve, *ed;              /* our identity keys */
    char *since;
    OlmSess *olm; int n_olm;
    GroupSess *grp; int n_grp;
    cJSON *pending;                /* encrypted room events without key yet */
    cJSON *devices;                /* user -> [ {user, device_id, curve, ed} ] (verified) */
    cJSON *outbound;               /* room -> {pickle, shared{curve:true}, count} */
    cJSON *nokey;                  /* curve -> true: no one-time key (this run) */
    cJSON *room_enc;               /* room -> true/false: encryption state cache */
    cJSON *seen;                   /* "session|index" -> event_id (replay protection) */
    cJSON *rooms;                  /* room list state (rooms.json) */
    cJSON *direct;                 /* m.direct account data: user -> [room ids] */
    cJSON *hist;                   /* room -> pagination token for amx_load_history */
    cJSON *backup;                 /* backup.json: key backup version + private key */
    cJSON *bk_done;                /* "room|session" -> {n: tries, t: next try} for the backup */
    cJSON *kreq;                   /* "room|session" -> key request sent to our devices (this run) */
    unsigned char skey[32];        /* store key (passphrase), if skey_set */
    int skey_set;
    OlmUtility *util;
    AmxEventFn fn; void *ud;
    int verbose;
    int interruptible;          /* async layer: /sync long poll may be woken up */
    unsigned long txn;
    char err[512];
};

/* client.c */
void  amx__log(AmxClient *c, int level, const char *fmt, ...);
#define LOG(c, ...)  amx__log((c), 1, __VA_ARGS__)
#define LOG2(c, ...) amx__log((c), 2, __VA_ARGS__)
int   amx__fail(AmxClient *c, const char *fmt, ...);   /* sets error, returns -1 */
void *amx__malloc(size_t n);
char *amx__strdup(const char *s);
char *amx__strndup(const char *s, size_t n);
const char *amx__jstr(const cJSON *o, const char *k);
char *amx__read_store(AmxClient *c, const char *name);
int   amx__write_store(AmxClient *c, const char *name, const char *data);
int   amx__save_json(AmxClient *c, const char *name, const cJSON *j);
void *amx__random(size_t n);
char *amx__txn(AmxClient *c);
/* path relative to /_matrix/client/v3 (or absolute starting with /_matrix).
 * Returns parsed JSON or NULL; *status gets the HTTP status (0 = network). */
cJSON *amx__api(AmxClient *c, const char *method, const char *path, const cJSON *body,
                long timeout_s, long *status);
cJSON *amx__api_ok(AmxClient *c, const char *method, const char *path, const cJSON *body);
void  amx__emit(AmxClient *c, AmxEvent *ev);

/* keys.c */
int   amx__load_or_create_account(AmxClient *c);
int   amx__save_account(AmxClient *c);
int   amx__upload_keys(AmxClient *c, int otk_on_server, int need_fallback);
void  amx__sign_json(AmxClient *c, cJSON *obj);
int   amx__verify_signed(AmxClient *c, const cJSON *obj, const char *user, const char *keyid, const char *ed);

/* crypto_recv.c */
int   amx__load_sessions(AmxClient *c);
int   amx__save_olm_sessions(AmxClient *c);
int   amx__save_group_sessions(AmxClient *c);
char *amx__store_decode(AmxClient *c, const char *name, char *file);
char *amx__store_encode(AmxClient *c, const char *name, const char *data);
int   amx__backup_for_events(AmxClient *c, const char *room_id, const cJSON *events, int only_missing);
int   amx__put_group_session(AmxClient *c, const char *room, const char *sid, const char *sender_key,
                             const char *ed, OlmInboundGroupSession *s);
GroupSess *amx__find_group(AmxClient *c, const char *room_id, const char *session_id);
OlmSession *amx__new_olm_session(void);
OlmInboundGroupSession *amx__new_group_session(void);
void  amx__add_olm_session(AmxClient *c, const char *sender_key, OlmSession *s);
void  amx__add_group_session(AmxClient *c, const char *room, const char *sid, const char *sender_key,
                             const char *ed, OlmInboundGroupSession *s);
void  amx__handle_to_device(AmxClient *c, const cJSON *ev);
void  amx__handle_room_event(AmxClient *c, const char *room_id, const cJSON *ev, int history);
void  amx__retry_pending(AmxClient *c);
void  amx__fetch_missing_keys(AmxClient *c);   /* backup + key requests for pending events */

/* crypto_send.c */
int   amx__load_send_state(AmxClient *c);
void  amx__forget_devices(AmxClient *c, const char *user);
int   amx__encrypt_room_event(AmxClient *c, const char *room_id, const char *type,
                              const cJSON *content, cJSON **encrypted_out);

/* roomstate.c */
int   amx__rooms_load(AmxClient *c);
int   amx__rooms_save(AmxClient *c);
const char *amx__member_name(AmxClient *c, const char *room_id, const char *user);
void  amx__room_note_message(AmxClient *c, const AmxEvent *ev);
void  amx__room_sync_join(AmxClient *c, const char *room_id, const cJSON *jr);
void  amx__room_sync_invite(AmxClient *c, const char *room_id, const cJSON *ir);
void  amx__room_forget(AmxClient *c, const char *room_id);
void  amx__set_direct(AmxClient *c, const cJSON *content);
const char *amx__room_inviter_if_direct(AmxClient *c, const char *room_id);
int   amx__add_direct(AmxClient *c, const char *user, const char *room_id);

/* rooms.c */
int   amx__room_encrypted(AmxClient *c, const char *room_id);   /* 1, 0, -1 error */
void  amx__note_room_encryption(AmxClient *c, const char *room_id, int enc);

#endif
