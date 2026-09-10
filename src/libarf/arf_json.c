/** @file arf_json.c
 *  @brief Implementation of the small JSON reader described in arf_json.h.
 *  @author Ammar Qammaz (AmmarkoV)
 */

#include "arf_json.h"

#include <stdlib.h>
#include <string.h>

/** @brief Parser state.  `error` latches the first failure; once set every
 *  parse function unwinds without touching the input again. */
struct arfJsonParser
{
    const char *text;
    size_t      length;
    size_t      offset;
    const char *error;
    int         depth;
};

/** Deep enough for any ARF document by a wide margin, shallow enough that a
 *  hostile file cannot recurse the parser into the guard page. */
#define ARF_JSON_MAX_DEPTH 64

static struct arfJsonValue *arfJsonParseValue(struct arfJsonParser *parser);

static struct arfJsonValue *arfJsonAllocate(int type)
{
    struct arfJsonValue *value = (struct arfJsonValue *) calloc(1,sizeof(struct arfJsonValue));
    if (value!=0) { value->type = type; }
    return value;
}

void arfJsonFree(struct arfJsonValue *value)
{
    while (value!=0)
    {
        struct arfJsonValue *next = value->nextSibling;

        arfJsonFree(value->firstChild);
        if (value->string!=0) { free(value->string); }
        if (value->key!=0)    { free(value->key);    }
        free(value);

        /* Siblings are iterated rather than recursed -- a 36k-element array
         * would otherwise be 36k stack frames deep on free. */
        value = next;
    }
}

static void arfJsonSkipWhitespace(struct arfJsonParser *parser)
{
    while (parser->offset < parser->length)
    {
        char c = parser->text[parser->offset];
        if ( (c==' ') || (c=='\t') || (c=='\n') || (c=='\r') ) { parser->offset++; }
        else                                                   { break; }
    }
}

static int arfJsonPeek(struct arfJsonParser *parser)
{
    if (parser->offset >= parser->length) { return -1; }
    return (unsigned char) parser->text[parser->offset];
}

static int arfJsonLiteral(struct arfJsonParser *parser, const char *literal)
{
    size_t length = strlen(literal);
    if (parser->length - parser->offset < length)                     { return 0; }
    if (memcmp(parser->text + parser->offset,literal,length)!=0)      { return 0; }
    parser->offset += length;
    return 1;
}

/** @brief Encode one Unicode code point as UTF-8 into destination.
 *  @retval number of bytes written */
static int arfJsonEncodeUTF8(unsigned int codepoint, char *destination)
{
    if (codepoint < 0x80)
    {
        destination[0] = (char) codepoint;
        return 1;
    }
    if (codepoint < 0x800)
    {
        destination[0] = (char)(0xC0 | (codepoint >> 6));
        destination[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000)
    {
        destination[0] = (char)(0xE0 | (codepoint >> 12));
        destination[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        destination[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }

    destination[0] = (char)(0xF0 | (codepoint >> 18));
    destination[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
    destination[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    destination[3] = (char)(0x80 | (codepoint & 0x3F));
    return 4;
}

/** @brief Read four hex digits of a \\u escape.  @retval the value, or -1 */
static int arfJsonHex4(struct arfJsonParser *parser)
{
    if (parser->length - parser->offset < 4) { return -1; }

    int value = 0;
    for (int i=0; i<4; i++)
    {
        int c = (unsigned char) parser->text[parser->offset + i];
        int digit;

        if      ( (c>='0') && (c<='9') ) { digit = c - '0';      }
        else if ( (c>='a') && (c<='f') ) { digit = c - 'a' + 10; }
        else if ( (c>='A') && (c<='F') ) { digit = c - 'A' + 10; }
        else                             { return -1;            }

        value = (value << 4) | digit;
    }

    parser->offset += 4;
    return value;
}

/** @brief Parse a quoted string.  The opening quote must already be current.
 *  @retval a freshly allocated NUL terminated UTF-8 string, or 0 */
static char *arfJsonParseString(struct arfJsonParser *parser)
{
    if (arfJsonPeek(parser)!='"') { parser->error = "expected a string"; return 0; }
    parser->offset++;

    /* The decoded string is never longer than the escaped source, so one
     * up-front allocation of that size always suffices. */
    size_t capacity = parser->length - parser->offset + 1;
    char  *out      = (char *) malloc(capacity);
    if (out==0) { parser->error = "out of memory"; return 0; }

    size_t written = 0;
    while (parser->offset < parser->length)
    {
        int c = (unsigned char) parser->text[parser->offset++];

        if (c=='"')
        {
            out[written] = 0;
            return out;
        }

        if (c!='\\')
        {
            out[written++] = (char) c;
            continue;
        }

        if (parser->offset >= parser->length) { break; }
        int escape = (unsigned char) parser->text[parser->offset++];

        switch (escape)
        {
            case '"'  : out[written++] = '"';  break;
            case '\\' : out[written++] = '\\'; break;
            case '/'  : out[written++] = '/';  break;
            case 'b'  : out[written++] = '\b'; break;
            case 'f'  : out[written++] = '\f'; break;
            case 'n'  : out[written++] = '\n'; break;
            case 'r'  : out[written++] = '\r'; break;
            case 't'  : out[written++] = '\t'; break;
            case 'u'  :
            {
                int codepoint = arfJsonHex4(parser);
                if (codepoint < 0) { parser->error = "bad \\u escape"; free(out); return 0; }

                /* A high surrogate must be followed by its low half; anything
                 * else is passed through as-is rather than rejected, since a
                 * lone surrogate cannot break the rest of the document. */
                if ( (codepoint >= 0xD800) && (codepoint <= 0xDBFF) &&
                     (parser->length - parser->offset >= 6) &&
                     (parser->text[parser->offset]=='\\') && (parser->text[parser->offset+1]=='u') )
                {
                    size_t savedOffset = parser->offset;
                    parser->offset += 2;

                    int low = arfJsonHex4(parser);
                    if ( (low >= 0xDC00) && (low <= 0xDFFF) )
                    {
                        codepoint = 0x10000 + (((codepoint - 0xD800) << 10) | (low - 0xDC00));
                    }
                    else
                    {
                        parser->offset = savedOffset;
                    }
                }

                written += (size_t) arfJsonEncodeUTF8((unsigned int) codepoint,out + written);
            }
            break;

            default:
                parser->error = "unknown escape sequence";
                free(out);
                return 0;
        }
    }

    parser->error = "unterminated string";
    free(out);
    return 0;
}

static struct arfJsonValue *arfJsonParseNumber(struct arfJsonParser *parser)
{
    /* strtod needs a NUL terminated buffer and the document may not be one, so
     * the numeric run is copied out first.  Numbers in ARF documents are short. */
    size_t start = parser->offset;
    while (parser->offset < parser->length)
    {
        char c = parser->text[parser->offset];
        if ( ((c>='0') && (c<='9')) || (c=='-') || (c=='+') || (c=='.') || (c=='e') || (c=='E') )
        {
            parser->offset++;
        }
        else { break; }
    }

    size_t length = parser->offset - start;
    if ( (length==0) || (length >= 64) ) { parser->error = "malformed number"; return 0; }

    char scratch[64];
    memcpy(scratch,parser->text + start,length);
    scratch[length] = 0;

    char *end = 0;
    double number = strtod(scratch,&end);
    if ( (end==0) || (*end!=0) ) { parser->error = "malformed number"; return 0; }

    struct arfJsonValue *value = arfJsonAllocate(ARF_JSON_NUMBER);
    if (value==0) { parser->error = "out of memory"; return 0; }

    value->number = number;
    return value;
}

static void arfJsonAppendChild(struct arfJsonValue *parent, struct arfJsonValue *child)
{
    if (parent->lastChild==0) { parent->firstChild = child;            }
    else                      { parent->lastChild->nextSibling = child; }

    parent->lastChild = child;
    parent->childCount++;
}

static struct arfJsonValue *arfJsonParseArray(struct arfJsonParser *parser)
{
    struct arfJsonValue *array = arfJsonAllocate(ARF_JSON_ARRAY);
    if (array==0) { parser->error = "out of memory"; return 0; }

    parser->offset++;  /* [ */
    arfJsonSkipWhitespace(parser);

    if (arfJsonPeek(parser)==']') { parser->offset++; return array; }

    while (1)
    {
        struct arfJsonValue *element = arfJsonParseValue(parser);
        if (element==0) { arfJsonFree(array); return 0; }

        arfJsonAppendChild(array,element);
        arfJsonSkipWhitespace(parser);

        int c = arfJsonPeek(parser);
        if (c==',') { parser->offset++; arfJsonSkipWhitespace(parser); continue; }
        if (c==']') { parser->offset++; return array; }

        parser->error = "expected ',' or ']' in array";
        arfJsonFree(array);
        return 0;
    }
}

static struct arfJsonValue *arfJsonParseObject(struct arfJsonParser *parser)
{
    struct arfJsonValue *object = arfJsonAllocate(ARF_JSON_OBJECT);
    if (object==0) { parser->error = "out of memory"; return 0; }

    parser->offset++;  /* { */
    arfJsonSkipWhitespace(parser);

    if (arfJsonPeek(parser)=='}') { parser->offset++; return object; }

    while (1)
    {
        char *key = arfJsonParseString(parser);
        if (key==0) { arfJsonFree(object); return 0; }

        arfJsonSkipWhitespace(parser);
        if (arfJsonPeek(parser)!=':')
        {
            parser->error = "expected ':' after an object key";
            free(key);
            arfJsonFree(object);
            return 0;
        }
        parser->offset++;

        struct arfJsonValue *member = arfJsonParseValue(parser);
        if (member==0) { free(key); arfJsonFree(object); return 0; }

        member->key = key;
        arfJsonAppendChild(object,member);
        arfJsonSkipWhitespace(parser);

        int c = arfJsonPeek(parser);
        if (c==',') { parser->offset++; arfJsonSkipWhitespace(parser); continue; }
        if (c=='}') { parser->offset++; return object; }

        parser->error = "expected ',' or '}' in object";
        arfJsonFree(object);
        return 0;
    }
}

static struct arfJsonValue *arfJsonParseValue(struct arfJsonParser *parser)
{
    if (parser->error!=0) { return 0; }

    if (++parser->depth > ARF_JSON_MAX_DEPTH)
    {
        parser->error = "document nested too deeply";
        parser->depth--;
        return 0;
    }

    arfJsonSkipWhitespace(parser);

    struct arfJsonValue *value = 0;
    int c = arfJsonPeek(parser);

    switch (c)
    {
        case '{' : value = arfJsonParseObject(parser); break;
        case '[' : value = arfJsonParseArray(parser);  break;

        case '"' :
        {
            char *string = arfJsonParseString(parser);
            if (string!=0)
            {
                value = arfJsonAllocate(ARF_JSON_STRING);
                if (value!=0) { value->string = string; }
                else          { free(string); parser->error = "out of memory"; }
            }
        }
        break;

        case 't' :
            if (arfJsonLiteral(parser,"true"))
            {
                value = arfJsonAllocate(ARF_JSON_BOOL);
                if (value!=0) { value->number = 1.0; }
            }
            else { parser->error = "expected 'true'"; }
        break;

        case 'f' :
            if (arfJsonLiteral(parser,"false")) { value = arfJsonAllocate(ARF_JSON_BOOL); }
            else                                { parser->error = "expected 'false'"; }
        break;

        case 'n' :
            if (arfJsonLiteral(parser,"null")) { value = arfJsonAllocate(ARF_JSON_NULL); }
            else                               { parser->error = "expected 'null'"; }
        break;

        case -1 :
            parser->error = "unexpected end of document";
        break;

        default :
            value = arfJsonParseNumber(parser);
        break;
    }

    if ( (value==0) && (parser->error==0) ) { parser->error = "out of memory"; }

    parser->depth--;
    return value;
}

struct arfJsonValue *arfJsonParse(const char *text, size_t length, const char **error)
{
    struct arfJsonParser parser;
    parser.text   = text;
    parser.length = length;
    parser.offset = 0;
    parser.error  = 0;
    parser.depth  = 0;

    if (error!=0) { *error = 0; }
    if ( (text==0) || (length==0) )
    {
        if (error!=0) { *error = "empty document"; }
        return 0;
    }

    /* Step over a UTF-8 byte order mark if one is present. */
    if ( (length >= 3) && ((unsigned char)text[0]==0xEF) &&
         ((unsigned char)text[1]==0xBB) && ((unsigned char)text[2]==0xBF) )
    {
        parser.offset = 3;
    }

    struct arfJsonValue *root = arfJsonParseValue(&parser);
    if (root==0)
    {
        if (error!=0) { *error = (parser.error!=0) ? parser.error : "parse failed"; }
        return 0;
    }

    arfJsonSkipWhitespace(&parser);
    if (parser.offset != parser.length)
    {
        arfJsonFree(root);
        if (error!=0) { *error = "trailing content after the top level value"; }
        return 0;
    }

    return root;
}

const struct arfJsonValue *arfJsonMember(const struct arfJsonValue *object, const char *key)
{
    if ( (object==0) || (object->type!=ARF_JSON_OBJECT) ) { return 0; }

    for (const struct arfJsonValue *child=object->firstChild; child!=0; child=child->nextSibling)
    {
        if ( (child->key!=0) && (strcmp(child->key,key)==0) ) { return child; }
    }
    return 0;
}

const struct arfJsonValue *arfJsonAt(const struct arfJsonValue *array, unsigned int index)
{
    if (array==0) { return 0; }
    if ( (array->type!=ARF_JSON_ARRAY) && (array->type!=ARF_JSON_OBJECT) ) { return 0; }

    const struct arfJsonValue *child = array->firstChild;
    while ( (child!=0) && (index>0) ) { child = child->nextSibling; index--; }
    return child;
}

unsigned int arfJsonCount(const struct arfJsonValue *value)
{
    if (value==0) { return 0; }
    return value->childCount;
}

const char *arfJsonString(const struct arfJsonValue *value, const char *fallback)
{
    if ( (value==0) || (value->type!=ARF_JSON_STRING) || (value->string==0) ) { return fallback; }
    return value->string;
}

double arfJsonNumber(const struct arfJsonValue *value, double fallback)
{
    if ( (value==0) || ((value->type!=ARF_JSON_NUMBER) && (value->type!=ARF_JSON_BOOL)) ) { return fallback; }
    return value->number;
}
