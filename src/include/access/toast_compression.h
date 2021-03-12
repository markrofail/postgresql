/*-------------------------------------------------------------------------
 *
 * toast_compression.h
 *	  Functions for toast compression.
 *
 * Copyright (c) 2021, PostgreSQL Global Development Group
 *
 * src/include/access/toast_compression.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TOAST_COMPRESSION_H
#define TOAST_COMPRESSION_H

#include "postgres.h"

#include "utils/guc.h"

/* default compression method if not specified. */
#define DEFAULT_TOAST_COMPRESSION	"pglz"

/* GUCs */
extern char *default_toast_compression;

/*
 * Built-in compression methods.  pg_attribute will store this in the
 * attcompression column.
 */
#define PGLZ_COMPRESSION			'p'
#define LZ4_COMPRESSION				'l'

#define InvalidCompressionMethod	'\0'
#define CompressionMethodIsValid(cm)  ((bool) ((cm) != InvalidCompressionMethod))

/*
 * Built-in compression method-id.  The toast compression header will store
 * this in the first 2 bits of the raw length.  These built-in compression
 * method-id are directly mapped to the built-in compression methods.
 */
typedef enum CompressionId
{
	PGLZ_COMPRESSION_ID = 0,
	LZ4_COMPRESSION_ID = 1
} CompressionId;

#define IsValidCompression(cm)  ((cm) != InvalidCompressionMethod)

#define IsStorageCompressible(storage) ((storage) != TYPSTORAGE_PLAIN && \
										(storage) != TYPSTORAGE_EXTERNAL)

/* compression handler routines */
typedef struct varlena *(*cmcompress_function) (const struct varlena *value);
typedef struct varlena *(*cmdecompress_function) (const struct varlena *value);
typedef struct varlena *(*cmdecompress_slice_function)
			(const struct varlena *value, int32 slicelength);

/*
 * API struct for a compression routines.
 *
 * 'cmname'	- name of the compression method
 * 'datum_compress' - varlena compression function.
 * 'datum_decompress' - varlena decompression function.
 * 'datum_decompress_slice' - varlena slice decompression functions.
 */
typedef struct CompressionRoutine
{
	char		cmname[64];
	cmcompress_function datum_compress;
	cmdecompress_function datum_decompress;
	cmdecompress_slice_function datum_decompress_slice;
} CompressionRoutine;

extern char CompressionNameToMethod(char *compression);
extern const CompressionRoutine *GetCompressionRoutines(char method);
extern bool check_default_toast_compression(char **newval, void **extra,
											GucSource source);

/*
 * CompressionMethodToId - Convert compression method to compression id.
 *
 * For more details refer comment atop CompressionId in toast_compression.h
 */
static inline CompressionId
CompressionMethodToId(char method)
{
	switch (method)
	{
		case PGLZ_COMPRESSION:
			return PGLZ_COMPRESSION_ID;
		case LZ4_COMPRESSION:
			return LZ4_COMPRESSION_ID;
		default:
			elog(ERROR, "invalid compression method %c", method);
	}
}

/*
 * CompressionIdToMethod - Convert compression id to compression method
 *
 * For more details refer comment atop CompressionId in toast_compression.h
 */
static inline Oid
CompressionIdToMethod(CompressionId cmid)
{
	switch (cmid)
	{
		case PGLZ_COMPRESSION_ID:
			return PGLZ_COMPRESSION;
		case LZ4_COMPRESSION_ID:
			return LZ4_COMPRESSION;
		default:
			elog(ERROR, "invalid compression method id %d", cmid);
	}
}

/*
 * GetCompressionMethodName - Get compression method name
 */
static inline const char*
GetCompressionMethodName(char method)
{
	return GetCompressionRoutines(method)->cmname;
}

/*
 * GetDefaultToastCompression -- get the current toast compression
 *
 * This exists to hide the use of the default_toast_compression GUC variable.
 */
static inline char
GetDefaultToastCompression(void)
{
	return CompressionNameToMethod(default_toast_compression);
}

#endif							/* TOAST_COMPRESSION_H */
