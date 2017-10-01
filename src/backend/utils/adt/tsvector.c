/*-------------------------------------------------------------------------
 *
 * tsvector.c
 *	  I/O functions for tsvector
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsvector.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqformat.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

typedef struct
{
	WordEntry	entry;			/* must be first! */
	size_t		offset;			/* offset of lexeme in some buffer */
	WordEntryPos *pos;
} WordEntryIN;


/* Compare two WordEntryPos values for qsort */
int
compareWordEntryPos(const void *a, const void *b)
{
	int			apos = WEP_GETPOS(*(const WordEntryPos *) a);
	int			bpos = WEP_GETPOS(*(const WordEntryPos *) b);

	if (apos == bpos)
		return 0;
	return (apos > bpos) ? 1 : -1;
}

/*
 * Removes duplicate pos entries. If there's two entries with same pos
 * but different weight, the higher weight is retained.
 *
 * Returns new length.
 */
static int
uniquePos(WordEntryPos *a, int l)
{
	WordEntryPos *ptr,
			   *res;

	if (l <= 1)
		return l;

	qsort((void *) a, l, sizeof(WordEntryPos), compareWordEntryPos);

	res = a;
	ptr = a + 1;
	while (ptr - a < l)
	{
		if (WEP_GETPOS(*ptr) != WEP_GETPOS(*res))
		{
			res++;
			*res = *ptr;
			if (res - a >= MAXNUMPOS - 1 ||
				WEP_GETPOS(*res) == MAXENTRYPOS - 1)
				break;
		}
		else if (WEP_GETWEIGHT(*ptr) > WEP_GETWEIGHT(*res))
			WEP_SETWEIGHT(*res, WEP_GETWEIGHT(*ptr));
		ptr++;
	}

	return res + 1 - a;
}

/* Compare two WordEntryIN values for qsort */
static int
compareentry_in(const void *va, const void *vb, void *arg)
{
	const WordEntryIN *a = (const WordEntryIN *) va;
	const WordEntryIN *b = (const WordEntryIN *) vb;
	char	   *BufferStr = (char *) arg;

	return tsCompareString(&BufferStr[a->offset], a->entry.len,
						   &BufferStr[b->offset], b->entry.len,
						   false);
}

/* Compare two WordEntry values for qsort */
static int
compareentry(const void *va, const void *vb, void *arg)
{
	const WordEntry *a = (const WordEntry *) va;
	const WordEntry *b = (const WordEntry *) vb;
	TSVector	tsv = (TSVector) arg;

	uint32		offset1 = tsvector_getoffset(tsv, a - ARRPTR(tsv), NULL),
				offset2 = tsvector_getoffset(tsv, b - ARRPTR(tsv), NULL);

	return tsCompareString(STRPTR(tsv) + offset1, ENTRY_LEN(tsv, a),
						   STRPTR(tsv) + offset2, ENTRY_LEN(tsv, b),
						   false);
}

/*
 * Sort an array of WordEntryIN, remove duplicates.
 * *outbuflen receives the amount of space needed for strings and positions.
 */
static int
uniqueentry(WordEntryIN *a, int l, char *buf, int *outbuflen)
{
	int			buflen,
				i = 0;
	WordEntryIN *ptr,
			   *res;

	Assert(l >= 1);

	if (l > 1)
		qsort_arg((void *) a, l, sizeof(WordEntryIN), compareentry_in,
				  (void *) buf);

	buflen = 0;
	res = a;
	ptr = a + 1;
	while (ptr - a < l)
	{
		Assert(!ptr->entry.hasoff);

		if (!(ptr->entry.len == res->entry.len &&
			  strncmp(&buf[ptr->offset], &buf[res->offset], res->entry.len) == 0))
		{
			/* done accumulating data into *res, count space needed */
			buflen = SHORTALIGN(buflen);
			if (i++ % TS_OFFSET_STRIDE == 0)
			{
				buflen = INTALIGN(buflen);
				buflen += sizeof(WordEntry);
			}

			buflen += res->entry.len;
			if (res->entry.npos)
			{
				res->entry.npos = uniquePos(res->pos, res->entry.npos);
				buflen = SHORTALIGN(buflen);
				buflen += res->entry.npos * sizeof(WordEntryPos);
			}
			res++;
			if (res != ptr)
				*res = *ptr;
		}
		else if (ptr->entry.npos)
		{
			if (res->entry.npos)
			{
				/* append ptr's positions to res's positions */
				int			newlen = ptr->entry.npos + res->entry.npos;

				res->pos = (WordEntryPos *)
					repalloc(res->pos, newlen * sizeof(WordEntryPos));
				memcpy(&res->pos[res->entry.npos], ptr->pos,
					   ptr->entry.npos * sizeof(WordEntryPos));
				res->entry.npos = newlen;
				pfree(ptr->pos);
			}
			else
			{
				/* just give ptr's positions to pos */
				res->entry.npos = ptr->entry.npos;
				res->pos = ptr->pos;
			}
		}
		ptr++;
	}

	/* count space needed for last item */
	if (i % TS_OFFSET_STRIDE == 0)
	{
		buflen = INTALIGN(buflen);
		buflen += sizeof(WordEntry);
	}
	else
		buflen = SHORTALIGN(buflen);

	buflen += res->entry.len;

	if (res->entry.npos)
	{
		res->entry.npos = uniquePos(res->pos, res->entry.npos);
		buflen = SHORTALIGN(buflen);
		buflen += res->entry.npos * sizeof(WordEntryPos);
	}

	*outbuflen = buflen;
	return res + 1 - a;
}

Datum
tsvectorin(PG_FUNCTION_ARGS)
{
	char	   *buf = PG_GETARG_CSTRING(0);
	TSVectorParseState state;
	WordEntryIN *arr;
	int			totallen;
	int			arrlen;			/* allocated size of arr */
	int			len = 0;
	TSVector	in;
	int			i;
	char	   *token;
	int			toklen;
	WordEntryPos *pos;
	int			poslen;
	int			stroff;

	/*
	 * Tokens are appended to tmpbuf, cur is a pointer to the end of used
	 * space in tmpbuf.
	 */
	char	   *tmpbuf;
	char	   *cur;
	int			buflen = 256;	/* allocated size of tmpbuf */

	state = init_tsvector_parser(buf, false, false);

	arrlen = 64;
	arr = (WordEntryIN *) palloc(sizeof(WordEntryIN) * arrlen);
	cur = tmpbuf = (char *) palloc(buflen);

	while (gettoken_tsvector(state, &token, &toklen, &pos, &poslen, NULL))
	{
		if (toklen >= MAXSTRLEN)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("word is too long (%ld bytes, max %ld bytes)",
							(long) toklen,
							(long) (MAXSTRLEN - 1))));

		if (cur - tmpbuf > MAXSTRPOS)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("string is too long for tsvector (%ld bytes, max %ld bytes)",
							(long) (cur - tmpbuf), (long) MAXSTRPOS)));

		/*
		 * Enlarge buffers if needed
		 */
		if (len >= arrlen)
		{
			arrlen *= 2;
			arr = (WordEntryIN *)
				repalloc((void *) arr, sizeof(WordEntryIN) * arrlen);
		}
		while ((cur - tmpbuf) + toklen >= buflen)
		{
			int			dist = cur - tmpbuf;

			buflen *= 2;
			tmpbuf = (char *) repalloc((void *) tmpbuf, buflen);
			cur = tmpbuf + dist;
		}
		arr[len].entry.hasoff = 0;
		arr[len].entry.len = toklen;
		arr[len].offset = cur - tmpbuf;
		arr[len].entry.npos = poslen;
		arr[len].pos = (poslen != 0) ? pos : NULL;
		memcpy((void *) cur, (void *) token, toklen);
		cur += toklen;
		len++;
	}

	close_tsvector_parser(state);

	if (len > 0)
		len = uniqueentry(arr, len, tmpbuf, &buflen);
	else
		buflen = 0;

	if (buflen > MAXSTRPOS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("string is too long for tsvector (%d bytes, max %d bytes)", buflen, MAXSTRPOS)));

	totallen = CALCDATASIZE(len, buflen);
	in = (TSVector) palloc0(totallen);
	SET_VARSIZE(in, totallen);
	TS_SETCOUNT(in, len);
	stroff = 0;
	for (i = 0; i < len; i++)
	{
		tsvector_addlexeme(in, i, &stroff, &tmpbuf[arr[i].offset],
						   arr[i].entry.len, arr[i].pos, arr[i].entry.npos);

		if (arr[i].entry.npos)
			pfree(arr[i].pos);
	}

	Assert((STRPTR(in) + stroff - (char *) in) == totallen);
	PG_RETURN_TSVECTOR(in);
}

Datum
tsvectorout(PG_FUNCTION_ARGS)
{
	TSVector	out = PG_GETARG_TSVECTOR(0);
	char	   *outbuf;
	int32		i,
				lenbuf = 0,
				pp,
				tscount = TS_COUNT(out);
	uint32		pos;
	WordEntry  *ptr = ARRPTR(out);
	char	   *curbegin,
			   *curin,
			   *curout;

	lenbuf = tscount * 2 /* '' */ + tscount - 1 /* space */ + 2 /* \0 */ ;
	for (i = 0; i < tscount; i++)
	{
		int			npos = ENTRY_NPOS(out, ptr + i);

		lenbuf += ENTRY_LEN(out, ptr + i) * 2 * pg_database_encoding_max_length() /* for escape */ ;
		if (npos)
			lenbuf += 1 /* : */ + 7 /* int2 + , + weight */ * npos;
	}

	curout = outbuf = (char *) palloc(lenbuf);

	INITPOS(pos);
	for (i = 0; i < tscount; i++)
	{
		int			lex_len = ENTRY_LEN(out, ptr),
					npos = ENTRY_NPOS(out, ptr);

		curbegin = curin = STRPTR(out) + pos;
		if (i != 0)
			*curout++ = ' ';
		*curout++ = '\'';
		while (curin - curbegin < lex_len)
		{
			int			len = pg_mblen(curin);

			if (t_iseq(curin, '\''))
				*curout++ = '\'';
			else if (t_iseq(curin, '\\'))
				*curout++ = '\\';

			while (len--)
				*curout++ = *curin++;
		}

		*curout++ = '\'';
		if ((pp = npos) != 0)
		{
			WordEntryPos *wptr;

			*curout++ = ':';
			wptr = POSDATAPTR(curbegin, lex_len);
			while (pp)
			{
				curout += sprintf(curout, "%d", WEP_GETPOS(*wptr));
				switch (WEP_GETWEIGHT(*wptr))
				{
					case 3:
						*curout++ = 'A';
						break;
					case 2:
						*curout++ = 'B';
						break;
					case 1:
						*curout++ = 'C';
						break;
					case 0:
					default:
						break;
				}

				if (pp > 1)
					*curout++ = ',';
				pp--;
				wptr++;
			}
		}

		INCRPTR(out, ptr, pos);
	}

	*curout = '\0';
	PG_FREE_IF_COPY(out, 0);
	PG_RETURN_CSTRING(outbuf);
}

/*
 * Binary Input / Output functions. The binary format is as follows:
 *
 * uint32	number of lexemes
 *
 * for each lexeme:
 *		lexeme text in client encoding, null-terminated
 *		uint16	number of positions
 *		for each position:
 *			uint16 WordEntryPos
 */

Datum
tsvectorsend(PG_FUNCTION_ARGS)
{
	TSVector	vec = PG_GETARG_TSVECTOR(0);
	StringInfoData buf;
	int			i,
				j;
	uint32		pos;
	WordEntry  *weptr = ARRPTR(vec);

	pq_begintypsend(&buf);
	pq_sendint(&buf, TS_COUNT(vec), sizeof(int32));

	INITPOS(pos);
	for (i = 0; i < TS_COUNT(vec); i++)
	{
		char	   *lexeme = STRPTR(vec) + pos;
		int			npos = ENTRY_NPOS(vec, weptr),
					lex_len = ENTRY_LEN(vec, weptr);

		/*
		 * the strings in the TSVector array are not null-terminated, so we
		 * have to send the null-terminator separately
		 */
		pq_sendtext(&buf, lexeme, lex_len);
		pq_sendbyte(&buf, '\0');
		pq_sendint(&buf, npos, sizeof(uint16));

		if (npos > 0)
		{
			WordEntryPos *wepptr = POSDATAPTR(lexeme, lex_len);

			for (j = 0; j < npos; j++)
				pq_sendint(&buf, wepptr[j], sizeof(WordEntryPos));
		}
		INCRPTR(vec, weptr, pos);
	}

	PG_FREE_IF_COPY(vec, 0);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
tsvectorrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	TSVector	vec;
	int			i,
				datalen;		/* number of bytes used in the variable size
								 * area after fixed size TSVector header and
								 * WordEntries */
	int32		nentries;
	Size		hdrlen;
	Size		len;			/* allocated size of vec */
	bool		needSort = false;
	char	   *prev_lexeme = NULL;
	int			prev_lex_len;

	nentries = pq_getmsgint(buf, sizeof(int32));
	if (nentries < 0 || nentries > (MaxAllocSize / sizeof(WordEntry)))
		elog(ERROR, "invalid size of tsvector");

	hdrlen = DATAHDRSIZE + sizeof(WordEntry) * nentries;

	len = hdrlen * 2;			/* times two to make room for lexemes */
	vec = (TSVector) palloc0(len);
	TS_SETCOUNT(vec, nentries);

	datalen = 0;
	for (i = 0; i < nentries; i++)
	{
		char	   *lexeme,
				   *lexeme_out;
		uint16		npos;
		int			lex_len;

		lexeme = (char *) pq_getmsgstring(buf);
		npos = (uint16) pq_getmsgint(buf, sizeof(uint16));

		/* sanity checks */

		lex_len = strlen(lexeme);
		if (lex_len > MAXSTRLEN)
			elog(ERROR, "invalid tsvector: lexeme too long");

		if (datalen > MAXSTRPOS)
			elog(ERROR, "invalid tsvector: maximum total lexeme length exceeded");

		if (npos > MAXNUMPOS)
			elog(ERROR, "unexpected number of tsvector positions");

		/*
		 * Looks valid. Fill the WordEntry struct, and copy lexeme.
		 *
		 * But make sure the buffer is large enough first.
		 */
		while (hdrlen + SHORTALIGN(datalen + lex_len) + sizeof(WordEntry) +
			   npos * sizeof(WordEntryPos) >= len)
		{
			len *= 2;
			vec = (TSVector) repalloc(vec, len);
		}

		if (prev_lexeme && tsCompareString(lexeme, lex_len,
										   prev_lexeme, prev_lex_len, false) <= 0)
			needSort = true;

		lexeme_out = tsvector_addlexeme(vec, i, &datalen, lexeme,
										lex_len, NULL, npos);
		if (npos > 0)
		{
			WordEntryPos *wepptr;
			int			j;

			wepptr = POSDATAPTR(lexeme_out, lex_len);
			for (j = 0; j < npos; j++)
			{
				wepptr[j] = (WordEntryPos) pq_getmsgint(buf, sizeof(WordEntryPos));
				if (j > 0 && WEP_GETPOS(wepptr[j]) <= WEP_GETPOS(wepptr[j - 1]))
					elog(ERROR, "position information is misordered");
			}
		}

		prev_lexeme = lexeme;
		prev_lex_len = lex_len;
	}

	SET_VARSIZE(vec, hdrlen + datalen);

	if (needSort)
		qsort_arg((void *) ARRPTR(vec), TS_COUNT(vec), sizeof(WordEntry),
				  compareentry, (void *) vec);

	PG_RETURN_TSVECTOR(vec);
}
