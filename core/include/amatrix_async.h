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
    AMX_CMD_REACT, AMX_CMD_REDACT, AMX_CMD_MEDIA_THUMB, AMX_CMD_MEDIA_SAVE,
    AMX_CMD_SEND_REPLY, AMX_CMD_EDIT, AMX_CMD_LOAD_THREAD, AMX_CMD_SEND_FILE, AMX_CMD_TYPING,
    AMX_CMD_PRIVATE_RECEIPTS, AMX_CMD_MEMBERS, AMX_CMD_ROOM_SETTINGS, AMX_CMD_ROOM_SET, AMX_CMD_MODERATE, AMX_CMD_PROFILE,
    AMX_CMD_SET_PROFILE, AMX_CMD_URL_PREVIEW, AMX_CMD_DEVICES, AMX_CMD_DEVICE_RENAME,
    AMX_CMD_DEVICE_DELETE, AMX_CMD_HISTORY_CACHE
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
    int width, height;            /* MEDIA_THUMB: pixels is width x height; result = the key */
    AmxMember *members;           /* MEMBERS: n entries (freed with the message); result = room */
    AmxRoomSettings *settings;    /* ROOM_SETTINGS (freed with the message); result = room */
    const char *result2;          /* PROFILE: result = display name, result2 = picture (mxc://) */
    AmxDevice *devices;           /* DEVICES: n entries (freed with the message) */
    const char *title, *description, *image;   /* URL_PREVIEW (result = the caller's key, result2 = URL) */
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
/* amx_load_thread(); result = the root, count/at_start as for LOAD_HISTORY */
int amx_async_load_thread(AmxAsync *a, const char *room_id, const char *root, int limit, int from_newest,
                          unsigned long tag);
/* sends a file (amx_send_file); name (UTF-8) and thread_root may be NULL;
 * the result carries the event ID */
int amx_async_send_file(AmxAsync *a, const char *room_id, const char *path, const char *name,
                        const char *thread_root, unsigned long tag);
/* amx_send_typing (no result message); amx_set_private_receipts */
int amx_async_typing(AmxAsync *a, const char *room_id, long ms);
int amx_async_private_receipts(AmxAsync *a, int on);
/* the member list / settings of a room (results carry members, settings and
 * the room ID); ROOM_SET: amx_set_room_setting; MODERATE: action "kick",
 * "ban", "unban" (reason may be NULL) or "power" (level) */
int amx_async_members(AmxAsync *a, const char *room_id, unsigned long tag);
int amx_async_room_settings(AmxAsync *a, const char *room_id, unsigned long tag);
int amx_async_room_set(AmxAsync *a, const char *room_id, const char *what, const char *value, unsigned long tag);
int amx_async_moderate(AmxAsync *a, const char *room_id, const char *user, const char *action,
                       const char *reason, int level, unsigned long tag);
/* our profile (result/result2); SET_PROFILE: what "name" or "avatar" (value:
 * a picture file) */
int amx_async_profile(AmxAsync *a, unsigned long tag);
int amx_async_set_profile(AmxAsync *a, const char *what, const char *value, unsigned long tag);
/* amx_url_preview; key (e.g. the event ID) comes back as result */
int amx_async_url_preview(AmxAsync *a, const char *url, const char *key, unsigned long tag);
/* our sessions (devices/n); rename; sign out (password, wiped after use);
 * results carry the device ID */
int amx_async_devices(AmxAsync *a, unsigned long tag);
int amx_async_device_rename(AmxAsync *a, const char *device_id, const char *name, unsigned long tag);
int amx_async_device_delete(AmxAsync *a, const char *device_id, const char *password, unsigned long tag);
/* amx_set_history_cache (no result).  LOAD_HISTORY from the newest first
 * delivers the cached messages (cached = 1), then the server's. */
int amx_async_history_cache(AmxAsync *a, int on);
/* amx_send_reply() / amx_send_edit(); result = the new event ID */
int amx_async_send_reply(AmxAsync *a, const char *room_id, const char *reply_to, const char *thread_root,
                         const char *body, unsigned long tag);
int amx_async_edit(AmxAsync *a, const char *room_id, const char *event_id, const char *body, unsigned long tag);
/* amx_media_thumbnail(); key (e.g. the event ID) comes back as result */
int amx_async_media_thumbnail(AmxAsync *a, const char *media, int index, const char *key, int max_w,
                              int max_h, unsigned long tag);
/* amx_media_save(); result = path */
int amx_async_media_save(AmxAsync *a, const char *media, int index, const char *path, unsigned long tag);

#ifdef __cplusplus
}
#endif
#endif
