# Changelog

## 0.2 (15.11.2026)

- Fixed: login failed with `HTTP 400 M_NOT_JSON Body is not a valid JSON document`
  (or `Content not JSON`) when the password contains an umlaut or another
  non-ASCII character. Amatrix sent the password in the Amiga character set
  instead of UTF-8. The same for signing out other sessions.
- Fixed: a reply showed "(an older message)" instead of the quote when it was
  loaded before the message it answers. Replies from clients that put the
  relation outside the encryption are now recognised.
- Fixed: picture lines in the chat read `[_Picture ...: name]`.
- New drawer, program, readme and guide icons.

## 0.1 (14.11.2026)

- First release.
