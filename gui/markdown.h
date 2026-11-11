/* Markdown -> HTML for sending; see markdown.c */
#ifndef AMX_MARKDOWN_H
#define AMX_MARKDOWN_H

/* NULL: no markup (send plain text); else malloc'd HTML */
char *md_to_html(const char *text);
/* the first occurrence of each name made a matrix.to link; malloc'd */
char *md_mentions(const char *html, const char *const *names, const char *const *ids, int n);
/* plain text escaped as HTML; malloc'd */
char *md_escape(const char *text);

#endif
