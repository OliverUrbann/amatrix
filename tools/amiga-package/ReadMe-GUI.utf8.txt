Amatrix 0.1 - Matrix client for the Amiga (MUI)
===============================================

A small Matrix chat in the style of the old ICQ: on the left the list of
chats and rooms with pictures, a double click opens the chat window, Return
sends.  End-to-end encryption as in Element, without a proxy.

(Deutsche Anleitung: LiesMich.txt)

Requirements
------------
- 68020 or better (Vampire: perfect)
- MUI 5 (muimaster.library V20) with NList.mcc and BetterString.mcc
  (both are in the MUI 5 "contrib" package)
- AmiSSL 5 and a running TCP/IP stack (Roadshow, AmiTCP, ...)
- Optional: Herald (Aminet util/cdity/Herald) for notifications,
  openurl.library to open links, opusdec for voice messages

Installation
------------
Copy the drawer "Amatrix" wherever you like.  Nothing else is needed; the
program, its icon and the language catalogs are all inside the drawer.

Start
-----
Double click "Amatrix".  On the first start a window asks for
  Server:    e.g. https://matrix.org
  User:      e.g. alice
  Password:  is not stored
After that Amatrix logs in by itself on the next start (the login and the
keys are kept in the drawer "Amatrix-xxxxxxxx" next to the program - please
do not give this drawer to anybody).

Language
--------
Amatrix speaks English and German.  It follows the languages in the Locale
preferences (the German texts are Catalogs/deutsch/amatrix.catalog next to
the program; without a catalog Amatrix is English).  To force a language,
add the tool type
    LANGUAGE=english
to the icon, or start from a shell with "Amatrix LANGUAGE=english".
More languages: see README.md in the source (a translation is one text
file; no programming needed).

The menus
---------
Amatrix:        About Amatrix, Iconify, Log out, Quit
Room:           New chat, New room, Join room, Browse space, Members and
                settings, Mute, Favourite, Low priority, Search messages,
                Create poll, Export chat, Leave room
                (acts on the room selected in the list, or on the active
                chat window)
Message:        Quote, Forward, Copy, Copy link to message, Pin,
                Edit history, Report
                (acts on the message selected in the chat window)
Person:         Direct message, Show profile, Mention, Copy user ID,
                Ignore / Stop ignoring, Remove from room, Ban from room
                (acts on the sender of the selected message, not on your
                own; removing and banning only in rooms, and the server
                decides whether you may)
Account:        My profile, Sessions, Password and account, Enter security
                key, Password for Amatrix data
Settings:       Settings, MUI settings
What is not possible right now (no message selected, no room chosen) is
greyed out.

Using it
--------
- List:       Chats and rooms with a picture; below it the last message
              (or topic, members, invitation), the time on the right;
              (n) = unread messages; a double click opens the chat or,
              for invitations, asks whether to accept or decline
- Above the list: a field filters the rooms by name; if you are in
              Spaces, a choice above it selects "All rooms" or the rooms
              of one space.  Favourites (star) are on top, low priority
              rooms at the bottom (menu "Favourite on/off", Amiga-F, and
              "Low priority on/off").
- New chat:   direct chat with a user (@name:server, or just "name")
- New room:   name, encrypted, in the directory; can also create a space
- Menu "Join room ...": search public rooms or enter #alias:server; with
              "Preview" you read the history without joining (if the room
              allows it)
- Menu "Enter security key ...": type the security key from Element
              (EsTc ...) or the security passphrase.  Afterwards this
              device is verified (no warning in Element), and old messages
              are decrypted from the key backup when you open a room.  A
              passphrase takes some minutes on the Amiga, the key is fast.
- Menu "Password for Amatrix data ...": protects the login, keys and
              sessions in the drawer "Amatrix-xxxxxxxx" with a password
              (AES-256).  Amatrix asks for it at start.  Forgotten = the
              data is useless (then log out and log in again).
- Menu "Log out": logs this device out and deletes its keys
- Menu "My profile ..." (Amiga-O): display name and profile picture
- Menu "Sessions ..." (Amiga-G): your logged-in devices, whether they are
              verified, rename them, log others out (needs the password)
- Menu "Search messages ..." (Amiga-D): in all rooms or the selected one.
              The server searches unencrypted rooms, for encrypted rooms
              only the history stored on the Amiga is searched.  A double
              click on a hit opens the chat.
- Menu "Members and settings ..." (Amiga-T), also the i symbol in the chat
              window: name, topic, picture, who may join, who may read the
              history, switch on encryption; the members with their role
              (founder, admin, moderator, member), invite, remove, ban/
              unban, assign a role.  What you may not do in that room is
              greyed out.
- Chat window: loads the last 30 messages when opened.
              Toolbar on top (hold the mouse over a button for its help):
                arrow up      = load 20 older messages
                arrow back    = reply to the selected message (the line
                                above the input shows what to; "Cancel"
                                takes it back)
                pen           = edit your own selected message (the text
                                goes into the input, Return saves)
                waste basket  = delete your own selected message
                smiley + plus = react to the selected message (without a
                                selection: the newest from someone else);
                                opens a small choice; the same one again
                                takes your reaction back
                paper clip    = send a picture or file (requester, several
                                at once); or drag icons from the Workbench
                                into the chat window
                person + plus = invite someone to this room
                i in a circle = members and room settings
                door + arrow  = leave the room for good (asks first)
              Replies show a quote line above them (» Name: text), edited
              messages "(edited)", deleted ones "(message deleted)".  The
              date is shown where the day changes.  Threads: under the
              first message it says "Thread: n replies"; a click on the
              speech bubble symbol opens the thread in its own window,
              what you write there goes into the thread.
              Formatted messages (from Element): bold, italic and
              underline are shown as such, lists, quotes ("| "), headings
              and code blocks as lines of their own, links as
              "text (address)".  Messages that mention you have a bold "@"
              before the time; the room list then says "(3 @)".
              Emojis in the text appear as small pictures; unknown ones as
              a yellow face with a question mark.
              Pictures: a small preview below the message; a click opens
              the picture with the default program (Workbench/DefIcons,
              otherwise MultiView; copy in T:Amatrix).  The file symbol
              behind the name saves the picture or file wherever you like.
              Links are underlined; the globe behind opens the browser
              (openurl.library).
              Below a link there is a preview (title - description) which
              your server fetches; in encrypted rooms only if switched on
              in the settings.
              Type at the bottom, Return sends.  On the right of the
              toolbar it says "Alice is typing ..." when someone writes; a
              blue check mark behind your message means: read.
              Amatrix keeps the latest messages of every room in its
              drawer; a chat window shows them at once (even without a
              network), then the rest comes from the server.
              Reactions are shown below the message with a count (people);
              similar emojis count together (all laughing smileys, all
              hearts/kisses).  Your own count is bold.  A click on a small
              picture there sets or takes back your reaction.
              ARexx: REACT <room> heart (thumbsup, thumbsdown, heart,
              smile, joy, confused, eyes, tada, rocket).

ARexx port: AMATRIX.1 (OPENCHAT, SAY, ACCEPT, NEWCHAT, NEWROOM, INVITE,
STATUS, CHATLINES, UNREADABLE, OLDER, FIRSTLINE, UNREAD, ROOMLINE,
RECOVERY, UNLOCK, STOREPASS, CHATSTATUS, CLOSECHAT, REACT, REACTIONS,
PICKER, LEAVE, LASTLINE, MEDIA, SAVEMEDIA, OPENMEDIA, LINKS, OPENLINK,
REPLY, EDITLAST, DELETELAST, TEXTS, OPENTHREAD, THREADSAY, MUTE, SENDFILE,
COPY, CLIP, TYPE, TYPING, READ, ROOMINFO, MEMBERS, INFOSTATE, INFOSET,
INFOMEMBER, PROFILE, PROFILESTATE, PREVIEWS, SETOPTION, SESSIONS,
SESSIONLIST, SESSIONSET, CHATCACHED, FILTER, SPACE, ROOMS, FAVOURITE, FIND,
HITS, QUIT; plus MUI's own such as HIDE and SHOW).

Warning: after you enter the security key, the drawer "Amatrix-xxxxxxxx"
also holds the key to the key backup.  Best protect it with a password
(see above) and do not give it to anybody.

More features (as in Element)
-----------------------------
- Commands in the input field: /help shows them all (/me, /shrug,
  /spoiler, /plain, /html, /nick, /myroomnick, /topic, /roomname,
  /invite, /join, /knock, /part, /ignore, /unignore, /whois,
  /upgraderoom, /jumptodate ...).
- Formatting while typing, as Markdown: *italic*, **bold**, `code`, lists,
  > quote.  :smile: and other short codes become emojis.  A started @name
  or :code is completed with the Tab key (@name becomes a real mention).
- Arrow up in an empty input field: edit your own last message.  What is
  left in the field when you close a chat window is kept as a draft (room
  list: "Draft: ...").
- Polls: shown with their answers, a click votes; menu "Create poll ..."
  (Amiga-U), end your own polls.
- Locations: as text with a link to OpenStreetMap.
- Voice messages: "[Voice message 0:12]"; a click saves and plays it.  If
  opusdec (Aminet mus/edit/opus-tools) is in C: or in the Amatrix drawer,
  a WAV file is made first.
- Menu: "Quote" (Amiga-Z), "Forward ..." (Amiga-W), "Copy link to
  message", "Pin on/off" (pinned messages are shown in a bar above the
  history), "Edit history ...", "Report ..." (to the server's admins),
  "Export chat ..." (text or HTML), "Browse space ..." (the rooms of a
  space: join, preview, add/remove the selected room), "Password and
  account ..." (change password, deactivate account).
- Unread: when you open a chat, a marker stands above the first unread
  message.  /jumptodate DD.MM.YYYY jumps to a day.
- Notifications per room on the server (room window: default, all,
  mentions only, off) and keywords in the settings - also for your other
  devices.
- Ignore users (/ignore, the list is in the settings); decline an
  invitation and block the sender at once.
- Knocking: ask for entry to "knock" rooms (/knock); who knocks is shown in
  the room window and can be let in.
- A replaced room (room upgrade) shows a bar leading to the new room.
- Under a message "read by ..." shows who has read up to there.
- Settings: show joins/leaves/name changes, 24-hour clock, compact
  display, "Show who has read what".

Not planned: verifying other devices by comparing emojis (the security key
is enough), voice and video calls.

Notifications, iconify, settings
--------------------------------
- Notifications come through Herald (Aminet util/cdity/Herald).  If Herald
  is not running there are none.  They appear for new messages when the
  window of the room is not in front or Amatrix is iconified.  A double
  click on the notification opens the chat.
- Menu Room/Mute (Amiga-M) for the chosen room: no notifications, the list
  says "(muted)".
- Menu "Iconify" (Amiga-I) or the iconify symbol in the window: Amatrix
  becomes an AppIcon on the Workbench.  With unread messages it gets a
  pink exclamation mark (Icons/Amatrix-neu); the window title shows the
  number of unread messages.
- Menu "Settings ..." (Amiga-E): notifications on/off, for all messages or
  only direct messages and mentions, picture previews in the chat on/off,
  link previews (also in encrypted rooms), show that I am typing, send
  read receipts, store history locally, pictures in the room list, start
  iconified (also with the argument HIDE).
- Menu "MUI settings ...": font, colours etc. for Amatrix.

Known bugs
----------
- Thin horizontal lines sometimes appear in picture previews, mostly while
  scrolling.  For the strips a preview is made of, NList now and then does
  not draw the bottom row.  The picture itself is fine; after a complete
  redraw it is right.

Credits and licences
--------------------
Amatrix uses libolm (Apache-2.0), cJSON (MIT), stb_image (public domain),
the AmiSSL SDK (Apache-2.0) and the MUI SDK (not included).  Their texts
are in the source tree (third_party/).  Matrix is an open standard of the
Matrix.org Foundation; Element is a trademark of Element.
