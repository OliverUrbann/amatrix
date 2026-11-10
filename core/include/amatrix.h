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
    AMX_EV_MEMBERSHIP,      /* m.room.member in a joined room: room_id, sender, state_key=user, membership in body */
    AMX_EV_REACTION,        /* m.reaction (m.annotation): room_id, event_id, sender, relates_to = the
                               message reacted to, body = the key (an emoji, UTF-8) */
    AMX_EV_REDACTION,       /* m.room.redaction: room_id, event_id, sender, relates_to = the event
                               removed (a message or a reaction) */
    AMX_EV_EDIT,            /* an edit (m.replace): room_id, event_id (of the edit), sender,
                               relates_to = the message edited, msgtype/body = its new text */
    AMX_EV_TYPING,          /* who is typing in room_id now (not us): state_key = their user
                               IDs, body = their names, one per line; "" when nobody */
    AMX_EV_RECEIPT          /* a read receipt (not ours): room_id, sender = the reader, event_id
                               = the newest event read, timestamp */
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
    int history;             /* 1 if delivered by amx_load_history() */
    double timestamp;        /* origin_server_ts in ms */
    const char *sender_name; /* display name of the sender in this room (or the
                                localpart of the user ID if none is known) */
    const char *relates_to;  /* AMX_EV_REACTION / AMX_EV_REDACTION: the event concerned */
    const char *reply_to;    /* AMX_EV_MESSAGE: the message this answers (m.in_reply_to), or NULL;
                                a quoted fallback ("> <@user> ...") is taken off body */
    const char *thread_root; /* AMX_EV_MESSAGE: the thread (its first message) this belongs to */
    int redacted;            /* AMX_EV_MESSAGE: the message was deleted (body is "") */
    int thread_count;        /* AMX_EV_MESSAGE: replies in the thread it starts (server's count) */
    const char *formatted;   /* AMX_EV_MESSAGE / AMX_EV_EDIT: the HTML form of body
                                (formatted_body, org.matrix.custom.html), or NULL */
    int mention;             /* AMX_EV_MESSAGE: it mentions us (m.mentions, or our user
                                ID in the text for senders without m.mentions) */
    int cached;              /* AMX_EV_MESSAGE from the local history cache (amx_cached_history) */
    const char *media;       /* AMX_EV_MESSAGE with attachments (msgtype m.image /
                                m.file / m.video / m.audio / m.sticker, or a gallery):
                                an opaque description for amx_media_*() (body is the
                                file name or caption); else NULL */
} AmxEvent;

typedef void (*AmxEventFn)(AmxClient *c, const AmxEvent *ev, void *userdata);

/* ------------------------------------------------------------- lifecycle */

int  amx_init(void);                  /* once per program: network (bsdsocket/AmiSSL) */

/* Diagnosis: every log line (amx_set_verbose) and amx_log() also goes to this
 * file, written and closed line by line so it survives a crash or a frozen
 * machine.  NULL switches it off.  Shared by all processes of the program. */
void amx_set_logfile(const char *path);
void amx_log(const char *fmt, ...);
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
/* ------------------------------------------------------- store passphrase */

/* The store (login, keys, sessions, key backup key) can be protected with a
 * passphrase: AES-256-GCM per file, key by PBKDF2.  amx_store_encrypted():
 * the store has one (store.json); amx_store_locked(): ... and it was not
 * given yet - then amx_store_unlock() must come before amx_login().
 * amx_store_set_passphrase(): set, change (store unlocked) or with NULL/""
 * remove it; existing files are converted. */
int amx_store_encrypted(AmxClient *c);
int amx_store_locked(AmxClient *c);
int amx_store_unlock(AmxClient *c, const char *passphrase);
int amx_store_set_passphrase(AmxClient *c, const char *passphrase);

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
/* NULL-terminated list of joined room IDs (asks the server); free with
 * amx_free_strings. */
char **amx_joined_rooms(AmxClient *c);
void amx_free_strings(char **list);

/* The room list as the client knows it from /sync (kept in the store, so it
 * is complete after a restart too): joined rooms and pending invites, most
 * recent activity first, invites on top. */
typedef enum { AMX_ROOM_JOINED = 1, AMX_ROOM_INVITED } AmxMembership;

typedef struct {
    const char *room_id;
    const char *name;          /* display name: room name, alias, or the members' names */
    const char *topic;         /* may be NULL */
    const char *alias;         /* canonical alias, may be NULL */
    const char *dm_user;       /* direct chat: the other user, else NULL */
    const char *inviter;       /* AMX_ROOM_INVITED: who invited us */
    AmxMembership membership;
    int is_direct;             /* direct chat (m.direct) */
    int encrypted;
    int joined_members, invited_members;
    int unread;                /* notifications (server count) */
    int highlights;            /* mentions etc. */
    double last_activity;      /* origin_server_ts in ms of the newest event seen, 0 = unknown */
    const char *avatar_url;    /* mxc:// picture: the room's, else the other person's; may be NULL */
    const char *last_sender;   /* newest message seen (live): sender's display name, may be NULL */
    const char *last_body;     /*   its text (shortened; "(verschlüsselt)" without key), may be NULL */
    int last_own;              /*   1 if we sent it */
    const char *inviter_name;  /* AMX_ROOM_INVITED: display name of who invited us */
} AmxRoomInfo;

/* Snapshot of the room list.  Returns the number of rooms (0..), -1 on error;
 * *list is valid until amx_free_rooms(). */
int  amx_rooms(AmxClient *c, AmxRoomInfo **list);
void amx_free_rooms(AmxRoomInfo *list, int n);

/* Public room directory of the own server (server NULL) or another one.
 * search may be NULL; at most limit entries.  Returns the count or -1. */
typedef struct {
    const char *room_id, *name, *topic, *alias;
    int members;
} AmxPublicRoom;
int  amx_public_rooms(AmxClient *c, const char *server, const char *search, int limit,
                      AmxPublicRoom **list);
void amx_free_public_rooms(AmxPublicRoom *list, int n);

/* Room history, newest first, as events (ev->history = 1) via the callback,
 * decrypted where keys are known.  from_newest = 1 starts at the present,
 * 0 continues where the previous call stopped.  *at_start (may be NULL) is
 * set to 1 when the beginning of the room is reached.  Returns the number of
 * events delivered or -1. */
int amx_load_history(AmxClient *c, const char *room_id, int limit, int from_newest, int *at_start);

/* ---------------------------------------------------------------- messages */

/* Reacts to a message with key (an emoji, UTF-8; encrypted in encrypted
 * rooms).  event_id_out (may be NULL) gets the reaction's ID: it is what
 * amx_redact() needs to take the reaction back.  Caller frees. */
int amx_send_reaction(AmxClient *c, const char *room_id, const char *event_id, const char *key,
                      char **event_id_out);
/* Attachments (AmxEvent.media: one or more - a gallery has several; encrypted
 * ones are decrypted and checked).
 * amx_media_count: how many; amx_media_info: a field of one ("type" m.image/
 * m.file/m.video/m.audio/m.sticker, "name", "mime", "size", "w", "h") into
 * buf, or NULL.
 * amx_media_thumbnail: a preview of attachment index that fits into
 * max_w x max_h (aspect kept, never enlarged) as 0xAARRGGBB, *w x *h; free()
 * it.  Uses the server's thumbnail, or the encrypted thumbnail / the picture
 * itself (PNG, JPEG, GIF) in encrypted rooms.
 * amx_media_save: the whole file to path. */
int amx_media_count(const char *media);
const char *amx_media_info(const char *media, int index, const char *field, char *buf, size_t size);
int amx_media_thumbnail(AmxClient *c, const char *media, int index, int max_w, int max_h,
                        unsigned long **argb, int *w, int *h);
int amx_media_save(AmxClient *c, const char *media, int index, const char *path);

/* Like amx_load_history, for one thread (root = its first message): the
 * thread's messages, newest first, then (when all are there) the root itself;
 * events come with history = 1. */
int amx_load_thread(AmxClient *c, const char *room_id, const char *root, int limit, int from_newest,
                    int *at_start);

/* A reply to event reply_to; with thread_root (may be NULL) inside that thread
 * (reply_to may then be NULL: just a message in the thread). */
int amx_send_reply(AmxClient *c, const char *room_id, const char *reply_to, const char *thread_root,
                   const char *body, char **event_id_out);
/* Replaces the text of an own message (m.replace). */
int amx_send_edit(AmxClient *c, const char *room_id, const char *event_id, const char *body,
                  char **event_id_out);

/* Sends the file at path (a picture as m.image with its size, else m.file/
 * m.video/m.audio); name shown to others (NULL: the file name); encrypted
 * in encrypted rooms; with thread_root inside that thread. */
int amx_send_file(AmxClient *c, const char *room_id, const char *path, const char *name,
                  const char *thread_root, char **event_id_out);

/* ------------------------------------------------- members and settings */

typedef struct {
    char *user_id, *name, *avatar_url;    /* name: display name (UTF-8) or localpart */
    char *membership;                     /* "join", "invite", "ban" */
    int power;                            /* power level: 100 admin, 50 moderator,
                                             AMX_POWER_CREATOR a creator (room version 12+) */
} AmxMember;
#define AMX_POWER_CREATOR 1000000
/* The room's members (joined first, by power, then name); count or -1. */
int  amx_room_members(AmxClient *c, const char *room_id, AmxMember **list);
void amx_free_members(AmxMember *list, int n);

typedef struct {
    char *name, *topic, *avatar_url;      /* NULL if not set (UTF-8) */
    char *join_rule;                      /* "public", "invite", "knock", ... */
    char *history;                        /* "shared", "invited", "joined", "world_readable" */
    int encrypted;
    int my_power;                         /* our power level in the room */
    int level_name, level_topic, level_avatar, level_join_rules, level_history, level_encryption,
        level_power, level_invite, level_kick, level_ban, level_redact;    /* needed for each */
} AmxRoomSettings;
int  amx_room_settings(AmxClient *c, const char *room_id, AmxRoomSettings *s);
void amx_free_room_settings(AmxRoomSettings *s);
/* what: "name", "topic", "join_rule" (public/invite/knock), "history"
 * (shared/invited/joined/world_readable), "encryption" (value ignored, cannot
 * be undone), "avatar" (value: a picture file, uploaded). */
int  amx_set_room_setting(AmxClient *c, const char *room_id, const char *what, const char *value);
int  amx_kick(AmxClient *c, const char *room_id, const char *user, const char *reason);
int  amx_ban(AmxClient *c, const char *room_id, const char *user, const char *reason);
int  amx_unban(AmxClient *c, const char *room_id, const char *user);
int  amx_set_power(AmxClient *c, const char *room_id, const char *user, int level);

/* A profile (user NULL: our own): display name and picture (mxc://), each
 * NULL if not set; free() them. */
int  amx_get_profile(AmxClient *c, const char *user, char **name, char **avatar_url);
int  amx_set_displayname(AmxClient *c, const char *name);
/* the picture file (PNG, JPEG, GIF) uploaded as our profile picture */
int  amx_set_avatar(AmxClient *c, const char *path);

/* The local history cache: the newest messages of a room as stored the last
 * time (decrypted; edits and deletions applied), delivered as events with
 * history = 1 and cached = 1, newest first; returns how many.  On by default;
 * off deletes it. */
int  amx_cached_history(AmxClient *c, const char *room_id);
void amx_set_history_cache(AmxClient *c, int on);

/* Our sessions (devices): the one in use first, then by last activity. */
typedef struct {
    char *device_id, *name, *last_seen_ip;  /* name / ip may be NULL */
    double last_seen_ts;                    /* ms, 0 if unknown */
    int current;                            /* this is us */
    int verified;                           /* 1 cross-signed, 0 not, -1 no encryption keys */
} AmxDevice;
int  amx_devices(AmxClient *c, AmxDevice **list);
void amx_free_devices(AmxDevice *list, int n);
int  amx_rename_device(AmxClient *c, const char *device_id, const char *name);
/* signs another session out; the server asks for our password */
int  amx_delete_device(AmxClient *c, const char *device_id, const char *password);

/* The server's preview of a web page (Open Graph): title, description and
 * picture (mxc://), each NULL if missing; free() them.  -1 if the server has
 * none (previews off, page unreachable). */
int  amx_url_preview(AmxClient *c, const char *url, char **title, char **description, char **image);

/* Removes an event (a message or a reaction of ours); reason may be NULL. */
int amx_redact(AmxClient *c, const char *room_id, const char *event_id, const char *reason);

/* Marks the room as read up to event_id (read receipt + fully-read marker);
 * the server's unread count for the room drops to 0. */
int amx_mark_read(AmxClient *c, const char *room_id, const char *event_id);
/* Read receipts others do not see (m.read.private) from now on, or public again. */
void amx_set_private_receipts(AmxClient *c, int on);
/* "typing" for ms milliseconds in room_id (0: stopped typing). */
int amx_send_typing(AmxClient *c, const char *room_id, long ms);

/* Sends m.text; encrypted if the room is.  event_id_out may be NULL. */
int amx_send_text(AmxClient *c, const char *room_id, const char *body, char **event_id_out);

/* ------------------------------------------------------------ recovery */

/* The security key from Element ("EsTc 9xYz ...", spaces optional) or the
 * security passphrase opens secret storage on the server.  With it we
 *   - sign this device with the self-signing key: Element shows it as
 *     verified (*verified = 1),
 *   - keep the key backup's private key in the store (*backup = 1); from
 *     then on amx_load_history() fetches the room's old keys from the backup.
 * 0 if the key was right (even if one of the two was not possible), else -1. */
int amx_recovery_unlock(AmxClient *c, const char *key_or_passphrase, int *verified, int *backup);

/* 1 = this device carries a valid signature of our self-signing key
 * (verified, as after amx_recovery_unlock), 0 = not, -1 = error.  Asks the
 * server, so it survives restarts. */
int amx_device_verified(AmxClient *c);

/* Imports the backed-up keys of one room (and retries undecryptable events).
 * Number of new keys, 0 if none or no backup key, -1 on error. */
int amx_backup_room(AmxClient *c, const char *room_id);
int amx_backup_available(AmxClient *c);

/* ------------------------------------------------------------------ media */

/* A picture (e.g. AmxRoomInfo.avatar_url) as size x size pixels, 0xAARRGGBB,
 * centre-cropped to a square and scaled.  Fetched once as a server thumbnail
 * (PNG/JPEG), then taken from the store.  *argb: caller frees.  0 or -1. */
int amx_avatar(AmxClient *c, const char *mxc_url, int size, unsigned long **argb);

/* ---------------------------------------------------------------- charset */

/* UTF-8 <-> the system's local charset (ISO-8859-1 on AmigaOS, UTF-8 on
 * POSIX).  Characters the local charset lacks become '?'.  Caller frees. */
char *amx_to_local(const char *utf8);
char *amx_from_local(const char *local);

#ifdef __cplusplus
}
#endif
#endif
