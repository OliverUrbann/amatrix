/* Unit test for gui/markdown.c (runs on Linux: make markdown-test) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "markdown.h"

static int fails, total;

static void t(const char *in, const char *want)
{
    char *got = md_to_html(in);
    total++;
    if ((!want && got) || (want && (!got || strcmp(got, want)))) {
        fails++;
        printf("FAIL %s\n  want %s\n  got  %s\n", in, want ? want : "(plain)", got ? got : "(plain)");
    }
    free(got);
}

int main(void)
{
    t("Hallo Welt", NULL);
    t("snake_case_name und a*b", NULL);
    t("2 * 3 * 4", NULL);
    t("**fett** und *kursiv*", "<strong>fett</strong> und <em>kursiv</em>");
    t("_kursiv_ und __fett__", "<em>kursiv</em> und <strong>fett</strong>");
    t("~~weg~~", "<del>weg</del>");
    t("Code: `a < b`", "Code: <code>a &lt; b</code>");
    t("[Amiga](https://amiga.org)", "<a href=\"https://amiga.org\">Amiga</a>");
    t("https://example.org/a_b_c*d", NULL);
    t("siehe https://x.org/__init__ **jetzt**", "siehe https://x.org/__init__ <strong>jetzt</strong>");
    t("- eins\n- zwei", "<ul><li>eins</li><li>zwei</li></ul>");
    t("1. eins\n2. zwei\nDanach", "<ol><li>eins</li><li>zwei</li></ol>Danach");
    t("> zitiert\n> weiter\nantwort", "<blockquote>zitiert<br>weiter</blockquote>antwort");
    t("# Titel\ntext", "<h1>Titel</h1>text");
    t("```\nint a<b;\n```", "<pre><code>int a&lt;b;\n</code></pre>");
    t("zeile1\n**zeile2**", "zeile1<br><strong>zeile2</strong>");
    t("\\*kein\\*", "*kein*");
    t("Größe **ä**", "Größe <strong>ä</strong>");
    {
        const char *names[] = { "Alice" }, *ids[] = { "@alice:x.org" };
        char *h = md_mentions("Hallo Alice, <b>Alice</b>", names, ids, 1);
        total++;
        if (strcmp(h, "Hallo <a href=\"https://matrix.to/#/@alice:x.org\">Alice</a>, <b>Alice</b>")) {
            fails++;
            printf("FAIL mention: %s\n", h);
        }
        free(h);
        h = md_escape("a<b\nc");
        total++;
        if (strcmp(h, "a&lt;b<br>c")) { fails++; printf("FAIL escape: %s\n", h); }
        free(h);
    }
    printf("markdown: %d/%d ok\n", total - fails, total);
    return fails != 0;
}
