/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "bytea.pgc"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>


#line 1 "regression.h"






#line 6 "bytea.pgc"

/* exec sql whenever sqlerror  sqlprint ; */
#line 7 "bytea.pgc"


static void
dump_binary(char *buf, int len, int ind)
{
	int i;

	printf("len=%d, ind=%d, data=0x", len, ind);
	for (i = 0; i < len; ++i)
		printf("%02x", 0xff & buf[i]);
	printf("\n");
}

#define DATA_SIZE 0x200
#define LACK_SIZE 13
#
int
main(void)
{
/* exec sql begin declare section */
	 
	 
	 
	   
	   
		  
		  
	

	 

#line 27 "bytea.pgc"
  struct bytea_1  { int len; char arr[ 512 ]; }  send_buf [ 2 ] ;
 
#line 28 "bytea.pgc"
  struct bytea_2  { int len; char arr[ DATA_SIZE ]; }  recv_buf [ 2 ] ;
 
#line 29 "bytea.pgc"
  struct bytea_3  { int len; char arr[ DATA_SIZE ]; } * recv_vlen_buf ;
 
#line 30 "bytea.pgc"
  struct bytea_4  { int len; char arr[ DATA_SIZE - LACK_SIZE ]; }  recv_short_buf ;
 
#line 31 "bytea.pgc"
  struct bytea_5  { int len; char arr[ 1 ]; }  notjson = { . arr = "x" , . len = 1 } ;
 
#line 36 "bytea.pgc"
 int ind [ 2 ] ;
/* exec sql end declare section */
#line 37 "bytea.pgc"

	int i, j, c;

#define init() { \
	for (i = 0; i < 2; ++i) \
	{ \
		memset(recv_buf[i].arr, 0x0, sizeof(recv_buf[i].arr)); \
		recv_buf[i].len = 0; \
		ind[i] = 0; \
	} \
	recv_vlen_buf = NULL, \
	memset(recv_short_buf.arr, 0x0, sizeof(recv_short_buf.arr)); \
} \
while (0)

	ECPGdebug(1, stderr);

	for (i = 0; i < 2; ++i)
	{
		for (j = 0, c = 0xff; (c == -1 ? c = 0xff : 1), j < DATA_SIZE; ++j, --c)
			send_buf[i].arr[j] = c;

		send_buf[i].len = DATA_SIZE;
	}

    { ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 62 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 62 "bytea.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table if not exists test ( data1 bytea , data2 bytea )", ECPGt_EOIT, ECPGt_EORT);
#line 64 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 64 "bytea.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "ins_stmt", "insert into test values(?,?)");
#line 66 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 66 "bytea.pgc"

	{ ECPGprepare(__LINE__, NULL, 0, "sel_stmt", "select data1,data2 from test");
#line 67 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 67 "bytea.pgc"

	ECPGallocate_desc(__LINE__, "idesc");
#line 68 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();
#line 68 "bytea.pgc"

	ECPGallocate_desc(__LINE__, "odesc");
#line 69 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();
#line 69 "bytea.pgc"


	/* Test for static sql statement with normal host variable, indicator */
	init();
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 73 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 73 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test values ( $1  , $2  )", 
	ECPGt_bytea,&(send_buf[0]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bytea,&(send_buf[1]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 74 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 74 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select data1 , data2 from test", ECPGt_EOIT, 
	ECPGt_bytea,&(recv_buf[0]),(long)DATA_SIZE,(long)1,sizeof(struct bytea_2), 
	ECPGt_int,&(ind[0]),(long)1,(long)1,sizeof(int), 
	ECPGt_bytea,&(recv_short_buf),(long)DATA_SIZE - LACK_SIZE,(long)1,sizeof(struct bytea_4), 
	ECPGt_int,&(ind[1]),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 75 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 75 "bytea.pgc"

	dump_binary(recv_buf[0].arr, recv_buf[0].len, ind[0]);
	dump_binary(recv_short_buf.arr, recv_short_buf.len, ind[1]);

	/* Test for cursor */
	init();
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 81 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 81 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test values ( $1  , $2  )", 
	ECPGt_bytea,&(send_buf[0]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bytea,&(send_buf[1]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 82 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 82 "bytea.pgc"

	ECPGset_var( 0, &( send_buf[0] ), __LINE__);\
 /* declare cursor1 cursor for select data1 from test where data1 = $1  */
#line 83 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();
#line 83 "bytea.pgc"

#line 83 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cursor1 cursor for select data1 from test where data1 = $1 ", 
	ECPGt_bytea,&(send_buf[0]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 84 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 84 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch from cursor1", ECPGt_EOIT, 
	ECPGt_bytea,&(recv_buf[0]),(long)DATA_SIZE,(long)1,sizeof(struct bytea_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 85 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 85 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cursor1", ECPGt_EOIT, ECPGt_EORT);
#line 86 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 86 "bytea.pgc"

	{ ECPGdeallocate(__LINE__, 0, NULL, "cursor1");
#line 87 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 87 "bytea.pgc"

	dump_binary(recv_buf[0].arr, recv_buf[0].len, 0);

	/* Test for variable length array */
	init();
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 92 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 92 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test values ( $1  , $2  )", 
	ECPGt_bytea,&(send_buf[0]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bytea,&(send_buf[1]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 93 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 93 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test values ( $1  , $2  )", 
	ECPGt_bytea,&(send_buf[0]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bytea,&(send_buf[1]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 94 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 94 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select data1 from test", ECPGt_EOIT, 
	ECPGt_bytea,&(recv_vlen_buf),(long)DATA_SIZE,(long)0,sizeof(struct bytea_3), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 95 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 95 "bytea.pgc"

	dump_binary(recv_vlen_buf[0].arr, recv_vlen_buf[0].len, 0);
	dump_binary(recv_vlen_buf[1].arr, recv_vlen_buf[1].len, 0);
	free(recv_vlen_buf);

	/* Test for dynamic sql statement with normal host variable, indicator */
	init();
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 102 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 102 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "ins_stmt", 
	ECPGt_bytea,&(send_buf[0]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bytea,&(send_buf[1]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 103 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 103 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "sel_stmt", ECPGt_EOIT, 
	ECPGt_bytea,&(recv_buf[0]),(long)DATA_SIZE,(long)1,sizeof(struct bytea_2), 
	ECPGt_int,&(ind[0]),(long)1,(long)1,sizeof(int), 
	ECPGt_bytea,&(recv_short_buf),(long)DATA_SIZE - LACK_SIZE,(long)1,sizeof(struct bytea_4), 
	ECPGt_int,&(ind[1]),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 104 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 104 "bytea.pgc"

	dump_binary(recv_buf[0].arr, recv_buf[0].len, ind[0]);
	dump_binary(recv_short_buf.arr, recv_short_buf.len, ind[1]);

	/* Test for dynamic sql statement with sql descriptor */
	init();
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 110 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 110 "bytea.pgc"

	{ ECPGset_desc(__LINE__, "idesc", 1,ECPGd_data,
	ECPGt_bytea,&(send_buf[0]),(long)512,(long)1,sizeof(struct bytea_1), ECPGd_EODT);

#line 111 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 111 "bytea.pgc"

	{ ECPGset_desc(__LINE__, "idesc", 2,ECPGd_data,
	ECPGt_bytea,&(send_buf[1]),(long)512,(long)1,sizeof(struct bytea_1), ECPGd_EODT);

#line 112 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 112 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "ins_stmt", 
	ECPGt_descriptor, "idesc", 1L, 1L, 1L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 113 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 113 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "sel_stmt", ECPGt_EOIT, 
	ECPGt_descriptor, "odesc", 1L, 1L, 1L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 114 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 114 "bytea.pgc"

	{ ECPGget_desc(__LINE__, "odesc", 1,ECPGd_indicator,
	ECPGt_int,&(ind[0]),(long)1,(long)1,sizeof(int), ECPGd_data,
	ECPGt_bytea,&(recv_buf[0]),(long)DATA_SIZE,(long)1,sizeof(struct bytea_2), ECPGd_EODT);

#line 115 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 115 "bytea.pgc"

	{ ECPGget_desc(__LINE__, "odesc", 2,ECPGd_indicator,
	ECPGt_int,&(ind[1]),(long)1,(long)1,sizeof(int), ECPGd_data,
	ECPGt_bytea,&(recv_short_buf),(long)DATA_SIZE - LACK_SIZE,(long)1,sizeof(struct bytea_4), ECPGd_EODT);

#line 116 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 116 "bytea.pgc"

	dump_binary(recv_buf[0].arr, recv_buf[0].len, ind[0]);
	dump_binary(recv_short_buf.arr, recv_short_buf.len, ind[1]);

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table test", ECPGt_EOIT, ECPGt_EORT);
#line 120 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 120 "bytea.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 121 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 121 "bytea.pgc"


	/* Tests for bind parameters output on error when params are sent in binary mode */

	/* Should not show parameters on error, since it fails before the params have been parsed */
	{ ECPGtrans(__LINE__, NULL, "begin");
#line 126 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 126 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "set log_parameter_max_length_on_error = - 1", ECPGt_EOIT, ECPGt_EORT);
#line 127 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 127 "bytea.pgc"

	{ ECPGprepare(__LINE__, NULL, 0, "errparams2", "SELECT $1::json");
#line 128 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 128 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "errparams2", 
	ECPGt_bytea,&(notjson),(long)1,(long)1,sizeof(struct bytea_5), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 129 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 129 "bytea.pgc"

	{ ECPGtrans(__LINE__, NULL, "rollback");
#line 130 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 130 "bytea.pgc"


	/* Should show parameters on error, since the failure is after the params have been parsed */
	{ ECPGtrans(__LINE__, NULL, "begin");
#line 133 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 133 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "set log_parameter_max_length_on_error = - 1", ECPGt_EOIT, ECPGt_EORT);
#line 134 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 134 "bytea.pgc"

	{ ECPGprepare(__LINE__, NULL, 0, "errparams1", "SELECT a::json FROM (SELECT $1 AS a) AS sq");
#line 135 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 135 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "errparams1", 
	ECPGt_bytea,&(notjson),(long)1,(long)1,sizeof(struct bytea_5), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 136 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 136 "bytea.pgc"

	{ ECPGtrans(__LINE__, NULL, "rollback");
#line 137 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 137 "bytea.pgc"


	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 139 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 139 "bytea.pgc"


	return 0;
}
