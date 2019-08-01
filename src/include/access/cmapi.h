/*-------------------------------------------------------------------------
 *
 * cmapi.h
 *	  API for Postgres compression AM.
 *
 * Copyright (c) 2015-2017, PostgreSQL Global Development Group
 *
 * src/include/access/cmapi.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef CMAPI_H
#define CMAPI_H

#include "postgres.h"
#include "catalog/pg_attr_compression.h"
#include "catalog/pg_attribute.h"
#include "nodes/pg_list.h"

#define IsBuiltinCompression(cmid)	((cmid) < FirstBootstrapObjectId)
#define DefaultCompressionOid		(PGLZ_AC_OID)

typedef struct CompressionAmRoutine CompressionAmRoutine;

/*
 * CompressionAmOptions contains all information needed to compress varlena.
 *
 *  For optimization purposes it will be created once for each attribute
 *  compression and stored in cache, until its renewal on global cache reset,
 *  or until deletion of related attribute compression.
 */
typedef struct CompressionAmOptions
{
	Oid			acoid;			/* Oid of attribute compression,
								   should go always first */
	Oid			amoid;			/* Oid of compression access method */
	List	   *acoptions;		/* Parsed options, used for comparison */
	CompressionAmRoutine *amroutine;	/* compression access method routine */
	MemoryContext	mcxt;

	/* result of cminitstate function will be put here */
	void	   *acstate;
} CompressionAmOptions;

typedef void (*cmcheck_function) (Form_pg_attribute att, List *options);
typedef struct varlena *(*cmcompress_function)
			(CompressionAmOptions *cmoptions, const struct varlena *value);
typedef struct varlena *(*cmdecompress_slice_function)
			(CompressionAmOptions *cmoptions, const struct varlena *value,
             int32 slicelength);
typedef void *(*cminitstate_function) (Oid acoid, List *options);

/*
 * API struct for a compression AM.
 *
 * 'cmcheck' - called when attribute is linking with compression method.
 *  This function should check compability of compression method with
 *  the attribute and its options.
 *
 * 'cminitstate' - called when CompressionAmOptions instance is created.
 *  Should return pointer to a memory in a caller memory context, or NULL.
 *  Could be used to pass some internal state between compression function
 *  calls, like internal structure for parsed compression options.
 *
 * 'cmcompress' and 'cmdecompress' - varlena compression functions.
 */
struct CompressionAmRoutine
{
	NodeTag		type;

	cmcheck_function cmcheck;	/* can be NULL */
	cminitstate_function cminitstate;	/* can be NULL */
	cmcompress_function cmcompress;
	cmcompress_function cmdecompress;
	cmdecompress_slice_function cmdecompress_slice;
};

/* access/compression/cmapi.c */
extern CompressionAmRoutine *InvokeCompressionAmHandler(Oid amhandler);
extern List *GetAttrCompressionOptions(Oid acoid);
extern Oid	GetAttrCompressionAmOid(Oid acoid);

#endif							/* CMAPI_H */
