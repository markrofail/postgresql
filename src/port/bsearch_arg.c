/*
 *	bsearch_arg.c: bsearch variant with a user-supplied pointer
 *
 *	src/port/bsearch_arg.c
 */


#include "c.h"


/* simple counterpart to qsort_arg */
void *
bsearch_arg(const void *key, const void *base, size_t nmemb, size_t size,
			int (*compar) (const void *, const void *, void *),
			void *arg)
{
	size_t		l,
				u,
				idx;
	const void *p;
	int			comparison;

	l = 0;
	u = nmemb;
	while (l < u)
	{
		idx = (l + u) / 2;
		p = (void *) (((const char *) base) + (idx * size));
		comparison = (*compar) (key, p, arg);

		if (comparison < 0)
			u = idx;
		else if (comparison > 0)
			l = idx + 1;
		else
			return (void *) p;
	}

	return NULL;
}
