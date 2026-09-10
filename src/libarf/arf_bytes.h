/** @file arf_bytes.h
 *  @brief Internal little-endian byte cursor and growable output buffer.
 *
 *  Every read here is bounds-checked against the end of the buffer, so a
 *  truncated or corrupt container fails at the first short read instead of
 *  walking off the allocation.  A cursor that has overrun latches into a
 *  failed state and every subsequent read is a no-op returning zero, which
 *  means a parser can do a run of reads and check for failure once at the end.
 *
 *  Not part of the public API.
 *
 *  @author Ammar Qammaz (AmmarkoV)
 */

#ifndef ARF_BYTES_H_INCLUDED
#define ARF_BYTES_H_INCLUDED

#include <stdlib.h>
#include <string.h>

/** @brief A bounds-checked read cursor over a block of bytes we do not own. */
struct arfCursor
{
    const unsigned char *data;
    size_t               length;
    size_t               offset;
    int                  failed;
};

/** @brief A growable byte buffer we do own. */
struct arfBuffer
{
    unsigned char *data;
    size_t         length;
    size_t         capacity;
    int            failed;
};

static inline void arfCursorInit(struct arfCursor *cursor, const void *data, size_t length)
{
    cursor->data   = (const unsigned char *) data;
    cursor->length = length;
    cursor->offset = 0;
    cursor->failed = 0;
}

/** @brief Claim length bytes and advance, or latch failure and return 0. */
static inline const unsigned char *arfCursorTake(struct arfCursor *cursor, size_t length)
{
    if (cursor->failed)                            { return 0; }
    if (length > cursor->length - cursor->offset)  { cursor->failed = 1; return 0; }

    const unsigned char *at = cursor->data + cursor->offset;
    cursor->offset += length;
    return at;
}

static inline unsigned int arfCursorReadU32(struct arfCursor *cursor)
{
    const unsigned char *at = arfCursorTake(cursor,4);
    if (at==0) { return 0; }
    return (unsigned int) at[0]        | ((unsigned int) at[1] << 8) |
          ((unsigned int) at[2] << 16) | ((unsigned int) at[3] << 24);
}

static inline int arfCursorReadI32(struct arfCursor *cursor)
{
    return (int) arfCursorReadU32(cursor);
}

static inline unsigned int arfCursorReadU8(struct arfCursor *cursor)
{
    const unsigned char *at = arfCursorTake(cursor,1);
    if (at==0) { return 0; }
    return (unsigned int) at[0];
}

static inline float arfCursorReadF32(struct arfCursor *cursor)
{
    /* memcpy rather than a pointer cast: the payload is not guaranteed to be
     * 4-byte aligned inside the decompressed ZIP entry. */
    unsigned int bits = arfCursorReadU32(cursor);
    float value;
    memcpy(&value,&bits,4);
    return value;
}

/** @brief Read a length-prefixed UTF-8 string into a fixed buffer.
 *  @retval 1 on success, 0 if it overran the cursor or the destination */
static inline int arfCursorReadString(struct arfCursor *cursor, char *destination, size_t destinationSize)
{
    unsigned int length = arfCursorReadU32(cursor);
    const unsigned char *at = arfCursorTake(cursor,length);
    if (at==0)                        { return 0; }
    if (length >= destinationSize)    { cursor->failed = 1; return 0; }

    memcpy(destination,at,length);
    destination[length] = 0;
    return 1;
}

/** @brief Copy count little-endian float32s out.  On a little-endian host this
 *  is one memcpy; the loop below is the portable path and costs nothing
 *  measurable next to the ZIP inflate that produced the bytes. */
static inline int arfCursorReadFloats(struct arfCursor *cursor, float *destination, size_t count)
{
    const unsigned char *at = arfCursorTake(cursor,count*4);
    if (at==0) { return 0; }

    for (size_t i=0; i<count; i++)
    {
        unsigned int bits = (unsigned int) at[i*4+0]        | ((unsigned int) at[i*4+1] << 8) |
                           ((unsigned int) at[i*4+2] << 16) | ((unsigned int) at[i*4+3] << 24);
        memcpy(&destination[i],&bits,4);
    }
    return 1;
}

static inline int arfCursorReadU32s(struct arfCursor *cursor, unsigned int *destination, size_t count)
{
    const unsigned char *at = arfCursorTake(cursor,count*4);
    if (at==0) { return 0; }

    for (size_t i=0; i<count; i++)
    {
        destination[i] = (unsigned int) at[i*4+0]        | ((unsigned int) at[i*4+1] << 8) |
                        ((unsigned int) at[i*4+2] << 16) | ((unsigned int) at[i*4+3] << 24);
    }
    return 1;
}


static inline void arfBufferInit(struct arfBuffer *buffer)
{
    buffer->data     = 0;
    buffer->length   = 0;
    buffer->capacity = 0;
    buffer->failed   = 0;
}

static inline void arfBufferFree(struct arfBuffer *buffer)
{
    if (buffer->data!=0) { free(buffer->data); }
    arfBufferInit(buffer);
}

static inline int arfBufferReserve(struct arfBuffer *buffer, size_t extra)
{
    if (buffer->failed)                                    { return 0; }
    if (buffer->length + extra <= buffer->capacity)        { return 1; }

    size_t capacity = (buffer->capacity==0) ? 4096 : buffer->capacity;
    while (capacity < buffer->length + extra) { capacity *= 2; }

    unsigned char *data = (unsigned char *) realloc(buffer->data,capacity);
    if (data==0) { buffer->failed = 1; return 0; }

    buffer->data     = data;
    buffer->capacity = capacity;
    return 1;
}

static inline void arfBufferAppend(struct arfBuffer *buffer, const void *bytes, size_t length)
{
    if (!arfBufferReserve(buffer,length)) { return; }
    memcpy(buffer->data + buffer->length,bytes,length);
    buffer->length += length;
}

static inline void arfBufferWriteU8(struct arfBuffer *buffer, unsigned int value)
{
    unsigned char byte = (unsigned char)(value & 0xFF);
    arfBufferAppend(buffer,&byte,1);
}

static inline void arfBufferWriteU32(struct arfBuffer *buffer, unsigned int value)
{
    unsigned char bytes[4];
    bytes[0] = (unsigned char)( value        & 0xFF);
    bytes[1] = (unsigned char)((value >> 8)  & 0xFF);
    bytes[2] = (unsigned char)((value >> 16) & 0xFF);
    bytes[3] = (unsigned char)((value >> 24) & 0xFF);
    arfBufferAppend(buffer,bytes,4);
}

static inline void arfBufferWriteI32(struct arfBuffer *buffer, int value)
{
    arfBufferWriteU32(buffer,(unsigned int) value);
}

static inline void arfBufferWriteF32(struct arfBuffer *buffer, float value)
{
    unsigned int bits;
    memcpy(&bits,&value,4);
    arfBufferWriteU32(buffer,bits);
}

static inline void arfBufferWriteFloats(struct arfBuffer *buffer, const float *values, size_t count)
{
    for (size_t i=0; i<count; i++) { arfBufferWriteF32(buffer,values[i]); }
}

static inline void arfBufferWriteU32s(struct arfBuffer *buffer, const unsigned int *values, size_t count)
{
    for (size_t i=0; i<count; i++) { arfBufferWriteU32(buffer,values[i]); }
}

/** @brief Write a uint32 length followed by the raw UTF-8 bytes, no NUL. */
static inline void arfBufferWriteString(struct arfBuffer *buffer, const char *text)
{
    size_t length = strlen(text);
    arfBufferWriteU32(buffer,(unsigned int) length);
    arfBufferAppend(buffer,text,length);
}

#endif /* ARF_BYTES_H_INCLUDED */
