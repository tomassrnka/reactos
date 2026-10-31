/*
 * The subset of XPath 1.0 that event log queries use, over the XML of one event.
 *
 * Copyright 2026 the reactos-nitro authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <math.h>

#include "wevtapi_private.h"

/*
 * What is supported: location paths with the child, attribute, self, parent and descendant-or-self
 * axes, name and * tests, predicates, the operators or and = != < <= > >= + - * div mod and |,
 * string and number literals, and these functions: position last count not true false string number
 * boolean contains starts-with string-length and, as Windows has them, band and timediff.
 * Comparisons of two dates in the form of SystemTime compare the instants, not the text.
 */

#define MAX_EXPR_NODES 1024

/* The XML of an event */

enum { X_DOC, X_ELEM, X_ATTR };

struct xnode
{
    int kind;
    WCHAR *name;
    WCHAR *text;                    /* of an element without children, the value of an attribute */
    struct xnode *parent;
    struct xnode **kids;
    DWORD kid_count, kid_cap;
    struct xnode **attrs;
    DWORD attr_count;
};

static struct xnode *new_xnode(int kind, struct xnode *parent, const WCHAR *name, SIZE_T name_len)
{
    struct xnode *node = calloc(1, sizeof(*node));

    if (!node) return NULL;
    node->kind = kind;
    node->parent = parent;
    if (name && !(node->name = calloc(name_len + 1, sizeof(WCHAR))))
    {
        free(node);
        return NULL;
    }
    if (name_len) memcpy(node->name, name, name_len * sizeof(WCHAR));
    return node;
}

void xml_free(struct xnode *node)
{
    DWORD i;

    if (!node) return;
    for (i = 0; i < node->kid_count; i++) xml_free(node->kids[i]);
    for (i = 0; i < node->attr_count; i++) xml_free(node->attrs[i]);
    free(node->kids);
    free(node->attrs);
    free(node->name);
    free(node->text);
    free(node);
}

static BOOL add_ptr(struct xnode ***array, DWORD *count, DWORD *cap, struct xnode *node)
{
    if (cap && *count == *cap)
    {
        DWORD new_cap = *cap ? *cap * 2 : 8;
        struct xnode **p = realloc(*array, new_cap * sizeof(*p));

        if (!p) return FALSE;
        *array = p;
        *cap = new_cap;
    }
    else if (!cap)
    {
        struct xnode **p = realloc(*array, (*count + 1) * sizeof(*p));

        if (!p) return FALSE;
        *array = p;
    }
    (*array)[(*count)++] = node;
    return TRUE;
}

/* The text of [start, end) with the five entities resolved, or NULL */
static WCHAR *decode_xml_text(const WCHAR *start, const WCHAR *end)
{
    WCHAR *out = calloc(end - start + 1, sizeof(WCHAR)), *o = out;

    if (!out) return NULL;
    while (start < end)
    {
        if (*start != '&') { *o++ = *start++; continue; }
        if (end - start >= 4 && !wcsncmp(start, L"&lt;", 4)) { *o++ = '<'; start += 4; }
        else if (end - start >= 4 && !wcsncmp(start, L"&gt;", 4)) { *o++ = '>'; start += 4; }
        else if (end - start >= 5 && !wcsncmp(start, L"&amp;", 5)) { *o++ = '&'; start += 5; }
        else if (end - start >= 6 && !wcsncmp(start, L"&quot;", 6)) { *o++ = '"'; start += 6; }
        else if (end - start >= 6 && !wcsncmp(start, L"&apos;", 6)) { *o++ = '\''; start += 6; }
        else { *o++ = *start++; }
    }
    *o = 0;
    return out;
}

static BOOL append_text(struct xnode *node, const WCHAR *start, const WCHAR *end)
{
    WCHAR *add, *joined;
    SIZE_T old = node->text ? wcslen(node->text) : 0, len;

    if (!(add = decode_xml_text(start, end))) return FALSE;
    len = wcslen(add);
    if (!len) { free(add); return TRUE; }
    if (!(joined = realloc(node->text, (old + len + 1) * sizeof(WCHAR)))) { free(add); return FALSE; }
    memcpy(joined + old, add, (len + 1) * sizeof(WCHAR));
    node->text = joined;
    free(add);
    return TRUE;
}

static BOOL is_space(WCHAR c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static BOOL is_name_start(WCHAR c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c > 0x7f;
}

static BOOL is_name_char(WCHAR c)
{
    return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

struct xnode *xml_parse_event(const WCHAR *xml)
{
    struct xnode *doc = new_xnode(X_DOC, NULL, NULL, 0), *cur = doc;
    const WCHAR *p = xml, *end;

    if (!doc) return NULL;
    while (*p)
    {
        if (*p != '<')
        {
            for (end = p; *end && *end != '<'; end++) {}
            if (cur->kind == X_ELEM && !append_text(cur, p, end)) goto error;
            p = end;
            continue;
        }
        if (!wcsncmp(p, L"<?", 2))
        {
            if (!(end = wcsstr(p, L"?>"))) goto error;
            p = end + 2;
        }
        else if (!wcsncmp(p, L"<!--", 4))
        {
            if (!(end = wcsstr(p, L"-->"))) goto error;
            p = end + 3;
        }
        else if (p[1] == '/')
        {
            if (cur->kind != X_ELEM) goto error;
            for (end = p + 2; *end && *end != '>'; end++) {}
            if (!*end) goto error;
            cur = cur->parent;
            p = end + 1;
        }
        else
        {
            const WCHAR *name = p + 1;
            struct xnode *elem;
            BOOL empty = FALSE;

            for (end = name; is_name_char(*end); end++) {}
            if (end == name || !(elem = new_xnode(X_ELEM, cur, name, end - name))) goto error;
            if (!add_ptr(&cur->kids, &cur->kid_count, &cur->kid_cap, elem)) { xml_free(elem); goto error; }
            p = end;
            for (;;)
            {
                const WCHAR *aname, *value;
                WCHAR quote;
                struct xnode *attr;

                while (is_space(*p)) p++;
                if (*p == '>') { p++; break; }
                if (*p == '/' && p[1] == '>') { p += 2; empty = TRUE; break; }
                aname = p;
                while (is_name_char(*p) || *p == ':') p++;
                if (p == aname) goto error;
                end = p;
                while (is_space(*p)) p++;
                if (*p++ != '=') goto error;
                while (is_space(*p)) p++;
                quote = *p++;
                if (quote != '\'' && quote != '"') goto error;
                value = p;
                while (*p && *p != quote) p++;
                if (!*p) goto error;
                /* a namespace declaration is no attribute of an element, as far as a path is concerned */
                if (end - aname == 5 && !wcsncmp(aname, L"xmlns", 5)) { p++; continue; }
                if (!(attr = new_xnode(X_ATTR, elem, aname, end - aname))) goto error;
                if (!(attr->text = decode_xml_text(value, p)) || !add_ptr(&elem->attrs, &elem->attr_count, NULL, attr))
                {
                    xml_free(attr);
                    goto error;
                }
                p++;
            }
            if (!empty) cur = elem;
        }
    }
    if (cur != doc) goto error;
    return doc;

error:
    xml_free(doc);
    return NULL;
}

/* The expressions */

enum expr_kind
{
    E_OR, E_AND, E_EQ, E_NE, E_LT, E_LE, E_GT, E_GE, E_ADD, E_SUB, E_MUL, E_DIV, E_MOD, E_NEG, E_UNION,
    E_STR, E_NUM, E_FUNC, E_PATH
};

enum axis { A_CHILD, A_ATTR, A_SELF, A_PARENT, A_DESC_SELF };

enum func
{
    F_POSITION, F_LAST, F_COUNT, F_NOT, F_TRUE, F_FALSE, F_BAND, F_TIMEDIFF, F_CONTAINS, F_STARTS_WITH,
    F_STRING_LENGTH, F_STRING, F_NUMBER, F_BOOLEAN
};

static const struct { const WCHAR *name; enum func func; int min_args, max_args; } functions[] =
{
    { L"position", F_POSITION, 0, 0 }, { L"last", F_LAST, 0, 0 }, { L"count", F_COUNT, 1, 1 },
    { L"not", F_NOT, 1, 1 }, { L"true", F_TRUE, 0, 0 }, { L"false", F_FALSE, 0, 0 },
    { L"band", F_BAND, 2, 2 }, { L"timediff", F_TIMEDIFF, 1, 1 }, { L"contains", F_CONTAINS, 2, 2 },
    { L"starts-with", F_STARTS_WITH, 2, 2 }, { L"string-length", F_STRING_LENGTH, 0, 1 },
    { L"string", F_STRING, 0, 1 }, { L"number", F_NUMBER, 0, 1 }, { L"boolean", F_BOOLEAN, 1, 1 },
};

struct expr;

struct step
{
    enum axis axis;
    WCHAR *name;                    /* NULL for any */
    BOOL no_nodes;                  /* text(): never selects anything here */
    struct expr **preds;
    DWORD pred_count;
};

struct expr
{
    enum expr_kind kind;
    struct expr *a, *b;
    WCHAR *str;                     /* literal text */
    double num;
    enum func func;
    struct expr **args;
    DWORD arg_count;
    BOOL absolute;
    struct step *steps;
    DWORD step_count;
};

struct xpath
{
    struct expr *root;
};

static void free_expr(struct expr *e)
{
    DWORD i, j;

    if (!e) return;
    free_expr(e->a);
    free_expr(e->b);
    for (i = 0; i < e->arg_count; i++) free_expr(e->args[i]);
    free(e->args);
    for (i = 0; i < e->step_count; i++)
    {
        for (j = 0; j < e->steps[i].pred_count; j++) free_expr(e->steps[i].preds[j]);
        free(e->steps[i].preds);
        free(e->steps[i].name);
    }
    free(e->steps);
    free(e->str);
    free(e);
}

void xpath_free(struct xpath *path)
{
    if (!path) return;
    free_expr(path->root);
    free(path);
}

struct parser
{
    const WCHAR *p;
    BOOL fail;
    DWORD depth;
    DWORD nodes;
};

static struct expr *parse_expr(struct parser *ps);

static struct expr *new_expr(struct parser *ps, enum expr_kind kind)
{
    struct expr *e;

    if (ps->fail || ++ps->nodes > MAX_EXPR_NODES) { ps->fail = TRUE; return NULL; }
    if (!(e = calloc(1, sizeof(*e)))) ps->fail = TRUE;
    else e->kind = kind;
    return e;
}

static struct expr *binary(struct parser *ps, enum expr_kind kind, struct expr *a, struct expr *b)
{
    struct expr *e;

    if (!a || !b || !(e = new_expr(ps, kind))) { free_expr(a); free_expr(b); ps->fail = TRUE; return NULL; }
    e->a = a;
    e->b = b;
    return e;
}

static void skip_ws(struct parser *ps)
{
    while (is_space(*ps->p)) ps->p++;
}

/* A word that stands alone, as an operator does */
static BOOL take_word(struct parser *ps, const WCHAR *word)
{
    SIZE_T len = wcslen(word);

    skip_ws(ps);
    if (wcsncmp(ps->p, word, len) || is_name_char(ps->p[len])) return FALSE;
    ps->p += len;
    return TRUE;
}

static BOOL take_char(struct parser *ps, WCHAR c)
{
    skip_ws(ps);
    if (*ps->p != c) return FALSE;
    ps->p++;
    return TRUE;
}

static BOOL take_str(struct parser *ps, const WCHAR *s)
{
    SIZE_T len = wcslen(s);

    skip_ws(ps);
    if (wcsncmp(ps->p, s, len)) return FALSE;
    ps->p += len;
    return TRUE;
}

static WCHAR *read_name(struct parser *ps)
{
    const WCHAR *start = ps->p;
    WCHAR *name;

    if (!is_name_start(*ps->p)) return NULL;
    while (is_name_char(*ps->p)) ps->p++;
    if (!(name = calloc(ps->p - start + 1, sizeof(WCHAR)))) { ps->fail = TRUE; return NULL; }
    memcpy(name, start, (ps->p - start) * sizeof(WCHAR));
    return name;
}

static BOOL add_pred(struct step *step, struct expr *pred)
{
    struct expr **p = realloc(step->preds, (step->pred_count + 1) * sizeof(*p));

    if (!p) return FALSE;
    step->preds = p;
    p[step->pred_count++] = pred;
    return TRUE;
}

static void free_step(struct step *step)
{
    DWORD i;

    for (i = 0; i < step->pred_count; i++) free_expr(step->preds[i]);
    free(step->preds);
    free(step->name);
    memset(step, 0, sizeof(*step));
}

/* A step is parsed into the given storage, which the caller owns when it fails */
static BOOL parse_step(struct parser *ps, struct step *step)
{
    memset(step, 0, sizeof(*step));
    skip_ws(ps);
    if (ps->p[0] == '.' && ps->p[1] == '.') { ps->p += 2; step->axis = A_PARENT; }
    else if (ps->p[0] == '.') { ps->p++; step->axis = A_SELF; }
    else
    {
        if (*ps->p == '@') { ps->p++; step->axis = A_ATTR; }
        else step->axis = A_CHILD;

        skip_ws(ps);
        if (*ps->p == '*') ps->p++;
        else
        {
            WCHAR *name = read_name(ps);
            const WCHAR *save;

            if (!name) { ps->fail = TRUE; return FALSE; }
            save = ps->p;
            if (step->axis == A_CHILD && ps->p[0] == ':' && ps->p[1] == ':')
            {
                ps->p += 2;
                if (!wcscmp(name, L"child")) step->axis = A_CHILD;
                else if (!wcscmp(name, L"attribute")) step->axis = A_ATTR;
                else if (!wcscmp(name, L"self")) step->axis = A_SELF;
                else if (!wcscmp(name, L"parent")) step->axis = A_PARENT;
                else if (!wcscmp(name, L"descendant-or-self")) step->axis = A_DESC_SELF;
                else { free(name); ps->fail = TRUE; return FALSE; }
                free(name);
                skip_ws(ps);
                if (*ps->p == '*') { ps->p++; name = NULL; }
                else if (!(name = read_name(ps))) { ps->fail = TRUE; return FALSE; }
            }
            else if (*save == '(' )
            {
                /* node() and text() are tests, not functions */
                BOOL text = !wcscmp(name, L"text"), node = !wcscmp(name, L"node");

                free(name);
                name = NULL;
                if ((!text && !node) || ps->p[1] != ')') { ps->fail = TRUE; return FALSE; }
                ps->p += 2;
                step->no_nodes = text;
            }
            step->name = name;
        }
    }

    for (;;)
    {
        struct expr *pred;

        skip_ws(ps);
        if (*ps->p != '[') break;
        ps->p++;
        pred = parse_expr(ps);
        if (!pred || !take_char(ps, ']')) { free_expr(pred); ps->fail = TRUE; free_step(step); return FALSE; }
        if (!add_pred(step, pred)) { free_expr(pred); ps->fail = TRUE; free_step(step); return FALSE; }
    }
    return TRUE;
}

static BOOL add_step(struct expr *e, const struct step *step)
{
    struct step *p = realloc(e->steps, (e->step_count + 1) * sizeof(*p));

    if (!p) return FALSE;
    e->steps = p;
    p[e->step_count++] = *step;
    return TRUE;
}

static BOOL is_step_start(const struct parser *ps)
{
    WCHAR c = *ps->p;

    return c == '.' || c == '@' || c == '*' || is_name_start(c);
}

static struct expr *parse_path(struct parser *ps)
{
    struct expr *e = new_expr(ps, E_PATH);
    struct step step;

    if (!e) return NULL;
    skip_ws(ps);
    if (*ps->p == '/')
    {
        e->absolute = TRUE;
        ps->p++;
        if (*ps->p == '/')
        {
            memset(&step, 0, sizeof(step));
            step.axis = A_DESC_SELF;
            ps->p++;
            if (!add_step(e, &step)) goto error;
        }
        else
        {
            skip_ws(ps);
            if (!is_step_start(ps)) return e;
        }
    }
    for (;;)
    {
        if (!parse_step(ps, &step)) goto error;
        if (!add_step(e, &step)) { free_step(&step); goto error; }
        skip_ws(ps);
        if (*ps->p != '/') break;
        ps->p++;
        if (*ps->p == '/')
        {
            ps->p++;
            memset(&step, 0, sizeof(step));
            step.axis = A_DESC_SELF;
            if (!add_step(e, &step)) goto error;
        }
    }
    return e;

error:
    ps->fail = TRUE;
    free_expr(e);
    return NULL;
}

static struct expr *parse_primary(struct parser *ps)
{
    struct expr *e;
    const WCHAR *p;

    skip_ws(ps);
    p = ps->p;
    if (*p == '(')
    {
        ps->p++;
        if (!(e = parse_expr(ps))) return NULL;
        if (!take_char(ps, ')')) { free_expr(e); ps->fail = TRUE; return NULL; }
        return e;
    }
    if (*p == '\'' || *p == '"')
    {
        const WCHAR *end = wcschr(p + 1, *p);

        if (!end) { ps->fail = TRUE; return NULL; }
        if (!(e = new_expr(ps, E_STR))) return NULL;
        if (!(e->str = calloc(end - p, sizeof(WCHAR)))) { ps->fail = TRUE; free_expr(e); return NULL; }
        memcpy(e->str, p + 1, (end - p - 1) * sizeof(WCHAR));
        ps->p = end + 1;
        return e;
    }
    if ((*p >= '0' && *p <= '9') || (*p == '.' && p[1] >= '0' && p[1] <= '9'))
    {
        const WCHAR *end = p;

        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        {
            end = p + 2;
            while ((*end >= '0' && *end <= '9') || (*end >= 'a' && *end <= 'f') || (*end >= 'A' && *end <= 'F')) end++;
        }
        else
        {
            while ((*end >= '0' && *end <= '9') || *end == '.') end++;
        }
        if (!(e = new_expr(ps, E_NUM))) return NULL;
        if (!(e->str = calloc(end - p + 1, sizeof(WCHAR)))) { ps->fail = TRUE; free_expr(e); return NULL; }
        memcpy(e->str, p, (end - p) * sizeof(WCHAR));
        e->num = wcstod(e->str, NULL);
        if (end - p > 2 && (p[1] == 'x' || p[1] == 'X'))
        {
            ULONGLONG v = 0;
            const WCHAR *d;

            for (d = p + 2; d < end; d++)
                v = v * 16 + (*d <= '9' ? *d - '0' : (*d | 0x20) - 'a' + 10);
            e->num = (double)v;
        }
        ps->p = end;
        return e;
    }
    if (is_name_start(*p))
    {
        /* a name followed by an opening parenthesis is a function, unless it is a node test */
        const WCHAR *q = p;
        DWORD i;

        while (is_name_char(*q)) q++;
        {
            const WCHAR *r = q;

            while (is_space(*r)) r++;
            if (*r == '(' && !(q - p == 4 && (!wcsncmp(p, L"text", 4) || !wcsncmp(p, L"node", 4))))
            {
                SIZE_T len = q - p;

                for (i = 0; i < ARRAY_SIZE(functions); i++)
                    if (wcslen(functions[i].name) == len && !_wcsnicmp(p, functions[i].name, len)) break;
                if (i == ARRAY_SIZE(functions)) { ps->fail = TRUE; return NULL; }
                if (!(e = new_expr(ps, E_FUNC))) return NULL;
                e->func = functions[i].func;
                ps->p = r + 1;
                skip_ws(ps);
                if (*ps->p == ')') ps->p++;
                else for (;;)
                {
                    struct expr *arg = parse_expr(ps), **args;

                    if (!arg) { free_expr(e); return NULL; }
                    if (!(args = realloc(e->args, (e->arg_count + 1) * sizeof(*args)))) { free_expr(arg); free_expr(e); ps->fail = TRUE; return NULL; }
                    e->args = args;
                    args[e->arg_count++] = arg;
                    if (take_char(ps, ',')) continue;
                    if (take_char(ps, ')')) break;
                    free_expr(e);
                    ps->fail = TRUE;
                    return NULL;
                }
                if ((int)e->arg_count < functions[i].min_args || (int)e->arg_count > functions[i].max_args)
                {
                    free_expr(e);
                    ps->fail = TRUE;
                    return NULL;
                }
                return e;
            }
        }
    }
    return parse_path(ps);
}

static struct expr *parse_union(struct parser *ps)
{
    struct expr *e = parse_primary(ps);

    while (e && take_char(ps, '|')) e = binary(ps, E_UNION, e, parse_primary(ps));
    return e;
}

static struct expr *parse_unary(struct parser *ps)
{
    struct expr *e;

    skip_ws(ps);
    if (*ps->p == '-')
    {
        ps->p++;
        if (!(e = new_expr(ps, E_NEG))) return NULL;
        if (!(e->a = parse_unary(ps))) { free_expr(e); return NULL; }
        return e;
    }
    return parse_union(ps);
}

static struct expr *parse_mul(struct parser *ps)
{
    struct expr *e = parse_unary(ps);

    while (e)
    {
        if (take_char(ps, '*')) e = binary(ps, E_MUL, e, parse_unary(ps));
        else if (take_word(ps, L"div")) e = binary(ps, E_DIV, e, parse_unary(ps));
        else if (take_word(ps, L"mod")) e = binary(ps, E_MOD, e, parse_unary(ps));
        else break;
    }
    return e;
}

static struct expr *parse_add(struct parser *ps)
{
    struct expr *e = parse_mul(ps);

    while (e)
    {
        if (take_char(ps, '+')) e = binary(ps, E_ADD, e, parse_mul(ps));
        else if (take_char(ps, '-')) e = binary(ps, E_SUB, e, parse_mul(ps));
        else break;
    }
    return e;
}

static struct expr *parse_rel(struct parser *ps)
{
    struct expr *e = parse_add(ps);

    while (e)
    {
        if (take_str(ps, L"<=")) e = binary(ps, E_LE, e, parse_add(ps));
        else if (take_str(ps, L">=")) e = binary(ps, E_GE, e, parse_add(ps));
        else if (take_char(ps, '<')) e = binary(ps, E_LT, e, parse_add(ps));
        else if (take_char(ps, '>')) e = binary(ps, E_GT, e, parse_add(ps));
        else break;
    }
    return e;
}

static struct expr *parse_eq(struct parser *ps)
{
    struct expr *e = parse_rel(ps);

    while (e)
    {
        if (take_str(ps, L"!=")) e = binary(ps, E_NE, e, parse_rel(ps));
        else if (take_char(ps, '=')) e = binary(ps, E_EQ, e, parse_rel(ps));
        else break;
    }
    return e;
}

static struct expr *parse_and(struct parser *ps)
{
    struct expr *e = parse_eq(ps);

    while (e && take_word(ps, L"and")) e = binary(ps, E_AND, e, parse_eq(ps));
    return e;
}

static struct expr *parse_or(struct parser *ps)
{
    struct expr *e = parse_and(ps);

    while (e && take_word(ps, L"or")) e = binary(ps, E_OR, e, parse_and(ps));
    return e;
}

static struct expr *parse_expr(struct parser *ps)
{
    struct expr *e;

    if (ps->fail || ++ps->depth > EVT_MAX_XPATH_DEPTH) { ps->fail = TRUE; return NULL; }
    e = parse_or(ps);
    ps->depth--;
    return e;
}

struct xpath *xpath_compile(const WCHAR *text)
{
    struct parser ps = { text, FALSE, 0, 0 };
    struct xpath *path;
    struct expr *e;

    if (!text || wcslen(text) > EVT_MAX_XPATH_LEN) return NULL;
    e = parse_expr(&ps);
    skip_ws(&ps);
    if (!e || ps.fail || *ps.p)
    {
        free_expr(e);
        return NULL;
    }
    if (!(path = calloc(1, sizeof(*path)))) { free_expr(e); return NULL; }
    path->root = e;
    return path;
}

/* Evaluation */

struct block
{
    struct block *next;
};

struct ctx
{
    struct block *blocks;
    struct xnode *doc;
    BOOL fail;
};

static void *ctx_alloc(struct ctx *ctx, SIZE_T size)
{
    struct block *b;

    if (ctx->fail || size > MAXDWORD / 2 || !(b = calloc(1, sizeof(*b) + size))) { ctx->fail = TRUE; return NULL; }
    b->next = ctx->blocks;
    ctx->blocks = b;
    return b + 1;
}

static void ctx_free(struct ctx *ctx)
{
    while (ctx->blocks)
    {
        struct block *b = ctx->blocks;

        ctx->blocks = b->next;
        free(b);
    }
}

enum vtype { V_NODES, V_STR, V_NUM, V_BOOL };

struct value
{
    enum vtype type;
    struct xnode **nodes;
    DWORD count;
    const WCHAR *str;
    double num;
    BOOL b;
};

static const WCHAR *node_string(struct ctx *ctx, const struct xnode *node)
{
    SIZE_T len = 0, pos = 0;
    DWORD i;
    WCHAR *out;

    if (node->kind == X_ATTR || !node->kid_count) return node->text ? node->text : L"";
    for (i = 0; i < node->kid_count; i++) len += wcslen(node_string(ctx, node->kids[i]));
    if (!(out = ctx_alloc(ctx, (len + 1) * sizeof(WCHAR)))) return L"";
    for (i = 0; i < node->kid_count; i++)
    {
        const WCHAR *s = node_string(ctx, node->kids[i]);

        memcpy(out + pos, s, wcslen(s) * sizeof(WCHAR));
        pos += wcslen(s);
    }
    out[pos] = 0;
    return out;
}

/* Windows takes 0x as hexadecimal in numbers of text, which keywords need */
static BOOL parse_u64(const WCHAR *s, ULONGLONG *value)
{
    ULONGLONG v = 0;
    BOOL any = FALSE;

    while (is_space(*s)) s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        for (s += 2; (*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'); s++, any = TRUE)
            v = v * 16 + (*s <= '9' ? *s - '0' : (*s | 0x20) - 'a' + 10);
    }
    else
    {
        for (; *s >= '0' && *s <= '9'; s++, any = TRUE) v = v * 10 + (*s - '0');
    }
    while (is_space(*s)) s++;
    *value = v;
    return any && !*s;
}

static double string_to_number(const WCHAR *s)
{
    WCHAR *end;
    double d;
    ULONGLONG u;

    while (is_space(*s)) s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return parse_u64(s, &u) ? (double)u : NAN;
    if (!*s) return NAN;
    /* only the plain form of XPath: digits with a sign and a point */
    {
        const WCHAR *p = s;

        if (*p == '-') p++;
        if (!((*p >= '0' && *p <= '9') || (*p == '.' && p[1] >= '0' && p[1] <= '9'))) return NAN;
        for (; (*p >= '0' && *p <= '9') || *p == '.'; p++) {}
        while (is_space(*p)) p++;
        if (*p) return NAN;
    }
    d = wcstod(s, &end);
    return d;
}

static const WCHAR *value_string(struct ctx *ctx, const struct value *v)
{
    WCHAR *out;

    switch (v->type)
    {
    case V_NODES: return v->count ? node_string(ctx, v->nodes[0]) : L"";
    case V_STR: return v->str;
    case V_BOOL: return v->b ? L"true" : L"false";
    default:
        if (isnan(v->num)) return L"NaN";
        if (!(out = ctx_alloc(ctx, 40 * sizeof(WCHAR)))) return L"";
        if (v->num == floor(v->num) && fabs(v->num) < 1e15) _snwprintf(out, 40, L"%.0f", v->num);
        else _snwprintf(out, 40, L"%.15g", v->num);
        out[39] = 0;
        return out;
    }
}

static BOOL value_bool(const struct value *v)
{
    switch (v->type)
    {
    case V_NODES: return v->count != 0;
    case V_STR: return v->str[0] != 0;
    case V_BOOL: return v->b;
    default: return !isnan(v->num) && v->num != 0;
    }
}

static double value_number(struct ctx *ctx, const struct value *v)
{
    switch (v->type)
    {
    case V_NUM: return v->num;
    case V_BOOL: return v->b ? 1 : 0;
    default: return string_to_number(value_string(ctx, v));
    }
}

/* The instant of 2026-10-31T12:29:48.1752974Z in FILETIME units, for text of that form */
static BOOL parse_time(const WCHAR *s, ULONGLONG *time)
{
    int f[6], i;
    SYSTEMTIME st;
    FILETIME ft;
    ULONGLONG frac = 0, scale = 1000000;
    static const int len[6] = { 4, 2, 2, 2, 2, 2 };
    static const WCHAR sep[6] = { '-', '-', 'T', ':', ':', 0 };

    for (i = 0; i < 6; i++)
    {
        int j;

        f[i] = 0;
        for (j = 0; j < len[i]; j++, s++)
        {
            if (*s < '0' || *s > '9') return FALSE;
            f[i] = f[i] * 10 + (*s - '0');
        }
        if (sep[i])
        {
            if (*s++ != sep[i]) return FALSE;
        }
    }
    if (*s == '.')
    {
        s++;
        if (*s < '0' || *s > '9') return FALSE;
        for (; *s >= '0' && *s <= '9'; s++)
        {
            if (scale) { frac += (*s - '0') * scale; scale /= 10; }
        }
    }
    if (*s == 'Z') s++;
    if (*s) return FALSE;
    if (f[1] < 1 || f[1] > 12 || f[2] < 1 || f[2] > 31 || f[3] > 23 || f[4] > 59 || f[5] > 59) return FALSE;
    memset(&st, 0, sizeof(st));
    st.wYear = f[0]; st.wMonth = f[1]; st.wDay = f[2]; st.wHour = f[3]; st.wMinute = f[4]; st.wSecond = f[5];
    if (!SystemTimeToFileTime(&st, &ft)) return FALSE;
    *time = (((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) + frac;
    return TRUE;
}

struct atom
{
    enum vtype type;        /* never V_NODES */
    const WCHAR *str;
    double num;
    BOOL b;
};

static BOOL compare_atoms(enum expr_kind op, const struct atom *a, const struct atom *b)
{
    ULONGLONG ta, tb;
    int c;

    if (op == E_EQ || op == E_NE)
    {
        BOOL eq;

        if (a->type == V_BOOL || b->type == V_BOOL)
        {
            BOOL ba = a->type == V_BOOL ? a->b : a->type == V_NUM ? (!isnan(a->num) && a->num != 0) : a->str[0] != 0;
            BOOL bb = b->type == V_BOOL ? b->b : b->type == V_NUM ? (!isnan(b->num) && b->num != 0) : b->str[0] != 0;

            eq = ba == bb;
        }
        else if (a->type == V_NUM || b->type == V_NUM)
        {
            double da = a->type == V_NUM ? a->num : string_to_number(a->str);
            double db = b->type == V_NUM ? b->num : string_to_number(b->str);

            eq = da == db;
        }
        else if (parse_time(a->str, &ta) && parse_time(b->str, &tb)) eq = ta == tb;
        else eq = !_wcsicmp(a->str, b->str);     /* names of providers and channels are the same in whatever case */
        return op == E_EQ ? eq : !eq;
    }

    if (a->type == V_STR && b->type == V_STR && parse_time(a->str, &ta) && parse_time(b->str, &tb))
    {
        c = ta < tb ? -1 : ta > tb ? 1 : 0;
    }
    else
    {
        double da = a->type == V_NUM ? a->num : a->type == V_BOOL ? a->b : string_to_number(a->str);
        double db = b->type == V_NUM ? b->num : b->type == V_BOOL ? b->b : string_to_number(b->str);

        if (isnan(da) || isnan(db)) return FALSE;
        c = da < db ? -1 : da > db ? 1 : 0;
    }
    switch (op)
    {
    case E_LT: return c < 0;
    case E_LE: return c <= 0;
    case E_GT: return c > 0;
    default: return c >= 0;
    }
}

/* the atoms of a value: one for anything but a node set */
static DWORD atom_count(const struct value *v)
{
    return v->type == V_NODES ? v->count : 1;
}

static void get_atom(struct ctx *ctx, const struct value *v, DWORD i, struct atom *atom)
{
    memset(atom, 0, sizeof(*atom));
    switch (v->type)
    {
    case V_NODES: atom->type = V_STR; atom->str = node_string(ctx, v->nodes[i]); break;
    case V_STR: atom->type = V_STR; atom->str = v->str; break;
    case V_NUM: atom->type = V_NUM; atom->num = v->num; break;
    case V_BOOL: atom->type = V_BOOL; atom->b = v->b; break;
    }
}

static BOOL compare_values(struct ctx *ctx, enum expr_kind op, const struct value *a, const struct value *b)
{
    DWORD i, j;

    /* a set of nodes against a boolean is the boolean of the set */
    if ((a->type == V_NODES && b->type == V_BOOL) || (a->type == V_BOOL && b->type == V_NODES))
    {
        struct atom x = { V_BOOL, NULL, 0, value_bool(a) }, y = { V_BOOL, NULL, 0, value_bool(b) };

        return compare_atoms(op, &x, &y);
    }
    for (i = 0; i < atom_count(a); i++)
    {
        struct atom x;

        get_atom(ctx, a, i, &x);
        for (j = 0; j < atom_count(b); j++)
        {
            struct atom y;

            get_atom(ctx, b, j, &y);
            if (compare_atoms(op, &x, &y)) return TRUE;
        }
    }
    return FALSE;
}

static struct value eval(struct ctx *ctx, const struct expr *e, struct xnode *node, DWORD pos, DWORD size);

static struct value make_bool(BOOL b)
{
    struct value v = { V_BOOL };

    v.b = b;
    return v;
}

static struct value make_num(double d)
{
    struct value v = { V_NUM };

    v.num = d;
    return v;
}

static struct value make_nodes(void)
{
    struct value v = { V_NODES };

    return v;
}

static BOOL add_node(struct ctx *ctx, struct value *set, struct xnode *node)
{
    DWORD i;
    struct xnode **p;

    for (i = 0; i < set->count; i++)
        if (set->nodes[i] == node) return TRUE;
    /* the sets are small, and grow in steps of eight */
    if (!(set->count & 7))
    {
        if (!(p = ctx_alloc(ctx, (set->count + 8) * sizeof(*p)))) return FALSE;
        if (set->count) memcpy(p, set->nodes, set->count * sizeof(*p));
        set->nodes = p;
    }
    set->nodes[set->count++] = node;
    return TRUE;
}

static BOOL name_matches(const struct step *step, const struct xnode *n)
{
    return !step->name || !wcscmp(step->name, n->name);
}

static void collect_descendants(struct ctx *ctx, struct value *set, struct xnode *node)
{
    DWORD i;

    add_node(ctx, set, node);
    for (i = 0; i < node->kid_count && !ctx->fail; i++) collect_descendants(ctx, set, node->kids[i]);
}

static BOOL apply_preds(struct ctx *ctx, const struct step *step, struct value *cand)
{
    DWORD p;

    for (p = 0; p < step->pred_count; p++)
    {
        struct value kept = make_nodes();
        DWORD i;

        for (i = 0; i < cand->count && !ctx->fail; i++)
        {
            struct value r = eval(ctx, step->preds[p], cand->nodes[i], i + 1, cand->count);
            BOOL keep = r.type == V_NUM ? r.num == (double)(i + 1) : value_bool(&r);

            if (keep && !add_node(ctx, &kept, cand->nodes[i])) return FALSE;
        }
        *cand = kept;
    }
    return !ctx->fail;
}

static struct value eval_path(struct ctx *ctx, const struct expr *e, struct xnode *context)
{
    struct value cur = make_nodes();
    DWORD s, i;

    if (!add_node(ctx, &cur, e->absolute ? ctx->doc : context)) return cur;
    for (s = 0; s < e->step_count && !ctx->fail; s++)
    {
        const struct step *step = &e->steps[s];
        struct value next = make_nodes();

        for (i = 0; i < cur.count && !ctx->fail; i++)
        {
            struct xnode *n = cur.nodes[i];
            struct value cand = make_nodes();
            DWORD k;

            if (step->no_nodes) continue;
            switch (step->axis)
            {
            case A_CHILD:
                for (k = 0; k < n->kid_count; k++)
                    if (name_matches(step, n->kids[k])) add_node(ctx, &cand, n->kids[k]);
                break;
            case A_ATTR:
                for (k = 0; k < n->attr_count; k++)
                    if (name_matches(step, n->attrs[k])) add_node(ctx, &cand, n->attrs[k]);
                break;
            case A_SELF:
                add_node(ctx, &cand, n);
                break;
            case A_PARENT:
                if (n->parent) add_node(ctx, &cand, n->parent);
                break;
            case A_DESC_SELF:
                collect_descendants(ctx, &cand, n);
                break;
            }
            if (!apply_preds(ctx, step, &cand)) return next;
            for (k = 0; k < cand.count; k++) add_node(ctx, &next, cand.nodes[k]);
        }
        cur = next;
    }
    return cur;
}

static ULONGLONG expr_u64(struct ctx *ctx, const struct expr *e, struct xnode *node, DWORD pos, DWORD size)
{
    ULONGLONG u = 0;
    struct value v;

    if (e->kind == E_NUM && parse_u64(e->str, &u)) return u;
    v = eval(ctx, e, node, pos, size);
    if (v.type == V_NUM) return isnan(v.num) ? 0 : (ULONGLONG)(LONGLONG)v.num;
    parse_u64(value_string(ctx, &v), &u);
    return u;
}

static struct value eval_func(struct ctx *ctx, const struct expr *e, struct xnode *node, DWORD pos, DWORD size)
{
    struct value a, b, r = { V_STR };
    ULONGLONG t1, t2;
    FILETIME now;
    const WCHAR *s, *t;

    switch (e->func)
    {
    case F_POSITION: return make_num(pos);
    case F_LAST: return make_num(size);
    case F_TRUE: return make_bool(TRUE);
    case F_FALSE: return make_bool(FALSE);
    case F_COUNT:
        a = eval(ctx, e->args[0], node, pos, size);
        return make_num(a.type == V_NODES ? a.count : NAN);
    case F_NOT:
        a = eval(ctx, e->args[0], node, pos, size);
        return make_bool(!value_bool(&a));
    case F_BOOLEAN:
        a = eval(ctx, e->args[0], node, pos, size);
        return make_bool(value_bool(&a));
    case F_BAND:
        t1 = expr_u64(ctx, e->args[0], node, pos, size);
        t2 = expr_u64(ctx, e->args[1], node, pos, size);
        /* it stands in a predicate for the test of whether any of the bits is set, and a number there would be a position */
        return make_bool((t1 & t2) != 0);
    case F_TIMEDIFF:
        a = eval(ctx, e->args[0], node, pos, size);
        if (!parse_time(value_string(ctx, &a), &t1)) return make_num(NAN);
        GetSystemTimeAsFileTime(&now);
        t2 = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
        return make_num(((double)t2 - (double)t1) / 10000.0);
    case F_CONTAINS:
    case F_STARTS_WITH:
        a = eval(ctx, e->args[0], node, pos, size);
        b = eval(ctx, e->args[1], node, pos, size);
        s = value_string(ctx, &a);
        t = value_string(ctx, &b);
        if (e->func == F_CONTAINS) return make_bool(wcsstr(s, t) != NULL);
        return make_bool(!wcsncmp(s, t, wcslen(t)));
    case F_STRING_LENGTH:
    case F_STRING:
    case F_NUMBER:
        if (e->arg_count) a = eval(ctx, e->args[0], node, pos, size);
        else
        {
            a = make_nodes();
            add_node(ctx, &a, node);
        }
        if (e->func == F_STRING_LENGTH) return make_num((double)wcslen(value_string(ctx, &a)));
        if (e->func == F_NUMBER) return make_num(value_number(ctx, &a));
        r.str = value_string(ctx, &a);
        return r;
    }
    return make_bool(FALSE);
}

static struct value eval(struct ctx *ctx, const struct expr *e, struct xnode *node, DWORD pos, DWORD size)
{
    struct value a, b, r;
    DWORD i;

    if (ctx->fail) return make_bool(FALSE);
    switch (e->kind)
    {
    case E_OR:
        a = eval(ctx, e->a, node, pos, size);
        if (value_bool(&a)) return make_bool(TRUE);
        b = eval(ctx, e->b, node, pos, size);
        return make_bool(value_bool(&b));
    case E_AND:
        a = eval(ctx, e->a, node, pos, size);
        if (!value_bool(&a)) return make_bool(FALSE);
        b = eval(ctx, e->b, node, pos, size);
        return make_bool(value_bool(&b));
    case E_EQ: case E_NE: case E_LT: case E_LE: case E_GT: case E_GE:
        a = eval(ctx, e->a, node, pos, size);
        b = eval(ctx, e->b, node, pos, size);
        return make_bool(compare_values(ctx, e->kind, &a, &b));
    case E_ADD: case E_SUB: case E_MUL: case E_DIV: case E_MOD:
    {
        double x, y;

        a = eval(ctx, e->a, node, pos, size);
        b = eval(ctx, e->b, node, pos, size);
        x = value_number(ctx, &a);
        y = value_number(ctx, &b);
        switch (e->kind)
        {
        case E_ADD: return make_num(x + y);
        case E_SUB: return make_num(x - y);
        case E_MUL: return make_num(x * y);
        case E_DIV: return make_num(x / y);
        default: return make_num(fmod(x, y));
        }
    }
    case E_NEG:
        a = eval(ctx, e->a, node, pos, size);
        return make_num(-value_number(ctx, &a));
    case E_UNION:
        a = eval(ctx, e->a, node, pos, size);
        b = eval(ctx, e->b, node, pos, size);
        if (a.type != V_NODES || b.type != V_NODES) { ctx->fail = TRUE; return make_bool(FALSE); }
        r = a;
        for (i = 0; i < b.count; i++) add_node(ctx, &r, b.nodes[i]);
        return r;
    case E_STR:
        r.type = V_STR;
        r.str = e->str;
        return r;
    case E_NUM:
        return make_num(e->num);
    case E_FUNC:
        return eval_func(ctx, e, node, pos, size);
    case E_PATH:
        return eval_path(ctx, e, node);
    }
    return make_bool(FALSE);
}

BOOL xpath_match(const struct xpath *path, struct xnode *document)
{
    struct ctx ctx = { NULL, document, FALSE };
    struct value v = eval(&ctx, path->root, document, 1, 1);
    BOOL match = !ctx.fail && ((v.type == V_NODES && v.count) || (v.type == V_BOOL && v.b));

    ctx_free(&ctx);
    return match;
}

/* The text of the node that a path of names (Event/System/Provider/@Name) leads to, from the document */
const WCHAR *xml_value(struct xnode *doc, const WCHAR *path)
{
    struct xnode *cur = doc;
    const WCHAR *p = path;

    while (*p)
    {
        const WCHAR *end = wcschr(p, '/');
        SIZE_T len = end ? (SIZE_T)(end - p) : wcslen(p);
        struct xnode *found = NULL;
        DWORD i;

        if (*p == '@')
        {
            for (i = 0; i < cur->attr_count; i++)
                if (wcslen(cur->attrs[i]->name) == len - 1 && !wcsncmp(cur->attrs[i]->name, p + 1, len - 1)) found = cur->attrs[i];
            return found ? (found->text ? found->text : L"") : NULL;
        }
        for (i = 0; i < cur->kid_count; i++)
            if (cur->kids[i]->kind == X_ELEM && wcslen(cur->kids[i]->name) == len && !wcsncmp(cur->kids[i]->name, p, len))
            {
                found = cur->kids[i];
                break;
            }
        if (!found) return NULL;
        cur = found;
        p += len;
        if (*p == '/') p++;
    }
    return cur->text ? cur->text : L"";
}
