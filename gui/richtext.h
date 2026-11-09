/* richtext.h - Matrix HTML (formatted_body) to text with style marks. */
#ifndef AMX_RICHTEXT_H
#define AMX_RICHTEXT_H

/* The mark that starts a style code in the result: RICH_MARK followed by
 * 'b' (bold), 'i' (italic), 'u' (underline) or 'n' (normal).  The GUI turns
 * it into ESC after it has removed every ESC a sender put into the text. */
#define RICH_MARK '\001'

/* HTML (UTF-8) -> UTF-8 text: bold/italic/underline as marks (set again at
 * the start of every line), lists, quotes, code blocks and headings as
 * lines, links as "text (URL)", mention pills bold, the reply fallback
 * (<mx-reply>) and spoilers left out.  malloc()ed; NULL if out of memory. */
char *amx_rich_from_html(const char *html);

#endif
