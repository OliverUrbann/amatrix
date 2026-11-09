/* Unit test of gui/richtext.c (runs on Linux: make richtext-test). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "richtext.h"

static int fails, n;

/* in the expectations '^' stands for RICH_MARK */
static void t(const char *html, const char *want)
{
    char *got = amx_rich_from_html(html), *q;
    n++;
    for (q = got; q && *q; q++) if (*q == RICH_MARK) *q = '^';
    if (!got || strcmp(got, want)) {
        fails++;
        printf("not ok %d\n  html: %s\n  want: %s\n  got:  %s\n", n, html, want, got ? got : "(null)");
    } else printf("ok %d\n", n);
    free(got);
}

int main(void)
{
    t("Hallo <b>fett</b> und <em>kursiv</em>.", "Hallo ^bfett^n und ^ikursiv^n.");
    t("<strong>a <i>b</i> c</strong>", "^ba ^ib^n^b c^n");
    t("<p>Erster</p>\n<p>Zweiter</p>", "Erster\nZweiter");
    t("eins<br>zwei<br/>drei", "eins\nzwei\ndrei");
    t("<b>fett<br>weiter</b>", "^bfett\n^bweiter^n");
    t("<ul><li>A</li><li>B <b>x</b></li></ul>", "- A\n- B ^bx^n");
    t("<ol start=\"3\"><li>drei</li><li>vier<ul><li>innen</li></ul></li></ol>", "3. drei\n4. vier\n  - innen");
    t("<blockquote><p>Zitat</p><p>noch eins</p></blockquote><p>Antwort</p>", "| Zitat\n| noch eins\nAntwort");
    t("<pre><code class=\"language-c\">int x;\n  x = 1;\n</code></pre>danach", "    int x;\n      x = 1;\ndanach");
    t("Nimm <code>ls -l</code> dafür", "Nimm `ls -l` dafür");
    t("<h2>Titel</h2>Text", "^bTitel^n\nText");
    t("a &amp; b &lt;c&gt; &quot;d&quot; &#8364; &#x41; &auml;", "a & b <c> \"d\" \xe2\x82\xac A \xc3\xa4");
    t("<mx-reply><blockquote>alt</blockquote></mx-reply>Neu", "Neu");
    t("Hi <a href=\"https://matrix.to/#/@alice:example.org\">Alice</a>!", "Hi ^bAlice^n!");
    t("Siehe <a href=\"https://example.org/x?a=1&amp;b=2\">hier</a>.", "Siehe hier (https://example.org/x?a=1&b=2).");
    t("<a href=\"https://example.org\">https://example.org</a>", "https://example.org");
    t("geheim: <span data-mx-spoiler>Darth</span>!", "geheim: [Spoiler]!");
    t("<del>weg</del> da", "~weg~ da");
    t("viele     Leerzeichen\n und Umbruch", "viele Leerzeichen und Umbruch");
    t("b\033o[1@5]se \001b", "bo[1@5]se b");
    t("<u>unter</u>strichen", "^uunter^nstrichen");
    t("kaputt <b>offen", "kaputt ^boffen");
    t("x < y", "x < y");
    t("<font color=\"#ff0000\">rot</font> <span>s</span>", "rot s");
    t("Text<hr>mehr", "Text\n----\nmehr");
    t("", "");
    printf("%d tests, %d failed\n", n, fails);
    return fails ? 1 : 0;
}
