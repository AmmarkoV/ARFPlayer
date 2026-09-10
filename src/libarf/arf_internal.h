/** @file arf_internal.h
 *  @brief Bits shared between the reader and the writer.  Not public.
 *  @author Ammar Qammaz (AmmarkoV)
 */

#ifndef ARF_INTERNAL_H_INCLUDED
#define ARF_INTERNAL_H_INCLUDED

/** @brief Record why the last call failed.  printf-style. */
void arfSetError(const char *format, ...);

#endif /* ARF_INTERNAL_H_INCLUDED */
