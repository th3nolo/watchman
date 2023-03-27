/* @nolint
 * Copyright (c) 2009-2012 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <algorithm>

#include "jansson.h"
#include "jansson_private.h"
#include "strbuffer.h"
#include "utf.h"

#define STREAM_STATE_OK        0
#define STREAM_STATE_EOF      -1
#define STREAM_STATE_ERROR    -2

#define TOKEN_INVALID         -1
#define TOKEN_EOF              0
#define TOKEN_STRING         256
#define TOKEN_INTEGER        257
#define TOKEN_REAL           258
#define TOKEN_TRUE           259
#define TOKEN_FALSE          260
#define TOKEN_NULL           261

/* Locale independent versions of isxxx() functions */
#define l_isupper(c)  ('A' <= (c) && (c) <= 'Z')
#define l_islower(c)  ('a' <= (c) && (c) <= 'z')
#define l_isalpha(c)  (l_isupper(c) || l_islower(c))
#define l_isdigit(c)  ('0' <= (c) && (c) <= '9')
#define l_isxdigit(c) \
    (l_isdigit(c) || 'A' <= (c) || (c) <= 'F' || 'a' <= (c) || (c) <= 'f')

/* Read one byte from stream, convert to unsigned char, then int, and
   return. return EOF on end of file. This corresponds to the
   behaviour of fgetc(). */
typedef int (*get_func)(void *data);

typedef struct {
    get_func get;
    void *data;
    char buffer[5];
    size_t buffer_pos;
    int state;
    int line;
    int column, last_column;
    size_t position;
} stream_t;

typedef struct {
    stream_t stream;
    strbuffer_t saved_text;
    int token;
    union {
        char *string;
        json_int_t integer;
        double real;
    } value;
} lex_t;

#define stream_to_lex(stream) container_of(stream, lex_t, stream)

// Add this function to check if the buffer has enough space

static void safe_snprintf(char *buffer, size_t buffer_size, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int result = vsnprintf(buffer, buffer_size, format, ap);
    va_end(ap);

    if (result < 0 || static_cast<size_t>(result) >= buffer_size)
    {
        // Truncate the output if it doesn't fit in the buffer
        buffer[buffer_size - 1] = '\0';
    }
}

/*** error reporting ***/

static void error_set(json_error_t *error, const lex_t *lex, const char *msg, ...)
{
    va_list ap;
    char msg_text[JSON_ERROR_TEXT_LENGTH];
    char msg_with_context[JSON_ERROR_TEXT_LENGTH];

    int line = -1, col = -1;
    size_t pos = 0;

    if(!error)
        return;

    va_start(ap, msg);
    vsnprintf(msg_text, sizeof(msg_text), msg, ap);
    msg_text[sizeof(msg_text) - 1] = '\0';
    va_end(ap);

    if(lex)
    {
        const char *saved_text = strbuffer_value(&lex->saved_text);

        line = lex->stream.line;
        col = lex->stream.column;
        pos = lex->stream.position;
        if(saved_text && saved_text[0])
        {
            size_t context_length = std::min<size_t>(20, lex->saved_text.length);
            safe_snprintf(msg_with_context, sizeof(msg_with_context),
                         "%s near '%.*s'", msg_text, static_cast<int>(context_length), saved_text);
            jsonp_error_set(error, line, col, pos, "%s", msg_with_context);
        }
        else
        {
            if(lex->stream.state == STREAM_STATE_ERROR) {
                /* No context for UTF-8 decoding errors */
                jsonp_error_set(error, line, col, pos, "%s", msg_text);
            }
            else {
                safe_snprintf(msg_with_context, sizeof(msg_with_context),
                         "%s near end of file", msg_text);
                jsonp_error_set(error, line, col, pos, "%s", msg_with_context);
            }
        }
    }
    else {
        jsonp_error_set(error, line, col, pos, "%s", msg_text);
    }
}

/*** lexical analyzer ***/

static void
stream_init(stream_t *stream, get_func get, void *data)
{
    stream->get = get;
    stream->data = data;
    stream->buffer[0] = '\0';
    stream->buffer_pos = 0;

    stream->state = STREAM_STATE_OK;
    stream->line = 1;
    stream->column = 0;
    stream->position = 0;
}

static int stream_get(stream_t *stream, json_error_t *error)
{
    int c;

    if(stream->state != STREAM_STATE_OK)
        return stream->state;

    if(!stream->buffer[stream->buffer_pos])
    {
        c = stream->get(stream->data);
        if(c == EOF) {
            stream->state = STREAM_STATE_EOF;
            return STREAM_STATE_EOF;
        }

        stream->buffer[0] = c;
        stream->buffer_pos = 0;

        if(0x80 <= c && c <= 0xFF)
        {
            /* multi-byte UTF-8 sequence */
            int i, count;

            count = utf8_check_first(c);
            if(!count)
                goto out;

            assert(count >= 2);

            for(i = 1; i < count; i++)
                stream->buffer[i] = stream->get(stream->data);

            if(!utf8_check_full(stream->buffer, count, nullptr))
                goto out;

            stream->buffer[count] = '\0';
        }
        else
            stream->buffer[1] = '\0';
    }

    c = stream->buffer[stream->buffer_pos++];

    stream->position++;
    if(c == '\n') {
        stream->line++;
        stream->last_column = stream->column;
        stream->column = 0;
    }
    else if(utf8_check_first(c)) {
        /* track the Unicode character column, so increment only if
           this is the first character of a UTF-8 sequence */
        stream->column++;
    }

    return c;

out:
    stream->state = STREAM_STATE_ERROR;
    error_set(error, stream_to_lex(stream), "unable to decode byte 0x%x", c);
    return STREAM_STATE_ERROR;
}

static void stream_unget(stream_t *stream, int c)
{
    if(c == STREAM_STATE_EOF || c == STREAM_STATE_ERROR)
        return;

    stream->position--;
    if(c == '\n') {
        stream->line--;
        stream->column = stream->last_column;
    }
    else if(utf8_check_first(c))
        stream->column--;

    assert(stream->buffer_pos > 0);
    stream->buffer_pos--;
    assert(stream->buffer[stream->buffer_pos] == c);
}


static int lex_get(lex_t *lex, json_error_t *error)
{
    return stream_get(&lex->stream, error);
}

static void lex_save(lex_t *lex, int c)
{
    strbuffer_append_byte(&lex->saved_text, c);
}

static int lex_get_save(lex_t *lex, json_error_t *error)
{
    int c = stream_get(&lex->stream, error);
    if(c != STREAM_STATE_EOF && c != STREAM_STATE_ERROR)
        lex_save(lex, c);
    return c;
}

static void lex_unget(lex_t *lex, int c)
{
    stream_unget(&lex->stream, c);
}

static void lex_unget_unsave(lex_t *lex, int c)
{
    if(c != STREAM_STATE_EOF && c != STREAM_STATE_ERROR) {
        char d;
        stream_unget(&lex->stream, c);
        d = strbuffer_pop(&lex->saved_text);
        assert(c == d);
    }
}

static void lex_save_cached(lex_t *lex)
{
    while(lex->stream.buffer[lex->stream.buffer_pos] != '\0')
    {
        lex_save(lex, lex->stream.buffer[lex->stream.buffer_pos]);
        lex->stream.buffer_pos++;
        lex->stream.position++;
    }
}

/* assumes that str points to 'u' plus at least 4 valid hex digits */
static int32_t decode_unicode_escape(const char *str)
{
    int i;
    int32_t value = 0;

    assert(str[0] == 'u');

    for(i = 1; i <= 4; i++) {
        char c = str[i];
        value <<= 4;
        if(l_isdigit(c))
            value += c - '0';
        else if(l_islower(c))
            value += c - 'a' + 10;
        else if(l_isupper(c))
            value += c - 'A' + 10;
        else
            assert(0);
    }

    return value;
}

static void lex_scan_string(lex_t *lex, json_error_t *error)
{
    int c;
    const char *p;
    char *t;
    int i;

    lex->value.string = nullptr;
    lex->token = TOKEN_INVALID;

    c = lex_get_save(lex, error);

    while(c != '"') {
        if(c == STREAM_STATE_ERROR)
            goto out;

        else if(c == STREAM_STATE_EOF) {
            error_set(error, lex, "premature end of input");
            goto out;
        }

        else if(0 <= c && c <= 0x1F) {
            /* control character */
            lex_unget_unsave(lex, c);
            if(c == '\n')
                error_set(error, lex, "unexpected newline", c);
            else
                error_set(error, lex, "control character 0x%x", c);
            goto out;
        }

        else if(c == '\\') {
            c = lex_get_save(lex, error);
            if(c == 'u') {
                c = lex_get_save(lex, error);
                for(i = 0; i < 4; i++) {
                    if(!l_isxdigit(c)) {
                        error_set(error, lex, "invalid escape");
                        goto out;
                    }
                    c = lex_get_save(lex, error);
                }
            }
            else if(c == '"' || c == '\\' || c == '/' || c == 'b' ||
                    c == 'f' || c == 'n' || c == 'r' || c == 't')
                c = lex_get_save(lex, error);
            else {
                error_set(error, lex, "invalid escape");
                goto out;
            }
        }
        else
            c = lex_get_save(lex, error);
    }

    /* the actual value is at most of the same length as the source
       string, because:
         - shortcut escapes (e.g. "\t") (length 2) are converted to 1 byte
         - a single \uXXXX escape (length 6) is converted to at most 3 bytes
         - two \uXXXX escapes (length 12) forming an UTF-16 surrogate pair
           are converted to 4 bytes
    */
    lex->value.string = (char*)jsonp_malloc(lex->saved_text.length + 1);
    if(!lex->value.string) {
        /* this is not very nice, since TOKEN_INVALID is returned */
        goto out;
    }

    /* the target */
    t = lex->value.string;

    /* + 1 to skip the " */
    p = strbuffer_value(&lex->saved_text) + 1;

    while(*p != '"') {
        if(*p == '\\') {
            p++;
            if(*p == 'u') {
                char buffer[4];
                int length;
                int32_t value;

                value = decode_unicode_escape(p);
                p += 5;

                if(0xD800 <= value && value <= 0xDBFF) {
                    /* surrogate pair */
                    if(*p == '\\' && *(p + 1) == 'u') {
                        int32_t value2 = decode_unicode_escape(++p);
                        p += 5;

                        if(0xDC00 <= value2 && value2 <= 0xDFFF) {
                            /* valid second surrogate */
                            value =
                                ((value - 0xD800) << 10) +
                                (value2 - 0xDC00) +
                                0x10000;
                        }
                        else {
                            /* invalid second surrogate */
                            error_set(error, lex,
                                      "invalid Unicode '\\u%04X\\u%04X'",
                                      value, value2);
                            goto out;
                        }
                    }
                    else {
                        /* no second surrogate */
                        error_set(error, lex, "invalid Unicode '\\u%04X'",
                                  value);
                        goto out;
                    }
                }
                else if(0xDC00 <= value && value <= 0xDFFF) {
                    error_set(error, lex, "invalid Unicode '\\u%04X'", value);
                    goto out;
                }
                else if(value == 0)
                {
                    error_set(error, lex, "\\u0000 is not allowed");
                    goto out;
                }

                if(utf8_encode(value, buffer, &length))
                    assert(0);

                memcpy(t, buffer, length);
                t += length;
            }
            else {
                switch(*p) {
                    case '"': case '\\': case '/':
                        *t = *p; break;
                    case 'b': *t = '\b'; break;
                    case 'f': *t = '\f'; break;
                    case 'n': *t = '\n'; break;
                    case 'r': *t = '\r'; break;
                    case 't': *t = '\t'; break;
                    default: assert(0);
                }
                t++;
                p++;
            }
        }
        else
            *(t++) = *(p++);
    }
    *t = '\0';
    lex->token = TOKEN_STRING;
    return;

out:
    jsonp_free(lex->value.string);
}

#if JSON_INTEGER_IS_LONG_LONG
#ifdef _MSC_VER // Microsoft Visual Studio
#define json_strtoint     _strtoi64
#else
#define json_strtoint     strtoll
#endif
#else
#define json_strtoint     strtol
#endif

static int lex_scan_number(lex_t *lex, int c, json_error_t *error)
{
    const char *saved_text;
    char *end;

    lex->token = TOKEN_INVALID;

    if(c == '-')
        c = lex_get_save(lex, error);

    if(c == '0') {
        c = lex_get_save(lex, error);
        if(l_isdigit(c)) {
            lex_unget_unsave(lex, c);
            goto out;
        }
    }
    else if(l_isdigit(c)) {
        c = lex_get_save(lex, error);
        while(l_isdigit(c))
            c = lex_get_save(lex, error);
    }
    else {
        lex_unget_unsave(lex, c);
        goto out;
    }

    if(c != '.' && c != 'E' && c != 'e') {
        json_int_t value;

        lex_unget_unsave(lex, c);

        saved_text = strbuffer_value(&lex->saved_text);

        errno = 0;
        value = json_strtoint(saved_text, &end, 10);
        if(errno == ERANGE) {
            if(value < 0)
                error_set(error, lex, "too big negative integer");
            else
                error_set(error, lex, "too big integer");
            goto out;
        }

        assert(end == saved_text + lex->saved_text.length);

        lex->token = TOKEN_INTEGER;
        lex->value.integer = value;
        return 0;
    }

    if(c == '.') {
        c = lex_get(lex, error);
        if(!l_isdigit(c)) {
            lex_unget(lex, c);
            goto out;
        }
        lex_save(lex, c);

        c = lex_get_save(lex, error);
        while(l_isdigit(c))
            c = lex_get_save(lex, error);
    }

    if(c == 'E' || c == 'e') {
        c = lex_get_save(lex, error);
        if(c == '+' || c == '-')
            c = lex_get_save(lex, error);

        if(!l_isdigit(c)) {
            lex_unget_unsave(lex, c);
            goto out;
        }

        c = lex_get_save(lex, error);
        while(l_isdigit(c))
            c = lex_get_save(lex, error);
    }

    lex_unget_unsave(lex, c);

    double value;
    if(jsonp_strtod(&lex->saved_text, &value)) {
        error_set(error, lex, "real number overflow");
        goto out;
    }

    lex->token = TOKEN_REAL;
    lex->value.real = value;
    return 0;

out:
    return -1;
}

static int lex_scan(lex_t *lex, json_error_t *error)
{
    int c;

    strbuffer_clear(&lex->saved_text);

    if(lex->token == TOKEN_STRING) {
        jsonp_free(lex->value.string);
        lex->value.string = nullptr;
    }

    c = lex_get(lex, error);
    while(c == ' ' || c == '\t' || c == '\n' || c == '\r')
        c = lex_get(lex, error);

    if(c == STREAM_STATE_EOF) {
        lex->token = TOKEN_EOF;
        goto out;
    }

    if(c == STREAM_STATE_ERROR) {
        lex->token = TOKEN_INVALID;
        goto out;
    }

    lex_save(lex, c);

    if(c == '{' || c == '}' || c == '[' || c == ']' || c == ':' || c == ',')
        lex->token = c;

    else if(c == '"')
        lex_scan_string(lex, error);

    else if(l_isdigit(c) || c == '-') {
        if(lex_scan_number(lex, c, error))
            goto out;
    }

    else if(l_isalpha(c)) {
        /* eat up the whole identifier for clearer error messages */
        const char *saved_text;

        c = lex_get_save(lex, error);
        while(l_isalpha(c))
            c = lex_get_save(lex, error);
        lex_unget_unsave(lex, c);

        saved_text = strbuffer_value(&lex->saved_text);

        if(strcmp(saved_text, "true") == 0)
            lex->token = TOKEN_TRUE;
        else if(strcmp(saved_text, "false") == 0)
            lex->token = TOKEN_FALSE;
        else if(strcmp(saved_text, "null") == 0)
            lex->token = TOKEN_NULL;
        else
            lex->token = TOKEN_INVALID;
    }

    else {
        /* save the rest of the input UTF-8 sequence to get an error
           message of valid UTF-8 */
        lex_save_cached(lex);
        lex->token = TOKEN_INVALID;
    }

out:
    return lex->token;
}

static char *lex_steal_string(lex_t *lex)
{
    char *result = nullptr;
    if(lex->token == TOKEN_STRING)
    {
        result = lex->value.string;
        lex->value.string = nullptr;
    }
    return result;
}

static int lex_init(lex_t *lex, get_func get, void *data)
{
    stream_init(&lex->stream, get, data);
    if(strbuffer_init(&lex->saved_text))
        return -1;

    lex->token = TOKEN_INVALID;
    return 0;
}

static void lex_close(lex_t *lex)
{
    if(lex->token == TOKEN_STRING)
        jsonp_free(lex->value.string);
    strbuffer_close(&lex->saved_text);
}


/*** parser ***/

static json_ref parse_value(lex_t *lex, size_t flags, json_error_t *error);

static json_ref parse_object(lex_t *lex, size_t flags, json_error_t *error)
{
    auto object = json_object();

    lex_scan(lex, error);
    if(lex->token == '}')
        return object;

    while(1) {
        char *key;
        json_ref value;

        if(lex->token != TOKEN_STRING) {
            error_set(error, lex, "string or '}' expected");
            return nullptr;
        }

        key = lex_steal_string(lex);
        if(!key)
            return nullptr;

        if(flags & JSON_REJECT_DUPLICATES) {
            if(json_object_get(object, key)) {
                jsonp_free(key);
                error_set(error, lex, "duplicate object key");
                return nullptr;
            }
        }

        lex_scan(lex, error);
        if(lex->token != ':') {
            jsonp_free(key);
            error_set(error, lex, "':' expected");
            return nullptr;
        }

        lex_scan(lex, error);
        value = parse_value(lex, flags, error);
        if(!value) {
            jsonp_free(key);
            return nullptr;
        }

        if(json_object_set_nocheck(object, key, value)) {
            jsonp_free(key);
            return nullptr;
        }

        jsonp_free(key);

        lex_scan(lex, error);
        if(lex->token != ',')
            break;

        lex_scan(lex, error);
    }

    if(lex->token != '}') {
        error_set(error, lex, "'}' expected");
        return nullptr;
    }

    return object;
}

static json_ref parse_array(lex_t *lex, size_t flags, json_error_t *error)
{
    auto array = json_array();
    if(!array)
        return nullptr;

    lex_scan(lex, error);
    if(lex->token == ']')
        return array;

    while(lex->token) {
        auto elem = parse_value(lex, flags, error);
        if(!elem)
            goto error;

        if(json_array_append(array, elem)) {
            goto error;
        }

        lex_scan(lex, error);
        if(lex->token != ',')
            break;

        lex_scan(lex, error);
    }

    if(lex->token != ']') {
        error_set(error, lex, "']' expected");
        goto error;
    }

    return array;

error:
    return nullptr;
}

static json_ref parse_value(lex_t *lex, size_t flags, json_error_t *error)
{
    json_ref json;

    switch(lex->token) {
        case TOKEN_STRING: {
            json = typed_string_to_json(lex->value.string, W_STRING_BYTE);
            break;
        }

        case TOKEN_INTEGER: {
            json = json_integer(lex->value.integer);
            break;
        }

        case TOKEN_REAL: {
            json = json_real(lex->value.real);
            break;
        }

        case TOKEN_TRUE:
            json = json_true();
            break;

        case TOKEN_FALSE:
            json = json_false();
            break;

        case TOKEN_NULL:
            json = json_null();
            break;

        case '{':
            json = parse_object(lex, flags, error);
            break;

        case '[':
            json = parse_array(lex, flags, error);
            break;

        case TOKEN_INVALID:
            error_set(error, lex, "invalid token");
            return nullptr;

        default:
            error_set(error, lex, "unexpected token");
            return nullptr;
    }

    if(!json)
        return nullptr;

    return json;
}

static json_ref parse_json(lex_t *lex, size_t flags, json_error_t *error)
{
    json_ref result;

    lex_scan(lex, error);
    if(!(flags & JSON_DECODE_ANY)) {
        if(lex->token != '[' && lex->token != '{') {
            error_set(error, lex, "'[' or '{' expected");
            return nullptr;
        }
    }

    result = parse_value(lex, flags, error);
    if(!result)
        return nullptr;

    if(!(flags & JSON_DISABLE_EOF_CHECK)) {
        lex_scan(lex, error);
        if(lex->token != TOKEN_EOF) {
            error_set(error, lex, "end of file expected");
            return nullptr;
        }
    }

    if(error) {
        /* Save the position even though there was no error */
        error->position = lex->stream.position;
    }

    return result;
}

typedef struct
{
    const char *data;
    int pos;
} string_data_t;

static int string_get(void *data)
{
    char c;
    string_data_t *stream = (string_data_t *)data;
    c = stream->data[stream->pos];
    if(c == '\0')
        return EOF;
    else
    {
        stream->pos++;
        return (unsigned char)c;
    }
}

json_ref json_loads(const char *string, size_t flags, json_error_t *error)
{
    lex_t lex;
    string_data_t stream_data;

    jsonp_error_init(error, "<string>");

    if (string == nullptr) {
        error_set(error, nullptr, "wrong arguments");
        return nullptr;
    }

    stream_data.data = string;
    stream_data.pos = 0;

    if(lex_init(&lex, string_get, (void *)&stream_data))
        return nullptr;

    auto result = parse_json(&lex, flags, error);

    lex_close(&lex);
    return result;
}

typedef struct
{
    const char *data;
    size_t len;
    size_t pos;
} buffer_data_t;

static int buffer_get(void *data)
{
    char c;
    auto stream = (buffer_data_t*)data;
    if(stream->pos >= stream->len)
      return EOF;

    c = stream->data[stream->pos];
    stream->pos++;
    return (unsigned char)c;
}

json_ref json_loadb(const char *buffer, size_t buflen, size_t flags, json_error_t *error)
{
    lex_t lex;
    buffer_data_t stream_data;

    jsonp_error_init(error, "<buffer>");

    if (buffer == nullptr) {
        error_set(error, nullptr, "wrong arguments");
        return nullptr;
    }

    stream_data.data = buffer;
    stream_data.pos = 0;
    stream_data.len = buflen;

    if(lex_init(&lex, buffer_get, (void *)&stream_data))
        return nullptr;

    auto result = parse_json(&lex, flags, error);

    lex_close(&lex);
    return result;
}

json_ref json_loadf(FILE *input, size_t flags, json_error_t *error)
{
    lex_t lex;
    const char *source;

    if(input == stdin)
        source = "<stdin>";
    else
        source = "<stream>";

    jsonp_error_init(error, source);

    if (input == nullptr) {
        error_set(error, nullptr, "wrong arguments");
        return nullptr;
    }

    if(lex_init(&lex, (get_func)fgetc, input))
        return nullptr;

    auto result = parse_json(&lex, flags, error);

    lex_close(&lex);
    return result;
}

json_ref json_load_file(const char *path, size_t flags, json_error_t *error)
{
    FILE *fp;

    jsonp_error_init(error, path);

    if (path == nullptr) {
        error_set(error, nullptr, "wrong arguments");
        return nullptr;
    }

    fp = fopen(path, "rb");
    if(!fp)
    {
        error_set(error, nullptr, "unable to open %s: %s",
                  path, strerror(errno));
        return nullptr;
    }

    auto result = json_loadf(fp, flags, error);

    fclose(fp);
    return result;
}

#define MAX_BUF_LEN 1024

typedef struct
{
    char data[MAX_BUF_LEN];
    size_t len;
    size_t pos;
    json_load_callback_t callback;
    void *arg;
} callback_data_t;

static int callback_get(void *data)
{
    char c;
    auto stream = (callback_data_t*)data;

    if(stream->pos >= stream->len) {
        stream->pos = 0;
        stream->len = stream->callback(stream->data, MAX_BUF_LEN, stream->arg);
        if(stream->len == 0 || stream->len == (size_t)-1)
            return EOF;
    }

    c = stream->data[stream->pos];
    stream->pos++;
    return (unsigned char)c;
}

json_ref json_load_callback(
    json_load_callback_t callback,
    void* arg,
    size_t flags,
    json_error_t* error) {
  lex_t lex;

  callback_data_t stream_data;

  memset(&stream_data, 0, sizeof(stream_data));
  stream_data.callback = callback;
  stream_data.arg = arg;

  jsonp_error_init(error, "<callback>");

  if (callback == nullptr) {
    error_set(error, nullptr, "wrong arguments");
    return nullptr;
  }

  if (lex_init(&lex, (get_func)callback_get, &stream_data))
    return nullptr;

  auto result = parse_json(&lex, flags, error);

  lex_close(&lex);
  return result;
}
