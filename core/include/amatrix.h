/* amatrix.h - Matrix client core for AmigaOS (and POSIX for testing).
 *
 * A client handle per account.  All calls are synchronous; amx_sync() does
 * one /sync round and reports what happened through the event callback.
 * A GUI runs this in its own process and forwards the events (later layer).
 *
 * Strings are UTF-8.  Returned strings marked "caller frees" are malloc'd.
 * Functions returning int give 0 on success, -1 on error; amx_error() then
 * describes the error (Matrix errcode + message, or a local reason).
 */
#ifndef AMATRIX_H
#define AMATRIX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AmxClient AmxClient;

/* ------------------------------------------------------------------ events */

typedef enum {
    AMX_EV_MESSAGE = 1,     /* room message (decrypted or plain): room_id, event_id, sender, msgtype, body */
    AMX_EV_UNDECRYPTABLE,   /* encrypted event we cannot (yet) read: room_id, event_id, sender, reason */
    AMX_EV_INVITE,          /* we were invited: room_id, sender */
    AMX_EV_ROOM_KEY,        /* a room key arrived: room_id, sender */
    AMX_EV_MEMBERSHIP       /* m.room.member in a joined room: room_id, sender, state_key=user, membership in body */
} AmxEventType;

typedef struct {
    AmxEventType type;
    const char *room_id;
    const char *event_id;
    const char *sender;
    const char *msgtype;     /* AMX_EV_MESSAGE: m.text, m.notice, m.emote, m.image, ... */
    const char *body;        /* AMX_EV_MESSAGE: text; AMX_EV_MEMBERSHIP: join/leave/invite/ban */
    const char *state_key;   /* AMX_EV_MEMBERSHIP: the user the change is about */
    const char *reason;      /* AMX_EV_UNDECRYPTABLE: "no key", "UNKNOWN_MESSAGE_INDEX", ... */
    int encrypted;           /* 1 if the event was end-to-end encrypted */
    int own;                 /* 1 if sent by our own user */
} AmxEvent;

typedef void (*AmxEventFn)(AmxClient *c, const AmxEvent *ev, void *userdata);

/* ------------------------------------------------------------- lifecycle */

int  amx_init(void);                  /* once per program: network (bsdsocket/AmiSSL) */
void amx_cleanup(void);

/* homeserver: "https://host[:port]".  store_dir: directory for this
 * account's device, keys and sessions (created if missing). */
AmxClient *amx_client_new(const char *homeserver, const char *store_dir);
void amx_client_free(AmxClient *c);

void amx_set_event_handler(AmxClient *c, AmxEventFn fn, void *userdata);
void amx_set_verbose(AmxClient *c, int level);   /* 0 quiet, 1 info, 2 HTTP */
const char *amx_error(AmxClient *c);

/* ---------------------------------------------------------------- account */

/* Uses the saved login in store_dir if there is one, else logs in with the
 * password and creates a new device.  Uploads device keys as needed. */
int amx_login(AmxClient *c, const char *user, const char *password);
int amx_logout(AmxClient *c);                    /* this device; clears the store */
const char *amx_user_id(AmxClient *c);
const char *amx_device_id(AmxClient *c);

/* ------------------------------------------------------------------- sync */

/* One /sync round (long-poll up to timeout_ms).  Fires events. */
int amx_sync(AmxClient *c, long timeout_ms);

/* -------------------------------------------------------------------- rooms */

typedef struct {
    const char *name;           /* may be NULL */
    const char *topic;          /* may be NULL */
    const char **invite;        /* NULL-terminated user IDs, may be NULL */
    int is_public;              /* public room (preset public_chat), else private */
    int encrypted;              /* enable m.megolm.v1.aes-sha2 */
    int is_direct;              /* direct chat (marks the room as DM for invitees) */
} AmxRoomOptions;

int amx_create_room(AmxClient *c, const AmxRoomOptions *o, char **room_id_out);  /* caller frees */
int amx_join(AmxClient *c, const char *room_id_or_alias, char **room_id_out);    /* out may be NULL */
int amx_leave(AmxClient *c, const char *room_id);
int amx_invite(AmxClient *c, const char *room_id, const char *user_id);
/* NULL-terminated list of joined room IDs; free with amx_free_strings. */
char **amx_joined_rooms(AmxClient *c);
void amx_free_strings(char **list);

/* ---------------------------------------------------------------- messages */

/* Sends m.text; encrypted if the room is.  event_id_out may be NULL. */
int amx_send_text(AmxClient *c, const char *room_id, const char *body, char **event_id_out);

/* ---------------------------------------------------------------- charset */

/* UTF-8 <-> the system's local charset (ISO-8859-1 on AmigaOS, UTF-8 on
 * POSIX).  Characters the local charset lacks become '?'.  Caller frees. */
char *amx_to_local(const char *utf8);
char *amx_from_local(const char *local);

#ifdef __cplusplus
}
#endif
#endif
