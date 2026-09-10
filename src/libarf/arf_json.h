/** @file arf_json.h
 *  @brief A small recursive-descent JSON reader, just enough for arf.json.
 *
 *  arf.json is a few tens of kilobytes with a fixed schema and all the real
 *  geometry lives in the binary data items, so this parser is optimised for
 *  being short and obviously correct rather than fast.  It builds a DOM of
 *  linked nodes; lookups are linear scans, which on the largest array in a
 *  container (127 skeleton joints) is not worth improving on.
 *
 *  Values are unowned by the caller -- free the root with arfJsonFree() and
 *  every descendant goes with it.  Accessors tolerate a NULL value and return
 *  the supplied fallback, so a chain of lookups can be written without a
 *  guard at each step.
 *
 *  Not part of the public API.
 *
 *  @author Ammar Qammaz (AmmarkoV)
 */

#ifndef ARF_JSON_H_INCLUDED
#define ARF_JSON_H_INCLUDED

#include <stddef.h>

enum arfJsonType
{
    ARF_JSON_NULL = 0,
    ARF_JSON_BOOL,
    ARF_JSON_NUMBER,
    ARF_JSON_STRING,
    ARF_JSON_ARRAY,
    ARF_JSON_OBJECT
};

struct arfJsonValue
{
    int    type;
    double number;                    /**< NUMBER, and 0/1 for BOOL */
    char  *string;                    /**< STRING payload, owned, NUL terminated */
    char  *key;                       /**< member name when the parent is an OBJECT */

    struct arfJsonValue *firstChild;  /**< ARRAY elements or OBJECT members */
    struct arfJsonValue *lastChild;
    struct arfJsonValue *nextSibling;
    unsigned int         childCount;
};

/** @brief Parse a JSON document.
 *  @param text   the document, need not be NUL terminated
 *  @param length bytes of text
 *  @param error  receives a static description of the first syntax error, may be 0
 *  @retval the root value, to be released with arfJsonFree(), or 0 */
struct arfJsonValue *arfJsonParse(const char *text, size_t length, const char **error);

/** @brief Release a value and every descendant. */
void arfJsonFree(struct arfJsonValue *value);

/** @brief Look up a member of an object.  @retval the member, or 0 */
const struct arfJsonValue *arfJsonMember(const struct arfJsonValue *object, const char *key);

/** @brief Index into an array.  @retval the element, or 0 */
const struct arfJsonValue *arfJsonAt(const struct arfJsonValue *array, unsigned int index);

/** @brief Number of elements or members, 0 for anything else. */
unsigned int arfJsonCount(const struct arfJsonValue *value);

/** @brief String payload, or fallback if the value is missing or not a string. */
const char *arfJsonString(const struct arfJsonValue *value, const char *fallback);

/** @brief Numeric payload, or fallback if the value is missing or not a number. */
double arfJsonNumber(const struct arfJsonValue *value, double fallback);

#endif /* ARF_JSON_H_INCLUDED */
