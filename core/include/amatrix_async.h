/* amatrix_async.h - the Matrix client in its own process, for GUIs.
 *
 * amx_async_new() starts a network process (AmigaOS: CreateNewProc, POSIX:
 * a thread).  It owns the AmxClient and does all networking and crypto;
 * the GUI never blocks.  The GUI sends commands (amx_async_login(),
 * amx_async_send_text(), ...), which return at once, and receives messages:
 * events, the room list, the result of each command, the connection state.
 *
 * GUI loop on AmigaOS:
 *
 *     ULONG amx_sig = amx_async_sigmask(a);
 *     for (;;) {
 *         sigs = Wait(sigs | amx_sig);          (with MUI: MUIM_Application_NewInput)
 *         while ((m = amx_async_get(a)) != NULL) { ...; amx_async_msg_free(m); }
 *     }
 *
 * On POSIX, amx_async_fd() becomes readable when messages are waiting.
 *
 * After a successful login the network process syncs continuously; a
 * command interrupts the long poll, so it runs at once.  The network process
 * initialises the network itself: do not call amx_init()/amx_cleanup() in a
 * program that uses this layer.
 *
 * Every command takes a tag chosen by the caller; it comes back in the
 * AMX_MSG_RESULT of that command.  Commands are executed in order.
 */
#ifndef AMATRIX_ASYNC_H
#define AMATRIX_ASYNC_H

#include "amatrix.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AmxAsync AmxAsync;

typedef enum {
    AMX_CMD_LOGIN = 1, AMX_CMD_LOGOUT, AMX_CMD_SEND_TEXT, AMX_CMD_JOIN, AMX_CMD_LEAVE,
    AMX_CMD_INVITE, AMX_CMD_CREATE_ROOM, AMX_CMD_LOAD_HISTORY, AMX_CMD_PUBLIC_ROOMS,
    AMX_CMD_AVATAR, AMX_CMD_MARK_READ, AMX_CMD_RECOVERY, AMX_CMD_STORE_UNLOCK, AMX_CMD_STORE_PASS,
    AMX_CMD_REACT, AMX_CMD_REDACT
} AmxCmd;

typedef enum {
    AMX_MSG_EVENT = 1,   /* ev: an event (as the AmxEventFn callback gets it) */
    AMX_MSG_ROOMS,       /* rooms/n: the complete room list, sent when it changed */
    AMX_MSG_RESULT,      /* cmd/tag/status: a command finished */
    AMX_MSG_STATE        /* online: connection state changed; error when offline */
} AmxMsgType;

typedef struct {
    AmxMsgType type;
    AmxEvent ev;                  /* AMX_MSG_EVENT */
    AmxRoomInfo *rooms;           /* AMX_MSG_ROOMS (n entries) */
    AmxPublicRoom *pub;           /* AMX_MSG_RESULT of AMX_CMD_PUBLIC_ROOMS (n entries) */
    int n;
    AmxCmd cmd;                   /* AMX_MSG_RESULT */
    unsigned long tag;
    int status;                   /* 0 ok, -1 error (see error) */
    const char *result;           /* LOGIN: user ID, SEND_TEXT: event ID,
                                     JOIN/CREATE_ROOM: room ID, REACT: the
                                     reaction's event ID, AVATAR: the
                                     mxc URL; else NULL */
    const char *error;            /* RESULT/STATE: reason, or NULL */
    int at_start;                 /* LOAD_HISTORY: beginning of the room reached */
    int count;                    /* LOAD_HISTORY: events delivered */
    int online;                   /* AMX_MSG_STATE */
    unsigned long *pixels;        /* AVATAR: size x size, 0xAARRGGBB (freed with the message) */
    int verified, backup;         /* RECOVERY: device signed / backup key taken
                                     over; LOGIN: device verified */
    int size;
} AmxMsg;

/* Starts the network process for one account (store_dir as for
 * amx_client_new).  NULL if it could not be started. */
AmxAsync *amx_async_new(const char *homeserver, const char *store_dir);

/* Stops the network process (interrupting a sync) and frees everything,
 * including undelivered messages. */
void amx_async_free(AmxAsync *a);

unsigned long amx_async_sigmask(AmxAsync *a);   /* AmigaOS: signals for Wait() */
int amx_async_fd(AmxAsync *a);                  /* POSIX: readable = messages */

/* Next message or NULL (never blocks).  Free with amx_async_msg_free(). */
AmxMsg *amx_async_get(AmxAsync *a);
void amx_async_msg_free(AmxMsg *m);

/* 1 = log to stdout, 2 = HTTP too (from the network process) */
void amx_async_set_verbose(AmxAsync *a, int level);

/* Commands: 0 = queued, -1 = out of memory.  Strings are copied. */
int amx_async_login(AmxAsync *a, const char *user, const char *password, unsigned long tag);
int amx_async_logout(AmxAsync *a, unsigned long tag);
int amx_async_send_text(AmxAsync *a, const char *room_id, const char *body, unsigned long tag);
int amx_async_join(AmxAsync *a, const char *room_id_or_alias, unsigned long tag);
int amx_async_leave(AmxAsync *a, const char *room_id, unsigned long tag);
int amx_async_invite(AmxAsync *a, const char *room_id, const char *user_id, unsigned long tag);
int amx_async_create_room(AmxAsync *a, const AmxRoomOptions *opt, unsigned long tag);
int amx_async_load_history(AmxAsync *a, const char *room_id, int limit, int from_newest,
                           unsigned long tag);
int amx_async_public_rooms(AmxAsync *a, const char *server, const char *search, int limit,
                           unsigned long tag);
int amx_async_avatar(AmxAsync *a, const char *mxc_url, int size, unsigned long tag);
int amx_async_mark_read(AmxAsync *a, const char *room_id, const char *event_id, unsigned long tag);
/* amx_recovery_unlock() in the network process (a passphrase can take minutes) */
int amx_async_recovery(AmxAsync *a, const char *key_or_passphrase, unsigned long tag);
/* the store passphrase (amx_store_unlock before login / amx_store_set_passphrase) */
int amx_async_store_unlock(AmxAsync *a, const char *passphrase, unsigned long tag);
int amx_async_store_passphrase(AmxAsync *a, const char *passphrase, unsigned long tag);
int amx_async_react(AmxAsync *a, const char *room_id, const char *event_id, const char *key, unsigned long tag);
int amx_async_redact(AmxAsync *a, const char *room_id, const char *event_id, unsigned long tag);

#ifdef __cplusplus
}
#endif
#endif
