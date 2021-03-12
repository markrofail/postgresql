/*-------------------------------------------------------------------------
 *
 * toast_compression.c
 *	  Functions for toast compression.
 *
 * Copyright (c) 2021, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/toast_compression.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_LZ4
#include <lz4.h>
#endif

#include "access/toast_compression.h"
#include "common/pg_lzcompress.h"
#include "fmgr.h"
#include "utils/builtins.h"

static struct varlena *pglz_cmcompress(const struct varlena *value);
static struct varlena *pglz_cmdecompress(const struct varlena *value);
static struct varlena *pglz_cmdecompress_slice(const struct varlena *value,
											  int32 slicelength);
static struct varlena *lz4_cmcompress(const struct varlena *value);
static struct varlena *lz4_cmdecompress(const struct varlena *value);
static struct varlena *lz4_cmdecompress_slice(const struct varlena *value,
											  int32 slicelength);

#define NO_LZ4_SUPPORT() \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("unsupported LZ4 compression method"), \
			 errdetail("This functionality requires the server to be built with lz4 support."), \
			 errhint("You need to rebuild PostgreSQL using --with-lz4.")))

/* handler routines for pglz and lz4 built-in compression methods */
const CompressionRoutine toast_compression[] =
{
	{
		.cmname = "pglz",
		.datum_compress = pglz_cmcompress,
		.datum_decompress = pglz_cmdecompress,
		.datum_decompress_slice = pglz_cmdecompress_slice
	},
	{
		.cmname = "lz4",
		.datum_compress = lz4_cmcompress,
		.datum_decompress = lz4_cmdecompress,
		.datum_decompress_slice = lz4_cmdecompress_slice
	}
};

/* Compile-time default */
char	*default_toast_compression = DEFAULT_TOAST_COMPRESSION;

/*
 * pglz_cmcompress - compression routine for pglz compression method
 *
 * Compresses source into dest using the default strategy. Returns the
 * compressed varlena, or NULL if compression fails.
 */
static struct varlena *
pglz_cmcompress(const struct varlena *value)
{
	int32		valsize,
				len;
	struct varlena *tmp = NULL;

	valsize = VARSIZE_ANY_EXHDR(DatumGetPointer(value));

	/*
	 * No point in wasting a palloc cycle if value size is outside the allowed
	 * range for compression.
	 */
	if (valsize < PGLZ_strategy_default->min_input_size ||
		valsize > PGLZ_strategy_default->max_input_size)
		return NULL;

	/*
	 * Figure out the maximum possible size of the pglz output, add the bytes
	 * that will be needed for varlena overhead, and allocate that amount.
	 */
	tmp = (struct varlena *) palloc(PGLZ_MAX_OUTPUT(valsize) +
									VARHDRSZ_COMPRESS);

	len = pglz_compress(VARDATA_ANY(value),
						valsize,
						(char *) tmp + VARHDRSZ_COMPRESS,
						NULL);
	if (len < 0)
	{
		pfree(tmp);
		return NULL;
	}

	SET_VARSIZE_COMPRESSED(tmp, len + VARHDRSZ_COMPRESS);

	return tmp;
}

/*
 * pglz_cmdecompress - decompression routine for pglz compression method
 *
 * Returns the decompressed varlena.
 */
static struct varlena *
pglz_cmdecompress(const struct varlena *value)
{
	struct varlena *result;
	int32		rawsize;

	/* allocate memory for the uncompressed data */
	result = (struct varlena *) palloc(VARRAWSIZE_4B_C(value) + VARHDRSZ);

	/* decompress the data */
	rawsize = pglz_decompress((char *) value + VARHDRSZ_COMPRESS,
							  VARSIZE(value) - VARHDRSZ_COMPRESS,
							  VARDATA(result),
							  VARRAWSIZE_4B_C(value), true);
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed pglz data is corrupt")));

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
}

/*
 * pglz_decompress - slice decompression routine for pglz compression method
 *
 * Decompresses part of the data. Returns the decompressed varlena.
 */
static struct varlena *
pglz_cmdecompress_slice(const struct varlena *value,
						int32 slicelength)
{
	struct varlena *result;
	int32		rawsize;

	/* allocate memory for the uncompressed data */
	result = (struct varlena *) palloc(slicelength + VARHDRSZ);

	/* decompress the data */
	rawsize = pglz_decompress((char *) value + VARHDRSZ_COMPRESS,
							  VARSIZE(value) - VARHDRSZ_COMPRESS,
							  VARDATA(result),
							  slicelength, false);
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed pglz data is corrupt")));

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
}

/*
 * lz4_cmcompress - compression routine for lz4 compression method
 *
 * Compresses source into dest using the LZ4 defaults. Returns the
 * compressed varlena, or NULL if compression fails.
 */
static struct varlena *
lz4_cmcompress(const struct varlena *value)
{
#ifndef USE_LZ4
	NO_LZ4_SUPPORT();
#else
	int32		valsize;
	int32		len;
	int32		max_size;
	struct varlena *tmp = NULL;

	valsize = VARSIZE_ANY_EXHDR(value);

	/*
	 * Figure out the maximum possible size of the LZ4 output, add the bytes
	 * that will be needed for varlena overhead, and allocate that amount.
	 */
	max_size = LZ4_compressBound(valsize);
	tmp = (struct varlena *) palloc(max_size + VARHDRSZ_COMPRESS);

	len = LZ4_compress_default(VARDATA_ANY(value),
							   (char *) tmp + VARHDRSZ_COMPRESS,
							   valsize, max_size);
	if (len <= 0)
		elog(ERROR, "could not compress data with lz4");

	/* data is incompressible so just free the memory and return NULL */
	if (len > valsize)
	{
		pfree(tmp);
		return NULL;
	}

	SET_VARSIZE_COMPRESSED(tmp, len + VARHDRSZ_COMPRESS);

	return tmp;
#endif
}

/*
 * lz4_cmdecompress - decompression routine for lz4 compression method
 *
 * Returns the decompressed varlena.
 */
static struct varlena *
lz4_cmdecompress(const struct varlena *value)
{
#ifndef USE_LZ4
	NO_LZ4_SUPPORT();
#else
	int32		rawsize;
	struct varlena *result;

	/* allocate memory for the uncompressed data */
	result = (struct varlena *) palloc(VARRAWSIZE_4B_C(value) + VARHDRSZ);

	/* decompress the data */
	rawsize = LZ4_decompress_safe((char *) value + VARHDRSZ_COMPRESS,
								  VARDATA(result),
								  VARSIZE(value) - VARHDRSZ_COMPRESS,
								  VARRAWSIZE_4B_C(value));
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed lz4 data is corrupt")));


	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
#endif
}

/*
 * lz4_cmdecompress_slice - slice decompression routine for lz4 compression
 *
 * Decompresses part of the data. Returns the decompressed varlena.
 */
static struct varlena *
lz4_cmdecompress_slice(const struct varlena *value, int32 slicelength)
{
#ifndef USE_LZ4
	NO_LZ4_SUPPORT();
#elif LZ4_VERSION_NUMBER < 10803
	return lz4_cmdecompress(value);
#else
	int32		rawsize;
	struct varlena *result;

	/* allocate memory for the uncompressed data */
	result = (struct varlena *) palloc(slicelength + VARHDRSZ);

	/* decompress the data */
	rawsize = LZ4_decompress_safe_partial((char *) value + VARHDRSZ_COMPRESS,
										  VARDATA(result),
										  VARSIZE(value) - VARHDRSZ_COMPRESS,
										  slicelength,
										  slicelength);
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed lz4 data is corrupt")));

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
#endif
}

/*
 * CompressionNameToMethod - Get compression method from compression name
 *
 * Search in the available built-in methods.  If the compression not found
 * in the built-in methods then return InvalidCompressionMethod.
 */
char
CompressionNameToMethod(char *compression)
{
	if (strcmp(toast_compression[PGLZ_COMPRESSION_ID].cmname,
			   compression) == 0)
		return PGLZ_COMPRESSION;
	else if (strcmp(toast_compression[LZ4_COMPRESSION_ID].cmname,
			 compression) == 0)
	{
#ifndef USE_LZ4
		NO_LZ4_SUPPORT();
#endif
		return LZ4_COMPRESSION;
	}

	return InvalidCompressionMethod;
}

/*
 * GetCompressionRoutines - Get compression handler routines
 */
const CompressionRoutine*
GetCompressionRoutines(char method)
{
	return &toast_compression[CompressionMethodToId(method)];
}

/* check_hook: validate new default_toast_compression */
bool
check_default_toast_compression(char **newval, void **extra, GucSource source)
{
	if (**newval == '\0')
	{
		GUC_check_errdetail("%s cannot be empty.",
							"default_toast_compression");
		return false;
	}

	if (strlen(*newval) >= NAMEDATALEN)
	{
		GUC_check_errdetail("%s is too long (maximum %d characters).",
							"default_toast_compression", NAMEDATALEN - 1);
		return false;
	}

	if (!CompressionMethodIsValid(CompressionNameToMethod(*newval)))
	{
		/*
		 * When source == PGC_S_TEST, don't throw a hard error for a
		 * nonexistent compression method, only a NOTICE. See comments in
		 * guc.h.
		 */
		if (source == PGC_S_TEST)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
						errmsg("compression method \"%s\" does not exist",
							*newval)));
		}
		else
		{
			GUC_check_errdetail("Compression method \"%s\" does not exist.",
								*newval);
			return false;
		}
	}

	return true;
}
