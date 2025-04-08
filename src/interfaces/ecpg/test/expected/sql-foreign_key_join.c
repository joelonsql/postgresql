/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "foreign_key_join.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* test foreign key join syntax parsing of <- and -> operators */

#line 1 "regression.h"






#line 6 "foreign_key_join.pgc"


int main() {
  /* exec sql begin declare section */
     
      
  
#line 10 "foreign_key_join.pgc"
 int result ;
 
#line 11 "foreign_key_join.pgc"
 int c2 , c4 ;
/* exec sql end declare section */
#line 12 "foreign_key_join.pgc"


  ECPGdebug(1, stderr);
  { ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); }
#line 15 "foreign_key_join.pgc"


  { ECPGsetcommit(__LINE__, "on", NULL);}
#line 17 "foreign_key_join.pgc"

  /* exec sql whenever sql_warning  sqlprint ; */
#line 18 "foreign_key_join.pgc"

  /* exec sql whenever sqlerror  sqlprint ; */
#line 19 "foreign_key_join.pgc"


  /* Create tables for testing */
  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table t1 ( c1 int not null , c2 int not null , constraint t1_pkey primary key ( c1 ) )", ECPGt_EOIT, ECPGt_EORT);
#line 26 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 26 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 26 "foreign_key_join.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table t2 ( c3 int not null , c4 int not null , constraint t2_pkey primary key ( c3 ) , constraint t2_c3_fkey foreign key ( c3 ) references t1 ( c1 ) )", ECPGt_EOIT, ECPGt_EORT);
#line 33 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 33 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 33 "foreign_key_join.pgc"


  /* Insert minimal data for testing */
  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into t1 ( c1 , c2 ) values ( 1 , 10 )", ECPGt_EOIT, ECPGt_EORT);
#line 36 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 36 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 36 "foreign_key_join.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into t1 ( c1 , c2 ) values ( 2 , 20 )", ECPGt_EOIT, ECPGt_EORT);
#line 37 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 37 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 37 "foreign_key_join.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into t2 ( c3 , c4 ) values ( 1 , 30 )", ECPGt_EOIT, ECPGt_EORT);
#line 38 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 38 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 38 "foreign_key_join.pgc"


  printf("Testing foreign key join parser with <- and -> operators\n");
  printf("=========================================================\n\n");

  /* Test that we didn't break the parser - this should parse as 1 < (-2) */
  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select ( 1 < - 2 ) :: int", ECPGt_EOIT, 
	ECPGt_int,&(result),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 44 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 44 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 44 "foreign_key_join.pgc"

  printf("SELECT (1<-2)::int: result = %d (should be 0confdefs.h)\n", result);

  /* These should all parse successfully */
  printf("\nTesting valid -> syntax variations:\n");

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select t1 . c2 , t2 . c4 from t1 join t2 key ( c3 ) -> t1 ( c1 )", ECPGt_EOIT, 
	ECPGt_int,&(c2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(c4),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 52 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 52 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 52 "foreign_key_join.pgc"

  printf("  SELECT * FROM t1 JOIN t2 KEY (c3) -> t1 (c1) -- ok (c2=%d, c4=%d)\n", c2, c4);

  /*comment*/{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select t1 . c2 , t2 . c4 from t1 join t2 key ( c3 ) -> t1 ( c1 )", ECPGt_EOIT, 
	ECPGt_int,&(c2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(c4),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 57 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 57 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 57 "foreign_key_join.pgc"

  printf("  SELECT * FROM t1 JOIN t2 KEY (c3) ->/*comment*/ t1 (c1) -- ok (c2=%d, c4=%d)\n", c2, c4);

  /*comment*/{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select t1 . c2 , t2 . c4 from t1 join t2 key ( c3 ) -> t1 ( c1 )", ECPGt_EOIT, 
	ECPGt_int,&(c2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(c4),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 62 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 62 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 62 "foreign_key_join.pgc"

  printf("  SELECT * FROM t1 JOIN t2 KEY (c3) /*comment*/-> t1 (c1) -- ok (c2=%d, c4=%d)\n", c2, c4);

  /*comment*//*comment*/{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select t1 . c2 , t2 . c4 from t1 join t2 key ( c3 ) -> t1 ( c1 )", ECPGt_EOIT, 
	ECPGt_int,&(c2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(c4),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 67 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 67 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 67 "foreign_key_join.pgc"

  printf("  SELECT * FROM t1 JOIN t2 KEY (c3) /*comment*/->/*comment*/ t1 (c1) -- ok (c2=%d, c4=%d)\n", c2, c4);

  /* These should all parse successfully */
  printf("\nTesting valid <- syntax variations:\n");

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select t1 . c2 , t2 . c4 from t2 join t1 key ( c1 ) < - t2 ( c3 )", ECPGt_EOIT, 
	ECPGt_int,&(c2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(c4),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 75 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 75 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 75 "foreign_key_join.pgc"

  printf("  SELECT * FROM t2 JOIN t1 KEY (c1) <- t2 (c3) -- ok (c2=%d, c4=%d)\n", c2, c4);

  /*comment*/{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select t1 . c2 , t2 . c4 from t2 join t1 key ( c1 ) < - t2 ( c3 )", ECPGt_EOIT, 
	ECPGt_int,&(c2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(c4),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 80 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 80 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 80 "foreign_key_join.pgc"

  printf("  SELECT * FROM t2 JOIN t1 KEY (c1) <-/*comment*/ t2 (c3) -- ok (c2=%d, c4=%d)\n", c2, c4);

  /*comment*/{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select t1 . c2 , t2 . c4 from t2 join t1 key ( c1 ) < - t2 ( c3 )", ECPGt_EOIT, 
	ECPGt_int,&(c2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(c4),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 85 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 85 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 85 "foreign_key_join.pgc"

  printf("  SELECT * FROM t2 JOIN t1 KEY (c1) /*comment*/<- t2 (c3) -- ok (c2=%d, c4=%d)\n", c2, c4);

  /*comment*//*comment*/{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select t1 . c2 , t2 . c4 from t2 join t1 key ( c1 ) < - t2 ( c3 )", ECPGt_EOIT, 
	ECPGt_int,&(c2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(c4),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 90 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 90 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 90 "foreign_key_join.pgc"

  printf("  SELECT * FROM t2 JOIN t1 KEY (c1) /*comment*/<-/*comment*/ t2 (c3) -- ok (c2=%d, c4=%d)\n", c2, c4);

  /* Test that < and - with space are parsed as separate operators, not <- */
  printf("\nTesting that < and - with space are separate operators:\n");

  /* This should work as a comparison: 1 < (-2) */
  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select ( 1 < - 2 ) :: int", ECPGt_EOIT, 
	ECPGt_int,&(result),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 97 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 97 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 97 "foreign_key_join.pgc"

  printf("  SELECT (1 < - 2)::int: result = %d (should be 0)\n", result);

  /* Test less-than operator followed by negative number */
  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select ( 3 < - 1 ) :: int", ECPGt_EOIT, 
	ECPGt_int,&(result),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 101 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 101 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 101 "foreign_key_join.pgc"

  printf("  SELECT (3 < -1)::int: result = %d (should be 0)\n", result);

  /* Test the ambiguous case: should parse as 3 < (-2) */
  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select ( 3 < - 2 ) :: int", ECPGt_EOIT, 
	ECPGt_int,&(result),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 105 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 105 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 105 "foreign_key_join.pgc"

  printf("  SELECT (3<-2)::int: result = %d (should be 0)\n", result);

  /* Test some cases that should be true */
  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select ( - 1 < 1 ) :: int", ECPGt_EOIT, 
	ECPGt_int,&(result),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 109 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 109 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 109 "foreign_key_join.pgc"

  printf("  SELECT (-1 < 1)::int: result = %d (should be 1)\n", result);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select ( - 3 < - 2 ) :: int", ECPGt_EOIT, 
	ECPGt_int,&(result),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 112 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 112 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 112 "foreign_key_join.pgc"

  printf("  SELECT (-3<-2)::int: result = %d (should be 1)\n", result);

  /* Clean up */
  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table t2", ECPGt_EOIT, ECPGt_EORT);
#line 116 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 116 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 116 "foreign_key_join.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table t1", ECPGt_EOIT, ECPGt_EORT);
#line 117 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 117 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 117 "foreign_key_join.pgc"


  { ECPGdisconnect(__LINE__, "ALL");
#line 119 "foreign_key_join.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 119 "foreign_key_join.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 119 "foreign_key_join.pgc"


  printf("\nAll parser tests completed successfully!\n");
  return 0;
}