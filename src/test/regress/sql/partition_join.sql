--
-- PARTITION_JOIN
-- Test partitionwise join between partitioned tables
--

-- Enable partitionwise join, which by default is disabled.
SET enable_partitionwise_join to true;

--
-- partitioned by a single column
--
CREATE TABLE prt1 (a int, b int, c varchar) PARTITION BY RANGE(a);
CREATE TABLE prt1_p1 PARTITION OF prt1 FOR VALUES FROM (0) TO (250);
CREATE TABLE prt1_p3 PARTITION OF prt1 FOR VALUES FROM (500) TO (600);
CREATE TABLE prt1_p2 PARTITION OF prt1 FOR VALUES FROM (250) TO (500);
INSERT INTO prt1 SELECT i, i % 25, to_char(i, 'FM0000') FROM generate_series(0, 599) i WHERE i % 2 = 0;
CREATE INDEX iprt1_p1_a on prt1_p1(a);
CREATE INDEX iprt1_p2_a on prt1_p2(a);
CREATE INDEX iprt1_p3_a on prt1_p3(a);
ANALYZE prt1;

CREATE TABLE prt2 (a int, b int, c varchar) PARTITION BY RANGE(b);
CREATE TABLE prt2_p1 PARTITION OF prt2 FOR VALUES FROM (0) TO (250);
CREATE TABLE prt2_p2 PARTITION OF prt2 FOR VALUES FROM (250) TO (500);
CREATE TABLE prt2_p3 PARTITION OF prt2 FOR VALUES FROM (500) TO (600);
INSERT INTO prt2 SELECT i % 25, i, to_char(i, 'FM0000') FROM generate_series(0, 599) i WHERE i % 3 = 0;
CREATE INDEX iprt2_p1_b on prt2_p1(b);
CREATE INDEX iprt2_p2_b on prt2_p2(b);
CREATE INDEX iprt2_p3_b on prt2_p3(b);
ANALYZE prt2;

-- inner join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt2 t2 WHERE t1.a = t2.b AND t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt2 t2 WHERE t1.a = t2.b AND t1.b = 0 ORDER BY t1.a, t2.b;

-- left outer join, with whole-row reference; partitionwise join does not apply
EXPLAIN (COSTS OFF)
SELECT t1, t2 FROM prt1 t1 LEFT JOIN prt2 t2 ON t1.a = t2.b WHERE t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1, t2 FROM prt1 t1 LEFT JOIN prt2 t2 ON t1.a = t2.b WHERE t1.b = 0 ORDER BY t1.a, t2.b;

-- right outer join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1 RIGHT JOIN prt2 t2 ON t1.a = t2.b WHERE t2.a = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1 RIGHT JOIN prt2 t2 ON t1.a = t2.b WHERE t2.a = 0 ORDER BY t1.a, t2.b;

-- full outer join, with placeholder vars
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT 50 phv, * FROM prt1 WHERE prt1.b = 0) t1 FULL JOIN (SELECT 75 phv, * FROM prt2 WHERE prt2.a = 0) t2 ON (t1.a = t2.b) WHERE t1.phv = t1.a OR t2.phv = t2.b ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT 50 phv, * FROM prt1 WHERE prt1.b = 0) t1 FULL JOIN (SELECT 75 phv, * FROM prt2 WHERE prt2.a = 0) t2 ON (t1.a = t2.b) WHERE t1.phv = t1.a OR t2.phv = t2.b ORDER BY t1.a, t2.b;

-- Join with pruned partitions from joining relations
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt2 t2 WHERE t1.a = t2.b AND t1.a < 450 AND t2.b > 250 AND t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt2 t2 WHERE t1.a = t2.b AND t1.a < 450 AND t2.b > 250 AND t1.b = 0 ORDER BY t1.a, t2.b;

-- Currently we can't do partitioned join if nullable-side partitions are pruned
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1 WHERE a < 450) t1 LEFT JOIN (SELECT * FROM prt2 WHERE b > 250) t2 ON t1.a = t2.b WHERE t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1 WHERE a < 450) t1 LEFT JOIN (SELECT * FROM prt2 WHERE b > 250) t2 ON t1.a = t2.b WHERE t1.b = 0 ORDER BY t1.a, t2.b;

-- Currently we can't do partitioned join if nullable-side partitions are pruned
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1 WHERE a < 450) t1 FULL JOIN (SELECT * FROM prt2 WHERE b > 250) t2 ON t1.a = t2.b WHERE t1.b = 0 OR t2.a = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1 WHERE a < 450) t1 FULL JOIN (SELECT * FROM prt2 WHERE b > 250) t2 ON t1.a = t2.b WHERE t1.b = 0 OR t2.a = 0 ORDER BY t1.a, t2.b;

-- Semi-join
EXPLAIN (COSTS OFF)
SELECT t1.* FROM prt1 t1 WHERE t1.a IN (SELECT t2.b FROM prt2 t2 WHERE t2.a = 0) AND t1.b = 0 ORDER BY t1.a;
SELECT t1.* FROM prt1 t1 WHERE t1.a IN (SELECT t2.b FROM prt2 t2 WHERE t2.a = 0) AND t1.b = 0 ORDER BY t1.a;

-- Anti-join with aggregates
EXPLAIN (COSTS OFF)
SELECT sum(t1.a), avg(t1.a), sum(t1.b), avg(t1.b) FROM prt1 t1 WHERE NOT EXISTS (SELECT 1 FROM prt2 t2 WHERE t1.a = t2.b);
SELECT sum(t1.a), avg(t1.a), sum(t1.b), avg(t1.b) FROM prt1 t1 WHERE NOT EXISTS (SELECT 1 FROM prt2 t2 WHERE t1.a = t2.b);

-- lateral reference
EXPLAIN (COSTS OFF)
SELECT * FROM prt1 t1 LEFT JOIN LATERAL
			  (SELECT t2.a AS t2a, t3.a AS t3a, least(t1.a,t2.a,t3.b) FROM prt1 t2 JOIN prt2 t3 ON (t2.a = t3.b)) ss
			  ON t1.a = ss.t2a WHERE t1.b = 0 ORDER BY t1.a;
SELECT * FROM prt1 t1 LEFT JOIN LATERAL
			  (SELECT t2.a AS t2a, t3.a AS t3a, least(t1.a,t2.a,t3.b) FROM prt1 t2 JOIN prt2 t3 ON (t2.a = t3.b)) ss
			  ON t1.a = ss.t2a WHERE t1.b = 0 ORDER BY t1.a;

EXPLAIN (COSTS OFF)
SELECT t1.a, ss.t2a, ss.t2c FROM prt1 t1 LEFT JOIN LATERAL
			  (SELECT t2.a AS t2a, t3.a AS t3a, t2.b t2b, t2.c t2c, least(t1.a,t2.a,t3.b) FROM prt1 t2 JOIN prt2 t3 ON (t2.a = t3.b)) ss
			  ON t1.c = ss.t2c WHERE (t1.b + coalesce(ss.t2b, 0)) = 0 ORDER BY t1.a;
SELECT t1.a, ss.t2a, ss.t2c FROM prt1 t1 LEFT JOIN LATERAL
			  (SELECT t2.a AS t2a, t3.a AS t3a, t2.b t2b, t2.c t2c, least(t1.a,t2.a,t3.a) FROM prt1 t2 JOIN prt2 t3 ON (t2.a = t3.b)) ss
			  ON t1.c = ss.t2c WHERE (t1.b + coalesce(ss.t2b, 0)) = 0 ORDER BY t1.a;

-- bug with inadequate sort key representation
SET enable_partitionwise_aggregate TO true;
SET enable_hashjoin TO false;

EXPLAIN (COSTS OFF)
SELECT a, b FROM prt1 FULL JOIN prt2 p2(b,a,c) USING(a,b)
  WHERE a BETWEEN 490 AND 510
  GROUP BY 1, 2 ORDER BY 1, 2;
SELECT a, b FROM prt1 FULL JOIN prt2 p2(b,a,c) USING(a,b)
  WHERE a BETWEEN 490 AND 510
  GROUP BY 1, 2 ORDER BY 1, 2;

RESET enable_partitionwise_aggregate;
RESET enable_hashjoin;

--
-- partitioned by expression
--
CREATE TABLE prt1_e (a int, b int, c int) PARTITION BY RANGE(((a + b)/2));
CREATE TABLE prt1_e_p1 PARTITION OF prt1_e FOR VALUES FROM (0) TO (250);
CREATE TABLE prt1_e_p2 PARTITION OF prt1_e FOR VALUES FROM (250) TO (500);
CREATE TABLE prt1_e_p3 PARTITION OF prt1_e FOR VALUES FROM (500) TO (600);
INSERT INTO prt1_e SELECT i, i, i % 25 FROM generate_series(0, 599, 2) i;
CREATE INDEX iprt1_e_p1_ab2 on prt1_e_p1(((a+b)/2));
CREATE INDEX iprt1_e_p2_ab2 on prt1_e_p2(((a+b)/2));
CREATE INDEX iprt1_e_p3_ab2 on prt1_e_p3(((a+b)/2));
ANALYZE prt1_e;

CREATE TABLE prt2_e (a int, b int, c int) PARTITION BY RANGE(((b + a)/2));
CREATE TABLE prt2_e_p1 PARTITION OF prt2_e FOR VALUES FROM (0) TO (250);
CREATE TABLE prt2_e_p2 PARTITION OF prt2_e FOR VALUES FROM (250) TO (500);
CREATE TABLE prt2_e_p3 PARTITION OF prt2_e FOR VALUES FROM (500) TO (600);
INSERT INTO prt2_e SELECT i, i, i % 25 FROM generate_series(0, 599, 3) i;
ANALYZE prt2_e;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_e t1, prt2_e t2 WHERE (t1.a + t1.b)/2 = (t2.b + t2.a)/2 AND t1.c = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_e t1, prt2_e t2 WHERE (t1.a + t1.b)/2 = (t2.b + t2.a)/2 AND t1.c = 0 ORDER BY t1.a, t2.b;

--
-- N-way join
--
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c, t3.a + t3.b, t3.c FROM prt1 t1, prt2 t2, prt1_e t3 WHERE t1.a = t2.b AND t1.a = (t3.a + t3.b)/2 AND t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c, t3.a + t3.b, t3.c FROM prt1 t1, prt2 t2, prt1_e t3 WHERE t1.a = t2.b AND t1.a = (t3.a + t3.b)/2 AND t1.b = 0 ORDER BY t1.a, t2.b;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c, t3.a + t3.b, t3.c FROM (prt1 t1 LEFT JOIN prt2 t2 ON t1.a = t2.b) LEFT JOIN prt1_e t3 ON (t1.a = (t3.a + t3.b)/2) WHERE t1.b = 0 ORDER BY t1.a, t2.b, t3.a + t3.b;
SELECT t1.a, t1.c, t2.b, t2.c, t3.a + t3.b, t3.c FROM (prt1 t1 LEFT JOIN prt2 t2 ON t1.a = t2.b) LEFT JOIN prt1_e t3 ON (t1.a = (t3.a + t3.b)/2) WHERE t1.b = 0 ORDER BY t1.a, t2.b, t3.a + t3.b;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c, t3.a + t3.b, t3.c FROM (prt1 t1 LEFT JOIN prt2 t2 ON t1.a = t2.b) RIGHT JOIN prt1_e t3 ON (t1.a = (t3.a + t3.b)/2) WHERE t3.c = 0 ORDER BY t1.a, t2.b, t3.a + t3.b;
SELECT t1.a, t1.c, t2.b, t2.c, t3.a + t3.b, t3.c FROM (prt1 t1 LEFT JOIN prt2 t2 ON t1.a = t2.b) RIGHT JOIN prt1_e t3 ON (t1.a = (t3.a + t3.b)/2) WHERE t3.c = 0 ORDER BY t1.a, t2.b, t3.a + t3.b;

-- Cases with non-nullable expressions in subquery results;
-- make sure these go to null as expected
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.phv, t2.b, t2.phv, t3.a + t3.b, t3.phv FROM ((SELECT 50 phv, * FROM prt1 WHERE prt1.b = 0) t1 FULL JOIN (SELECT 75 phv, * FROM prt2 WHERE prt2.a = 0) t2 ON (t1.a = t2.b)) FULL JOIN (SELECT 50 phv, * FROM prt1_e WHERE prt1_e.c = 0) t3 ON (t1.a = (t3.a + t3.b)/2) WHERE t1.a = t1.phv OR t2.b = t2.phv OR (t3.a + t3.b)/2 = t3.phv ORDER BY t1.a, t2.b, t3.a + t3.b;
SELECT t1.a, t1.phv, t2.b, t2.phv, t3.a + t3.b, t3.phv FROM ((SELECT 50 phv, * FROM prt1 WHERE prt1.b = 0) t1 FULL JOIN (SELECT 75 phv, * FROM prt2 WHERE prt2.a = 0) t2 ON (t1.a = t2.b)) FULL JOIN (SELECT 50 phv, * FROM prt1_e WHERE prt1_e.c = 0) t3 ON (t1.a = (t3.a + t3.b)/2) WHERE t1.a = t1.phv OR t2.b = t2.phv OR (t3.a + t3.b)/2 = t3.phv ORDER BY t1.a, t2.b, t3.a + t3.b;

-- Semi-join
EXPLAIN (COSTS OFF)
SELECT t1.* FROM prt1 t1 WHERE t1.a IN (SELECT t1.b FROM prt2 t1, prt1_e t2 WHERE t1.a = 0 AND t1.b = (t2.a + t2.b)/2) AND t1.b = 0 ORDER BY t1.a;
SELECT t1.* FROM prt1 t1 WHERE t1.a IN (SELECT t1.b FROM prt2 t1, prt1_e t2 WHERE t1.a = 0 AND t1.b = (t2.a + t2.b)/2) AND t1.b = 0 ORDER BY t1.a;

EXPLAIN (COSTS OFF)
SELECT t1.* FROM prt1 t1 WHERE t1.a IN (SELECT t1.b FROM prt2 t1 WHERE t1.b IN (SELECT (t1.a + t1.b)/2 FROM prt1_e t1 WHERE t1.c = 0)) AND t1.b = 0 ORDER BY t1.a;
SELECT t1.* FROM prt1 t1 WHERE t1.a IN (SELECT t1.b FROM prt2 t1 WHERE t1.b IN (SELECT (t1.a + t1.b)/2 FROM prt1_e t1 WHERE t1.c = 0)) AND t1.b = 0 ORDER BY t1.a;

-- test merge joins
SET enable_hashjoin TO off;
SET enable_nestloop TO off;

EXPLAIN (COSTS OFF)
SELECT t1.* FROM prt1 t1 WHERE t1.a IN (SELECT t1.b FROM prt2 t1 WHERE t1.b IN (SELECT (t1.a + t1.b)/2 FROM prt1_e t1 WHERE t1.c = 0)) AND t1.b = 0 ORDER BY t1.a;
SELECT t1.* FROM prt1 t1 WHERE t1.a IN (SELECT t1.b FROM prt2 t1 WHERE t1.b IN (SELECT (t1.a + t1.b)/2 FROM prt1_e t1 WHERE t1.c = 0)) AND t1.b = 0 ORDER BY t1.a;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c, t3.a + t3.b, t3.c FROM (prt1 t1 LEFT JOIN prt2 t2 ON t1.a = t2.b) RIGHT JOIN prt1_e t3 ON (t1.a = (t3.a + t3.b)/2) WHERE t3.c = 0 ORDER BY t1.a, t2.b, t3.a + t3.b;
SELECT t1.a, t1.c, t2.b, t2.c, t3.a + t3.b, t3.c FROM (prt1 t1 LEFT JOIN prt2 t2 ON t1.a = t2.b) RIGHT JOIN prt1_e t3 ON (t1.a = (t3.a + t3.b)/2) WHERE t3.c = 0 ORDER BY t1.a, t2.b, t3.a + t3.b;

-- MergeAppend on nullable column
-- This should generate a partitionwise join, but currently fails to
EXPLAIN (COSTS OFF)
SELECT t1.a, t2.b FROM (SELECT * FROM prt1 WHERE a < 450) t1 LEFT JOIN (SELECT * FROM prt2 WHERE b > 250) t2 ON t1.a = t2.b WHERE t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t2.b FROM (SELECT * FROM prt1 WHERE a < 450) t1 LEFT JOIN (SELECT * FROM prt2 WHERE b > 250) t2 ON t1.a = t2.b WHERE t1.b = 0 ORDER BY t1.a, t2.b;

-- merge join when expression with whole-row reference needs to be sorted;
-- partitionwise join does not apply
EXPLAIN (COSTS OFF)
SELECT t1.a, t2.b FROM prt1 t1, prt2 t2 WHERE t1::text = t2::text AND t1.a = t2.b ORDER BY t1.a;
SELECT t1.a, t2.b FROM prt1 t1, prt2 t2 WHERE t1::text = t2::text AND t1.a = t2.b ORDER BY t1.a;

RESET enable_hashjoin;
RESET enable_nestloop;

--
-- partitioned by multiple columns
--
CREATE TABLE prt1_m (a int, b int, c int) PARTITION BY RANGE(a, ((a + b)/2));
CREATE TABLE prt1_m_p1 PARTITION OF prt1_m FOR VALUES FROM (0, 0) TO (250, 250);
CREATE TABLE prt1_m_p2 PARTITION OF prt1_m FOR VALUES FROM (250, 250) TO (500, 500);
CREATE TABLE prt1_m_p3 PARTITION OF prt1_m FOR VALUES FROM (500, 500) TO (600, 600);
INSERT INTO prt1_m SELECT i, i, i % 25 FROM generate_series(0, 599, 2) i;
ANALYZE prt1_m;

CREATE TABLE prt2_m (a int, b int, c int) PARTITION BY RANGE(((b + a)/2), b);
CREATE TABLE prt2_m_p1 PARTITION OF prt2_m FOR VALUES FROM (0, 0) TO (250, 250);
CREATE TABLE prt2_m_p2 PARTITION OF prt2_m FOR VALUES FROM (250, 250) TO (500, 500);
CREATE TABLE prt2_m_p3 PARTITION OF prt2_m FOR VALUES FROM (500, 500) TO (600, 600);
INSERT INTO prt2_m SELECT i, i, i % 25 FROM generate_series(0, 599, 3) i;
ANALYZE prt2_m;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1_m WHERE prt1_m.c = 0) t1 FULL JOIN (SELECT * FROM prt2_m WHERE prt2_m.c = 0) t2 ON (t1.a = (t2.b + t2.a)/2 AND t2.b = (t1.a + t1.b)/2) ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1_m WHERE prt1_m.c = 0) t1 FULL JOIN (SELECT * FROM prt2_m WHERE prt2_m.c = 0) t2 ON (t1.a = (t2.b + t2.a)/2 AND t2.b = (t1.a + t1.b)/2) ORDER BY t1.a, t2.b;

--
-- tests for list partitioned tables.
--
CREATE TABLE plt1 (a int, b int, c text) PARTITION BY LIST(c);
CREATE TABLE plt1_p1 PARTITION OF plt1 FOR VALUES IN ('0000', '0003', '0004', '0010');
CREATE TABLE plt1_p2 PARTITION OF plt1 FOR VALUES IN ('0001', '0005', '0002', '0009');
CREATE TABLE plt1_p3 PARTITION OF plt1 FOR VALUES IN ('0006', '0007', '0008', '0011');
INSERT INTO plt1 SELECT i, i, to_char(i/50, 'FM0000') FROM generate_series(0, 599, 2) i;
ANALYZE plt1;

CREATE TABLE plt2 (a int, b int, c text) PARTITION BY LIST(c);
CREATE TABLE plt2_p1 PARTITION OF plt2 FOR VALUES IN ('0000', '0003', '0004', '0010');
CREATE TABLE plt2_p2 PARTITION OF plt2 FOR VALUES IN ('0001', '0005', '0002', '0009');
CREATE TABLE plt2_p3 PARTITION OF plt2 FOR VALUES IN ('0006', '0007', '0008', '0011');
INSERT INTO plt2 SELECT i, i, to_char(i/50, 'FM0000') FROM generate_series(0, 599, 3) i;
ANALYZE plt2;

--
-- list partitioned by expression
--
CREATE TABLE plt1_e (a int, b int, c text) PARTITION BY LIST(ltrim(c, 'A'));
CREATE TABLE plt1_e_p1 PARTITION OF plt1_e FOR VALUES IN ('0000', '0003', '0004', '0010');
CREATE TABLE plt1_e_p2 PARTITION OF plt1_e FOR VALUES IN ('0001', '0005', '0002', '0009');
CREATE TABLE plt1_e_p3 PARTITION OF plt1_e FOR VALUES IN ('0006', '0007', '0008', '0011');
INSERT INTO plt1_e SELECT i, i, 'A' || to_char(i/50, 'FM0000') FROM generate_series(0, 599, 2) i;
ANALYZE plt1_e;

-- test partition matching with N-way join
EXPLAIN (COSTS OFF)
SELECT avg(t1.a), avg(t2.b), avg(t3.a + t3.b), t1.c, t2.c, t3.c FROM plt1 t1, plt2 t2, plt1_e t3 WHERE t1.b = t2.b AND t1.c = t2.c AND ltrim(t3.c, 'A') = t1.c GROUP BY t1.c, t2.c, t3.c ORDER BY t1.c, t2.c, t3.c;
SELECT avg(t1.a), avg(t2.b), avg(t3.a + t3.b), t1.c, t2.c, t3.c FROM plt1 t1, plt2 t2, plt1_e t3 WHERE t1.b = t2.b AND t1.c = t2.c AND ltrim(t3.c, 'A') = t1.c GROUP BY t1.c, t2.c, t3.c ORDER BY t1.c, t2.c, t3.c;

-- joins where one of the relations is proven empty
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt2 t2 WHERE t1.a = t2.b AND t1.a = 1 AND t1.a = 2;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1 WHERE a = 1 AND a = 2) t1 LEFT JOIN prt2 t2 ON t1.a = t2.b;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1 WHERE a = 1 AND a = 2) t1 RIGHT JOIN prt2 t2 ON t1.a = t2.b, prt1 t3 WHERE t2.b = t3.a;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1 WHERE a = 1 AND a = 2) t1 FULL JOIN prt2 t2 ON t1.a = t2.b WHERE t2.a = 0 ORDER BY t1.a, t2.b;

--
-- tests for hash partitioned tables.
--
CREATE TABLE pht1 (a int, b int, c text) PARTITION BY HASH(c);
CREATE TABLE pht1_p1 PARTITION OF pht1 FOR VALUES WITH (MODULUS 3, REMAINDER 0);
CREATE TABLE pht1_p2 PARTITION OF pht1 FOR VALUES WITH (MODULUS 3, REMAINDER 1);
CREATE TABLE pht1_p3 PARTITION OF pht1 FOR VALUES WITH (MODULUS 3, REMAINDER 2);
INSERT INTO pht1 SELECT i, i, to_char(i/50, 'FM0000') FROM generate_series(0, 599, 2) i;
ANALYZE pht1;

CREATE TABLE pht2 (a int, b int, c text) PARTITION BY HASH(c);
CREATE TABLE pht2_p1 PARTITION OF pht2 FOR VALUES WITH (MODULUS 3, REMAINDER 0);
CREATE TABLE pht2_p2 PARTITION OF pht2 FOR VALUES WITH (MODULUS 3, REMAINDER 1);
CREATE TABLE pht2_p3 PARTITION OF pht2 FOR VALUES WITH (MODULUS 3, REMAINDER 2);
INSERT INTO pht2 SELECT i, i, to_char(i/50, 'FM0000') FROM generate_series(0, 599, 3) i;
ANALYZE pht2;

--
-- hash partitioned by expression
--
CREATE TABLE pht1_e (a int, b int, c text) PARTITION BY HASH(ltrim(c, 'A'));
CREATE TABLE pht1_e_p1 PARTITION OF pht1_e FOR VALUES WITH (MODULUS 3, REMAINDER 0);
CREATE TABLE pht1_e_p2 PARTITION OF pht1_e FOR VALUES WITH (MODULUS 3, REMAINDER 1);
CREATE TABLE pht1_e_p3 PARTITION OF pht1_e FOR VALUES WITH (MODULUS 3, REMAINDER 2);
INSERT INTO pht1_e SELECT i, i, 'A' || to_char(i/50, 'FM0000') FROM generate_series(0, 299, 2) i;
ANALYZE pht1_e;

-- test partition matching with N-way join
EXPLAIN (COSTS OFF)
SELECT avg(t1.a), avg(t2.b), avg(t3.a + t3.b), t1.c, t2.c, t3.c FROM pht1 t1, pht2 t2, pht1_e t3 WHERE t1.b = t2.b AND t1.c = t2.c AND ltrim(t3.c, 'A') = t1.c GROUP BY t1.c, t2.c, t3.c ORDER BY t1.c, t2.c, t3.c;
SELECT avg(t1.a), avg(t2.b), avg(t3.a + t3.b), t1.c, t2.c, t3.c FROM pht1 t1, pht2 t2, pht1_e t3 WHERE t1.b = t2.b AND t1.c = t2.c AND ltrim(t3.c, 'A') = t1.c GROUP BY t1.c, t2.c, t3.c ORDER BY t1.c, t2.c, t3.c;

-- test default partition behavior for range
ALTER TABLE prt1 DETACH PARTITION prt1_p3;
ALTER TABLE prt1 ATTACH PARTITION prt1_p3 DEFAULT;
ANALYZE prt1;
ALTER TABLE prt2 DETACH PARTITION prt2_p3;
ALTER TABLE prt2 ATTACH PARTITION prt2_p3 DEFAULT;
ANALYZE prt2;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt2 t2 WHERE t1.a = t2.b AND t1.b = 0 ORDER BY t1.a, t2.b;

-- test default partition behavior for list
ALTER TABLE plt1 DETACH PARTITION plt1_p3;
ALTER TABLE plt1 ATTACH PARTITION plt1_p3 DEFAULT;
ANALYZE plt1;
ALTER TABLE plt2 DETACH PARTITION plt2_p3;
ALTER TABLE plt2 ATTACH PARTITION plt2_p3 DEFAULT;
ANALYZE plt2;

EXPLAIN (COSTS OFF)
SELECT avg(t1.a), avg(t2.b), t1.c, t2.c FROM plt1 t1 RIGHT JOIN plt2 t2 ON t1.c = t2.c WHERE t1.a % 25 = 0 GROUP BY t1.c, t2.c ORDER BY t1.c, t2.c;
--
-- multiple levels of partitioning
--
CREATE TABLE prt1_l (a int, b int, c varchar) PARTITION BY RANGE(a);
CREATE TABLE prt1_l_p1 PARTITION OF prt1_l FOR VALUES FROM (0) TO (250);
CREATE TABLE prt1_l_p2 PARTITION OF prt1_l FOR VALUES FROM (250) TO (500) PARTITION BY LIST (c);
CREATE TABLE prt1_l_p2_p1 PARTITION OF prt1_l_p2 FOR VALUES IN ('0000', '0001');
CREATE TABLE prt1_l_p2_p2 PARTITION OF prt1_l_p2 FOR VALUES IN ('0002', '0003');
CREATE TABLE prt1_l_p3 PARTITION OF prt1_l FOR VALUES FROM (500) TO (600) PARTITION BY RANGE (b);
CREATE TABLE prt1_l_p3_p1 PARTITION OF prt1_l_p3 FOR VALUES FROM (0) TO (13);
CREATE TABLE prt1_l_p3_p2 PARTITION OF prt1_l_p3 FOR VALUES FROM (13) TO (25);
INSERT INTO prt1_l SELECT i, i % 25, to_char(i % 4, 'FM0000') FROM generate_series(0, 599, 2) i;
ANALYZE prt1_l;

CREATE TABLE prt2_l (a int, b int, c varchar) PARTITION BY RANGE(b);
CREATE TABLE prt2_l_p1 PARTITION OF prt2_l FOR VALUES FROM (0) TO (250);
CREATE TABLE prt2_l_p2 PARTITION OF prt2_l FOR VALUES FROM (250) TO (500) PARTITION BY LIST (c);
CREATE TABLE prt2_l_p2_p1 PARTITION OF prt2_l_p2 FOR VALUES IN ('0000', '0001');
CREATE TABLE prt2_l_p2_p2 PARTITION OF prt2_l_p2 FOR VALUES IN ('0002', '0003');
CREATE TABLE prt2_l_p3 PARTITION OF prt2_l FOR VALUES FROM (500) TO (600) PARTITION BY RANGE (a);
CREATE TABLE prt2_l_p3_p1 PARTITION OF prt2_l_p3 FOR VALUES FROM (0) TO (13);
CREATE TABLE prt2_l_p3_p2 PARTITION OF prt2_l_p3 FOR VALUES FROM (13) TO (25);
INSERT INTO prt2_l SELECT i % 25, i, to_char(i % 4, 'FM0000') FROM generate_series(0, 599, 3) i;
ANALYZE prt2_l;

-- inner join, qual covering only top-level partitions
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_l t1, prt2_l t2 WHERE t1.a = t2.b AND t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_l t1, prt2_l t2 WHERE t1.a = t2.b AND t1.b = 0 ORDER BY t1.a, t2.b;

-- left join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_l t1 LEFT JOIN prt2_l t2 ON t1.a = t2.b AND t1.c = t2.c WHERE t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_l t1 LEFT JOIN prt2_l t2 ON t1.a = t2.b AND t1.c = t2.c WHERE t1.b = 0 ORDER BY t1.a, t2.b;

-- right join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_l t1 RIGHT JOIN prt2_l t2 ON t1.a = t2.b AND t1.c = t2.c WHERE t2.a = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_l t1 RIGHT JOIN prt2_l t2 ON t1.a = t2.b AND t1.c = t2.c WHERE t2.a = 0 ORDER BY t1.a, t2.b;

-- full join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1_l WHERE prt1_l.b = 0) t1 FULL JOIN (SELECT * FROM prt2_l WHERE prt2_l.a = 0) t2 ON (t1.a = t2.b AND t1.c = t2.c) ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1_l WHERE prt1_l.b = 0) t1 FULL JOIN (SELECT * FROM prt2_l WHERE prt2_l.a = 0) t2 ON (t1.a = t2.b AND t1.c = t2.c) ORDER BY t1.a, t2.b;

-- lateral partitionwise join
EXPLAIN (COSTS OFF)
SELECT * FROM prt1_l t1 LEFT JOIN LATERAL
			  (SELECT t2.a AS t2a, t2.c AS t2c, t2.b AS t2b, t3.b AS t3b, least(t1.a,t2.a,t3.b) FROM prt1_l t2 JOIN prt2_l t3 ON (t2.a = t3.b AND t2.c = t3.c)) ss
			  ON t1.a = ss.t2a AND t1.c = ss.t2c WHERE t1.b = 0 ORDER BY t1.a;
SELECT * FROM prt1_l t1 LEFT JOIN LATERAL
			  (SELECT t2.a AS t2a, t2.c AS t2c, t2.b AS t2b, t3.b AS t3b, least(t1.a,t2.a,t3.b) FROM prt1_l t2 JOIN prt2_l t3 ON (t2.a = t3.b AND t2.c = t3.c)) ss
			  ON t1.a = ss.t2a AND t1.c = ss.t2c WHERE t1.b = 0 ORDER BY t1.a;

-- join with one side empty
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT * FROM prt1_l WHERE a = 1 AND a = 2) t1 RIGHT JOIN prt2_l t2 ON t1.a = t2.b AND t1.b = t2.a AND t1.c = t2.c;

-- Test case to verify proper handling of subqueries in a partitioned delete.
-- The weird-looking lateral join is just there to force creation of a
-- nestloop parameter within the subquery, which exposes the problem if the
-- planner fails to make multiple copies of the subquery as appropriate.
EXPLAIN (COSTS OFF)
DELETE FROM prt1_l
WHERE EXISTS (
  SELECT 1
    FROM int4_tbl,
         LATERAL (SELECT int4_tbl.f1 FROM int8_tbl LIMIT 2) ss
    WHERE prt1_l.c IS NULL);

--
-- negative testcases
--
CREATE TABLE prt1_n (a int, b int, c varchar) PARTITION BY RANGE(c);
CREATE TABLE prt1_n_p1 PARTITION OF prt1_n FOR VALUES FROM ('0000') TO ('0250');
CREATE TABLE prt1_n_p2 PARTITION OF prt1_n FOR VALUES FROM ('0250') TO ('0500');
INSERT INTO prt1_n SELECT i, i, to_char(i, 'FM0000') FROM generate_series(0, 499, 2) i;
ANALYZE prt1_n;

CREATE TABLE prt2_n (a int, b int, c text) PARTITION BY LIST(c);
CREATE TABLE prt2_n_p1 PARTITION OF prt2_n FOR VALUES IN ('0000', '0003', '0004', '0010', '0006', '0007');
CREATE TABLE prt2_n_p2 PARTITION OF prt2_n FOR VALUES IN ('0001', '0005', '0002', '0009', '0008', '0011');
INSERT INTO prt2_n SELECT i, i, to_char(i/50, 'FM0000') FROM generate_series(0, 599, 2) i;
ANALYZE prt2_n;

CREATE TABLE prt3_n (a int, b int, c text) PARTITION BY LIST(c);
CREATE TABLE prt3_n_p1 PARTITION OF prt3_n FOR VALUES IN ('0000', '0004', '0006', '0007');
CREATE TABLE prt3_n_p2 PARTITION OF prt3_n FOR VALUES IN ('0001', '0002', '0008', '0010');
CREATE TABLE prt3_n_p3 PARTITION OF prt3_n FOR VALUES IN ('0003', '0005', '0009', '0011');
INSERT INTO prt2_n SELECT i, i, to_char(i/50, 'FM0000') FROM generate_series(0, 599, 2) i;
ANALYZE prt3_n;

CREATE TABLE prt4_n (a int, b int, c text) PARTITION BY RANGE(a);
CREATE TABLE prt4_n_p1 PARTITION OF prt4_n FOR VALUES FROM (0) TO (300);
CREATE TABLE prt4_n_p2 PARTITION OF prt4_n FOR VALUES FROM (300) TO (500);
CREATE TABLE prt4_n_p3 PARTITION OF prt4_n FOR VALUES FROM (500) TO (600);
INSERT INTO prt4_n SELECT i, i, to_char(i, 'FM0000') FROM generate_series(0, 599, 2) i;
ANALYZE prt4_n;

-- partitionwise join can not be applied if the partition ranges differ
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt4_n t2 WHERE t1.a = t2.a;
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1, prt4_n t2, prt2 t3 WHERE t1.a = t2.a and t1.a = t3.b;

-- partitionwise join can not be applied if there are no equi-join conditions
-- between partition keys
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1 t1 LEFT JOIN prt2 t2 ON (t1.a < t2.b);

-- equi-join with join condition on partial keys does not qualify for
-- partitionwise join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_m t1, prt2_m t2 WHERE t1.a = (t2.b + t2.a)/2;

-- equi-join between out-of-order partition key columns does not qualify for
-- partitionwise join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_m t1 LEFT JOIN prt2_m t2 ON t1.a = t2.b;

-- equi-join between non-key columns does not qualify for partitionwise join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_m t1 LEFT JOIN prt2_m t2 ON t1.c = t2.c;

-- partitionwise join can not be applied for a join between list and range
-- partitioned tables
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_n t1 LEFT JOIN prt2_n t2 ON (t1.c = t2.c);

-- partitionwise join can not be applied between tables with different
-- partition lists
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_n t1 JOIN prt2_n t2 ON (t1.c = t2.c) JOIN plt1 t3 ON (t1.c = t3.c);

-- partitionwise join can not be applied for a join between key column and
-- non-key column
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_n t1 FULL JOIN prt1 t2 ON (t1.c = t2.c);


--
-- Test advanced partition-matching algorithm for partitioned join
--

-- Tests for range-partitioned tables
CREATE TABLE prt1_ad (a int, b int, c varchar) PARTITION BY RANGE (a);
CREATE TABLE prt1_ad_p1 PARTITION OF prt1_ad FOR VALUES FROM (100) TO (200);
CREATE TABLE prt1_ad_p2 PARTITION OF prt1_ad FOR VALUES FROM (200) TO (300);
CREATE TABLE prt1_ad_p3 PARTITION OF prt1_ad FOR VALUES FROM (300) TO (400);
CREATE INDEX prt1_ad_a_idx on prt1_ad (a);
INSERT INTO prt1_ad SELECT i, i % 25, to_char(i, 'FM0000') FROM generate_series(100, 399) i;
ANALYZE prt1_ad;

CREATE TABLE prt2_ad (a int, b int, c varchar) PARTITION BY RANGE (b);
CREATE TABLE prt2_ad_p1 PARTITION OF prt2_ad FOR VALUES FROM (100) TO (150);
CREATE TABLE prt2_ad_p2 PARTITION OF prt2_ad FOR VALUES FROM (200) TO (300);
CREATE TABLE prt2_ad_p3 PARTITION OF prt2_ad FOR VALUES FROM (350) TO (500);
CREATE INDEX prt2_ad_b_idx on prt2_ad (b);
INSERT INTO prt2_ad_p1 SELECT i % 25, i, to_char(i, 'FM0000') FROM generate_series(100, 149) i;
INSERT INTO prt2_ad_p2 SELECT i % 25, i, to_char(i, 'FM0000') FROM generate_series(200, 299) i;
INSERT INTO prt2_ad_p3 SELECT i % 25, i, to_char(i, 'FM0000') FROM generate_series(350, 499) i;
ANALYZE prt2_ad;

-- inner join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_ad t1 INNER JOIN prt2_ad t2 ON (t1.a = t2.b) WHERE t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_ad t1 INNER JOIN prt2_ad t2 ON (t1.a = t2.b) WHERE t1.b = 0 ORDER BY t1.a, t2.b;

-- semi join
EXPLAIN (COSTS OFF)
SELECT t1.* FROM prt1_ad t1 WHERE EXISTS (SELECT 1 FROM prt2_ad t2 WHERE t1.a = t2.b) AND t1.b = 0 ORDER BY t1.a;
SELECT t1.* FROM prt1_ad t1 WHERE EXISTS (SELECT 1 FROM prt2_ad t2 WHERE t1.a = t2.b) AND t1.b = 0 ORDER BY t1.a;

-- left join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_ad t1 LEFT JOIN prt2_ad t2 ON (t1.a = t2.b) WHERE t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_ad t1 LEFT JOIN prt2_ad t2 ON (t1.a = t2.b) WHERE t1.b = 0 ORDER BY t1.a, t2.b;

-- anti join
EXPLAIN (COSTS OFF)
SELECT t1.* FROM prt1_ad t1 WHERE NOT EXISTS (SELECT 1 FROM prt2_ad t2 WHERE t1.a = t2.b) AND t1.b = 0;
SELECT t1.* FROM prt1_ad t1 WHERE NOT EXISTS (SELECT 1 FROM prt2_ad t2 WHERE t1.a = t2.b) AND t1.b = 0;

-- full join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT 175 phv, * FROM prt1_ad WHERE prt1_ad.b = 0) t1 FULL JOIN (SELECT 425 phv, * FROM prt2_ad WHERE prt2_ad.a = 0) t2 ON (t1.a = t2.b) WHERE t1.phv = t1.a OR t2.phv = t2.b ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT 175 phv, * FROM prt1_ad WHERE prt1_ad.b = 0) t1 FULL JOIN (SELECT 425 phv, * FROM prt2_ad WHERE prt2_ad.a = 0) t2 ON (t1.a = t2.b) WHERE t1.phv = t1.a OR t2.phv = t2.b ORDER BY t1.a, t2.b;

-- Test cases where one side has an extra partition

-- Add an extra partition to prt2_ad
CREATE TABLE prt2_ad_extra PARTITION OF prt2_ad FOR VALUES FROM (500) TO (MAXVALUE);
INSERT INTO prt2_ad SELECT i % 25, i, to_char(i, 'FM0000') FROM generate_series(500, 599) i;
ANALYZE prt2_ad;

-- inner join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_ad t1 INNER JOIN prt2_ad t2 ON (t1.a = t2.b) WHERE t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_ad t1 INNER JOIN prt2_ad t2 ON (t1.a = t2.b) WHERE t1.b = 0 ORDER BY t1.a, t2.b;

-- semi join
EXPLAIN (COSTS OFF)
SELECT t1.* FROM prt1_ad t1 WHERE EXISTS (SELECT 1 FROM prt2_ad t2 WHERE t1.a = t2.b) AND t1.b = 0 ORDER BY t1.a;
SELECT t1.* FROM prt1_ad t1 WHERE EXISTS (SELECT 1 FROM prt2_ad t2 WHERE t1.a = t2.b) AND t1.b = 0 ORDER BY t1.a;

-- left join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_ad t1 LEFT JOIN prt2_ad t2 ON (t1.a = t2.b) WHERE t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_ad t1 LEFT JOIN prt2_ad t2 ON (t1.a = t2.b) WHERE t1.b = 0 ORDER BY t1.a, t2.b;

-- left join; currently we can't do partitioned join if there are no matched
-- partitions on the nullable side
EXPLAIN (COSTS OFF)
SELECT t1.b, t1.c, t2.a, t2.c FROM prt2_ad t1 LEFT JOIN prt1_ad t2 ON (t1.b = t2.a) WHERE t1.a = 0 ORDER BY t1.b, t2.a;

-- anti join
EXPLAIN (COSTS OFF)
SELECT t1.* FROM prt1_ad t1 WHERE NOT EXISTS (SELECT 1 FROM prt2_ad t2 WHERE t1.a = t2.b) AND t1.b = 0;
SELECT t1.* FROM prt1_ad t1 WHERE NOT EXISTS (SELECT 1 FROM prt2_ad t2 WHERE t1.a = t2.b) AND t1.b = 0;

-- anti join; currently we can't do partitioned join if there are no matched
-- partitions on the nullable side
EXPLAIN (COSTS OFF)
SELECT t1.* FROM prt2_ad t1 WHERE NOT EXISTS (SELECT 1 FROM prt1_ad t2 WHERE t1.b = t2.a) AND t1.a = 0;

-- full join; currently we can't do partitioned join if there are no matched
-- partitions on the nullable side
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT 175 phv, * FROM prt1_ad WHERE prt1_ad.b = 0) t1 FULL JOIN (SELECT 425 phv, * FROM prt2_ad WHERE prt2_ad.a = 0) t2 ON (t1.a = t2.b) WHERE t1.phv = t1.a OR t2.phv = t2.b ORDER BY t1.a, t2.b;

-- 3-way join where not every pair of relations can do partitioned join
EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a, t3.c FROM prt1_ad t1 RIGHT JOIN prt2_ad t2 ON (t1.a = t2.b) INNER JOIN prt1_ad t3 ON (t2.b = t3.a) WHERE t1.b = 0 ORDER BY t1.a, t2.a, t3.c;
SELECT t1.a, t2.a, t3.c FROM prt1_ad t1 RIGHT JOIN prt2_ad t2 ON (t1.a = t2.b) INNER JOIN prt1_ad t3 ON (t2.b = t3.a) WHERE t1.b = 0 ORDER BY t1.a, t2.a, t3.c;

DROP TABLE prt2_ad_extra;

-- Test cases where a partition on one side matches multiple partitions on
-- the other side; we currently can't do partitioned join in those cases

-- Split prt2_ad_p3 into two partitions so that prt1_ad_p3 matches both
-- partitions
ALTER TABLE prt2_ad DETACH PARTITION prt2_ad_p3;
CREATE TABLE prt2_ad_p3_350_375 PARTITION OF prt2_ad FOR VALUES FROM (350) TO (375);
CREATE TABLE prt2_ad_p3_375_500 PARTITION OF prt2_ad FOR VALUES FROM (375) TO (500);
INSERT INTO prt2_ad SELECT i % 25, i, to_char(i, 'FM0000') FROM generate_series(350, 499) i;
ANALYZE prt2_ad;

-- inner join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_ad t1 INNER JOIN prt2_ad t2 ON (t1.a = t2.b) WHERE t1.b = 0 ORDER BY t1.a, t2.b;

-- semi join
EXPLAIN (COSTS OFF)
SELECT t1.* FROM prt1_ad t1 WHERE EXISTS (SELECT 1 FROM prt2_ad t2 WHERE t1.a = t2.b) AND t1.b = 0 ORDER BY t1.a;

-- left join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_ad t1 LEFT JOIN prt2_ad t2 ON (t1.a = t2.b) WHERE t1.b = 0 ORDER BY t1.a, t2.b;

-- anti join
EXPLAIN (COSTS OFF)
SELECT t1.* FROM prt1_ad t1 WHERE NOT EXISTS (SELECT 1 FROM prt2_ad t2 WHERE t1.a = t2.b) AND t1.b = 0;

-- full join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM (SELECT 175 phv, * FROM prt1_ad WHERE prt1_ad.b = 0) t1 FULL JOIN (SELECT 425 phv, * FROM prt2_ad WHERE prt2_ad.a = 0) t2 ON (t1.a = t2.b) WHERE t1.phv = t1.a OR t2.phv = t2.b ORDER BY t1.a, t2.b;

DROP TABLE prt2_ad_p3_350_375;
DROP TABLE prt2_ad_p3_375_500;

-- Test default partitions

-- Change prt1_ad_p1 to the default partition
ALTER TABLE prt1_ad DETACH PARTITION prt1_ad_p1;
ALTER TABLE prt1_ad ATTACH PARTITION prt1_ad_p1 DEFAULT;
ANALYZE prt1_ad;

CREATE TABLE prt2_ad_p3_300_400 PARTITION OF prt2_ad FOR VALUES FROM (300) TO (400);
INSERT INTO prt2_ad SELECT i % 25, i, to_char(i, 'FM0000') FROM generate_series(300, 399) i;
ANALYZE prt2_ad;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_ad t1 INNER JOIN prt2_ad t2 ON (t1.a = t2.b) WHERE t1.b = 0 ORDER BY t1.a, t2.b;
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_ad t1 INNER JOIN prt2_ad t2 ON (t1.a = t2.b) WHERE t1.b = 0 ORDER BY t1.a, t2.b;

DROP TABLE prt2_ad_p3_300_400;

-- Restore prt2_ad_p3
ALTER TABLE prt2_ad ATTACH PARTITION prt2_ad_p3 FOR VALUES FROM (350) TO (500);
ANALYZE prt2_ad;

-- Partitioned join can't be applied because the default partition of prt1_ad
-- prt1_ad_p1 matches prt2_ad_p1 and prt2_ad_p3
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_ad t1 INNER JOIN prt2_ad t2 ON (t1.a = t2.b) WHERE t1.b = 0 ORDER BY t1.a, t2.b;

-- Change prt2_ad_p3 to the default partition
ALTER TABLE prt2_ad DETACH PARTITION prt2_ad_p3;
ALTER TABLE prt2_ad ATTACH PARTITION prt2_ad_p3 DEFAULT;
ANALYZE prt2_ad;

-- Partitioned join can't be applied because the default partition of prt1_ad
-- prt1_ad_p1 matches prt2_ad_p1 and prt2_ad_p3
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c FROM prt1_ad t1 INNER JOIN prt2_ad t2 ON (t1.a = t2.b) WHERE t1.b = 0 ORDER BY t1.a, t2.b;

DROP TABLE prt1_ad_p3;
ANALYZE prt1_ad;

DROP TABLE prt2_ad_p3;
ANALYZE prt2_ad;

CREATE TABLE prt3_ad (a int, b int, c varchar) PARTITION BY RANGE (a);
CREATE TABLE prt3_ad_p1 PARTITION OF prt3_ad FOR VALUES FROM (200) TO (300);
CREATE TABLE prt3_ad_p2 PARTITION OF prt3_ad FOR VALUES FROM (300) TO (400);
CREATE INDEX prt3_ad_a_idx on prt3_ad (a);
INSERT INTO prt3_ad SELECT i, i % 25, to_char(i, 'FM0000') FROM generate_series(200, 399) i;
ANALYZE prt3_ad;

-- 3-way join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.b, t2.c, t3.a, t3.c FROM prt1_ad t1 LEFT JOIN prt2_ad t2 ON (t1.a = t2.b) LEFT JOIN prt3_ad t3 ON (t1.a = t3.a) WHERE t1.b = 0 ORDER BY t1.a, t2.b, t3.a;
SELECT t1.a, t1.c, t2.b, t2.c, t3.a, t3.c FROM prt1_ad t1 LEFT JOIN prt2_ad t2 ON (t1.a = t2.b) LEFT JOIN prt3_ad t3 ON (t1.a = t3.a) WHERE t1.b = 0 ORDER BY t1.a, t2.b, t3.a;


-- Tests for list-partitioned tables
CREATE TABLE plt1_ad (a int, b int, c text) PARTITION BY LIST (c);
CREATE TABLE plt1_ad_p1 PARTITION OF plt1_ad FOR VALUES IN ('0001', '0003');
CREATE TABLE plt1_ad_p2 PARTITION OF plt1_ad FOR VALUES IN ('0004', '0006');
CREATE TABLE plt1_ad_p3 PARTITION OF plt1_ad FOR VALUES IN ('0008', '0009');
INSERT INTO plt1_ad SELECT i, i, to_char(i % 10, 'FM0000') FROM generate_series(1, 299) i WHERE i % 10 NOT IN (0, 2, 5, 7);
ANALYZE plt1_ad;

CREATE TABLE plt2_ad (a int, b int, c text) PARTITION BY LIST (c);
CREATE TABLE plt2_ad_p1 PARTITION OF plt2_ad FOR VALUES IN ('0002', '0003');
CREATE TABLE plt2_ad_p2 PARTITION OF plt2_ad FOR VALUES IN ('0004', '0006');
CREATE TABLE plt2_ad_p3 PARTITION OF plt2_ad FOR VALUES IN ('0007', '0009');
INSERT INTO plt2_ad SELECT i, i, to_char(i % 10, 'FM0000') FROM generate_series(1, 299) i WHERE i % 10 NOT IN (0, 1, 5, 8);
ANALYZE plt2_ad;

-- inner join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;

-- semi join
EXPLAIN (COSTS OFF)
SELECT t1.* FROM plt1_ad t1 WHERE EXISTS (SELECT 1 FROM plt2_ad t2 WHERE t1.a = t2.a AND t1.c = t2.c) AND t1.b < 10 ORDER BY t1.a;
SELECT t1.* FROM plt1_ad t1 WHERE EXISTS (SELECT 1 FROM plt2_ad t2 WHERE t1.a = t2.a AND t1.c = t2.c) AND t1.b < 10 ORDER BY t1.a;

-- left join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 LEFT JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 LEFT JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;

-- anti join
EXPLAIN (COSTS OFF)
SELECT t1.* FROM plt1_ad t1 WHERE NOT EXISTS (SELECT 1 FROM plt2_ad t2 WHERE t1.a = t2.a AND t1.c = t2.c) AND t1.b < 10 ORDER BY t1.a;
SELECT t1.* FROM plt1_ad t1 WHERE NOT EXISTS (SELECT 1 FROM plt2_ad t2 WHERE t1.a = t2.a AND t1.c = t2.c) AND t1.b < 10 ORDER BY t1.a;

-- full join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 FULL JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE coalesce(t1.b, 0) < 10 AND coalesce(t2.b, 0) < 10 ORDER BY t1.a, t2.a;
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 FULL JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE coalesce(t1.b, 0) < 10 AND coalesce(t2.b, 0) < 10 ORDER BY t1.a, t2.a;

-- Test cases where one side has an extra partition

-- Add an extra partition to plt2_ad
CREATE TABLE plt2_ad_extra PARTITION OF plt2_ad FOR VALUES IN ('0000');
INSERT INTO plt2_ad_extra VALUES (0, 0, '0000');
ANALYZE plt2_ad;

-- inner join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;

-- semi join
EXPLAIN (COSTS OFF)
SELECT t1.* FROM plt1_ad t1 WHERE EXISTS (SELECT 1 FROM plt2_ad t2 WHERE t1.a = t2.a AND t1.c = t2.c) AND t1.b < 10 ORDER BY t1.a;
SELECT t1.* FROM plt1_ad t1 WHERE EXISTS (SELECT 1 FROM plt2_ad t2 WHERE t1.a = t2.a AND t1.c = t2.c) AND t1.b < 10 ORDER BY t1.a;

-- left join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 LEFT JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 LEFT JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;

-- left join; currently we can't do partitioned join if there are no matched
-- partitions on the nullable side
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt2_ad t1 LEFT JOIN plt1_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;

-- anti join
EXPLAIN (COSTS OFF)
SELECT t1.* FROM plt1_ad t1 WHERE NOT EXISTS (SELECT 1 FROM plt2_ad t2 WHERE t1.a = t2.a AND t1.c = t2.c) AND t1.b < 10 ORDER BY t1.a;
SELECT t1.* FROM plt1_ad t1 WHERE NOT EXISTS (SELECT 1 FROM plt2_ad t2 WHERE t1.a = t2.a AND t1.c = t2.c) AND t1.b < 10 ORDER BY t1.a;

-- anti join; currently we can't do partitioned join if there are no matched
-- partitions on the nullable side
EXPLAIN (COSTS OFF)
SELECT t1.* FROM plt2_ad t1 WHERE NOT EXISTS (SELECT 1 FROM plt1_ad t2 WHERE t1.a = t2.a AND t1.c = t2.c) AND t1.b < 10 ORDER BY t1.a;

-- full join; currently we can't do partitioned join if there are no matched
-- partitions on the nullable side
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 FULL JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE coalesce(t1.b, 0) < 10 AND coalesce(t2.b, 0) < 10 ORDER BY t1.a, t2.a;

DROP TABLE plt2_ad_extra;

-- Test cases where a partition on one side matches multiple partitions on
-- the other side; we currently can't do partitioned join in those cases

-- Split plt2_ad_p2 into two partitions so that prt1_ad_p2 matches both
-- partitions
ALTER TABLE plt2_ad DETACH PARTITION plt2_ad_p2;
CREATE TABLE plt2_ad_p2_0004 PARTITION OF plt2_ad FOR VALUES IN ('0004');
CREATE TABLE plt2_ad_p2_0006 PARTITION OF plt2_ad FOR VALUES IN ('0006');
INSERT INTO plt2_ad SELECT i, i, to_char(i % 10, 'FM0000') FROM generate_series(1, 299) i WHERE i % 10 IN (4, 6);
ANALYZE plt2_ad;

-- inner join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;

-- semi join
EXPLAIN (COSTS OFF)
SELECT t1.* FROM plt1_ad t1 WHERE EXISTS (SELECT 1 FROM plt2_ad t2 WHERE t1.a = t2.a AND t1.c = t2.c) AND t1.b < 10 ORDER BY t1.a;

-- left join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 LEFT JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;

-- anti join
EXPLAIN (COSTS OFF)
SELECT t1.* FROM plt1_ad t1 WHERE NOT EXISTS (SELECT 1 FROM plt2_ad t2 WHERE t1.a = t2.a AND t1.c = t2.c) AND t1.b < 10 ORDER BY t1.a;

-- full join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 FULL JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE coalesce(t1.b, 0) < 10 AND coalesce(t2.b, 0) < 10 ORDER BY t1.a, t2.a;

DROP TABLE plt2_ad_p2_0004;
DROP TABLE plt2_ad_p2_0006;

-- Restore plt2_ad_p2
ALTER TABLE plt2_ad ATTACH PARTITION plt2_ad_p2 FOR VALUES IN ('0004', '0006');
ANALYZE plt2_ad;

-- Test NULL partitions

-- Change plt1_ad_p1 to the NULL partition
ALTER TABLE plt1_ad DETACH PARTITION plt1_ad_p1;
CREATE TABLE plt1_ad_p1_null PARTITION OF plt1_ad FOR VALUES IN (NULL, '0001', '0003');
INSERT INTO plt1_ad SELECT i, i, to_char(i % 10, 'FM0000') FROM generate_series(1, 299) i WHERE i % 10 IN (1, 3);
INSERT INTO plt1_ad VALUES (-1, -1, NULL);
ANALYZE plt1_ad;

-- Change plt2_ad_p3 to the NULL partition
ALTER TABLE plt2_ad DETACH PARTITION plt2_ad_p3;
CREATE TABLE plt2_ad_p3_null PARTITION OF plt2_ad FOR VALUES IN (NULL, '0007', '0009');
INSERT INTO plt2_ad SELECT i, i, to_char(i % 10, 'FM0000') FROM generate_series(1, 299) i WHERE i % 10 IN (7, 9);
INSERT INTO plt2_ad VALUES (-1, -1, NULL);
ANALYZE plt2_ad;

-- inner join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;

-- semi join
EXPLAIN (COSTS OFF)
SELECT t1.* FROM plt1_ad t1 WHERE EXISTS (SELECT 1 FROM plt2_ad t2 WHERE t1.a = t2.a AND t1.c = t2.c) AND t1.b < 10 ORDER BY t1.a;
SELECT t1.* FROM plt1_ad t1 WHERE EXISTS (SELECT 1 FROM plt2_ad t2 WHERE t1.a = t2.a AND t1.c = t2.c) AND t1.b < 10 ORDER BY t1.a;

-- left join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 LEFT JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 LEFT JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;

-- anti join
EXPLAIN (COSTS OFF)
SELECT t1.* FROM plt1_ad t1 WHERE NOT EXISTS (SELECT 1 FROM plt2_ad t2 WHERE t1.a = t2.a AND t1.c = t2.c) AND t1.b < 10 ORDER BY t1.a;
SELECT t1.* FROM plt1_ad t1 WHERE NOT EXISTS (SELECT 1 FROM plt2_ad t2 WHERE t1.a = t2.a AND t1.c = t2.c) AND t1.b < 10 ORDER BY t1.a;

-- full join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 FULL JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE coalesce(t1.b, 0) < 10 AND coalesce(t2.b, 0) < 10 ORDER BY t1.a, t2.a;
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 FULL JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE coalesce(t1.b, 0) < 10 AND coalesce(t2.b, 0) < 10 ORDER BY t1.a, t2.a;

DROP TABLE plt1_ad_p1_null;
DROP TABLE plt2_ad_p3_null;

-- Restore plt1_ad_p1
ALTER TABLE plt1_ad ATTACH PARTITION plt1_ad_p1 FOR VALUES IN ('0001', '0003');

-- Add to plt1_ad the extra NULL partition containing only NULL values as the
-- key values
CREATE TABLE plt1_ad_extra PARTITION OF plt1_ad FOR VALUES IN (NULL);
INSERT INTO plt1_ad VALUES (-1, -1, NULL);
ANALYZE plt1_ad;

-- Restore plt2_ad_p3
ALTER TABLE plt2_ad ATTACH PARTITION plt2_ad_p3 FOR VALUES IN ('0007', '0009');
ANALYZE plt2_ad;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;

-- Partitioned join can't be applied because there isn't any partition on the
-- plt2_ad side that matches the NULL partition of plt1_ad plt1_ad_extra
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 LEFT JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;

-- Partitioned join can't be applied because there isn't any partition on the
-- plt2_ad side that matches the NULL partition of plt1_ad plt1_ad_extra
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 FULL JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE coalesce(t1.b, 0) < 10 AND coalesce(t2.b, 0) < 10 ORDER BY t1.a, t2.a;

-- Add to plt2_ad the extra NULL partition containing only NULL values as the
-- key values
CREATE TABLE plt2_ad_extra PARTITION OF plt2_ad FOR VALUES IN (NULL);
INSERT INTO plt2_ad VALUES (-1, -1, NULL);
ANALYZE plt2_ad;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 LEFT JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 LEFT JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 FULL JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE coalesce(t1.b, 0) < 10 AND coalesce(t2.b, 0) < 10 ORDER BY t1.a, t2.a;
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 FULL JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE coalesce(t1.b, 0) < 10 AND coalesce(t2.b, 0) < 10 ORDER BY t1.a, t2.a;

DROP TABLE plt1_ad_extra;
DROP TABLE plt2_ad_extra;

-- Test default partitions

-- Change plt1_ad_p1 to the default partition
ALTER TABLE plt1_ad DETACH PARTITION plt1_ad_p1;
ALTER TABLE plt1_ad ATTACH PARTITION plt1_ad_p1 DEFAULT;
DROP TABLE plt1_ad_p3;
ANALYZE plt1_ad;

DROP TABLE plt2_ad_p3;
ANALYZE plt2_ad;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;

-- Change plt2_ad_p2 to contain '0005' in addition to '0004' and '0006' as
-- the key values
ALTER TABLE plt2_ad DETACH PARTITION plt2_ad_p2;
CREATE TABLE plt2_ad_p2_ext PARTITION OF plt2_ad FOR VALUES IN ('0004', '0005', '0006');
INSERT INTO plt2_ad SELECT i, i, to_char(i % 10, 'FM0000') FROM generate_series(1, 299) i WHERE i % 10 IN (4, 5, 6);
ANALYZE plt2_ad;

-- Partitioned join can't be applied because the default partition of plt1_ad
-- plt1_ad_p1 matches plt2_ad_p1 and plt2_ad_p2_ext
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;

-- Change plt2_ad_p2_ext to the default partition
ALTER TABLE plt2_ad DETACH PARTITION plt2_ad_p2_ext;
ALTER TABLE plt2_ad ATTACH PARTITION plt2_ad_p2_ext DEFAULT;
ANALYZE plt2_ad;

-- Partitioned join can't be applied because the default partition of plt1_ad
-- plt1_ad_p1 matches plt2_ad_p1 and plt2_ad_p2_ext
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;

DROP TABLE plt2_ad_p2_ext;

-- Restore plt2_ad_p2
ALTER TABLE plt2_ad ATTACH PARTITION plt2_ad_p2 FOR VALUES IN ('0004', '0006');
ANALYZE plt2_ad;

CREATE TABLE plt3_ad (a int, b int, c text) PARTITION BY LIST (c);
CREATE TABLE plt3_ad_p1 PARTITION OF plt3_ad FOR VALUES IN ('0004', '0006');
CREATE TABLE plt3_ad_p2 PARTITION OF plt3_ad FOR VALUES IN ('0007', '0009');
INSERT INTO plt3_ad SELECT i, i, to_char(i % 10, 'FM0000') FROM generate_series(1, 299) i WHERE i % 10 IN (4, 6, 7, 9);
ANALYZE plt3_ad;

-- 3-way join
EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c, t3.a, t3.c FROM plt1_ad t1 LEFT JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) LEFT JOIN plt3_ad t3 ON (t1.a = t3.a AND t1.c = t3.c) WHERE t1.b < 10 ORDER BY t1.a;
SELECT t1.a, t1.c, t2.a, t2.c, t3.a, t3.c FROM plt1_ad t1 LEFT JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) LEFT JOIN plt3_ad t3 ON (t1.a = t3.a AND t1.c = t3.c) WHERE t1.b < 10 ORDER BY t1.a;

-- Test cases where one side has the default partition while the other side
-- has the NULL partition

-- Change plt2_ad_p1 to the NULL partition
ALTER TABLE plt2_ad DETACH PARTITION plt2_ad_p1;
CREATE TABLE plt2_ad_p1_null PARTITION OF plt2_ad FOR VALUES IN (NULL, '0001', '0003');
INSERT INTO plt2_ad SELECT i, i, to_char(i % 10, 'FM0000') FROM generate_series(1, 299) i WHERE i % 10 IN (1, 3);
INSERT INTO plt2_ad VALUES (-1, -1, NULL);
ANALYZE plt2_ad;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;

-- Change the NULL partition to contain only NULL values as the key values
ALTER TABLE plt2_ad DETACH PARTITION plt2_ad_p1_null;
CREATE TABLE plt2_ad_p1_nullonly PARTITION OF plt2_ad FOR VALUES IN (NULL);
INSERT INTO plt2_ad VALUES (-1, -1, NULL);
ANALYZE plt2_ad;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;
SELECT t1.a, t1.c, t2.a, t2.c FROM plt1_ad t1 INNER JOIN plt2_ad t2 ON (t1.a = t2.a AND t1.c = t2.c) WHERE t1.b < 10 ORDER BY t1.a;

DROP TABLE plt2_ad_p1_nullonly;
DROP TABLE plt2_ad_p1_null;
DROP TABLE plt2_ad_p1;

DROP TABLE plt1_ad;
DROP TABLE plt2_ad;
DROP TABLE plt3_ad;


CREATE TABLE plt1_ad (a int, b int, c text) PARTITION BY LIST (c);
CREATE TABLE plt1_ad_p1 PARTITION OF plt1_ad FOR VALUES IN ('0000', '0001', '0002');
CREATE TABLE plt1_ad_p2 PARTITION OF plt1_ad FOR VALUES IN ('0003', '0004');
INSERT INTO plt1_ad SELECT i, i, to_char(i % 5, 'FM0000') FROM generate_series(0, 24) i;
ANALYZE plt1_ad;

CREATE TABLE plt2_ad (a int, b int, c text) PARTITION BY LIST (c);
CREATE TABLE plt2_ad_p1 PARTITION OF plt2_ad FOR VALUES IN ('0002');
CREATE TABLE plt2_ad_p2 PARTITION OF plt2_ad FOR VALUES IN ('0003', '0004');
INSERT INTO plt2_ad SELECT i, i, to_char(i % 5, 'FM0000') FROM generate_series(0, 24) i WHERE i % 5 IN (2, 3, 4);
ANALYZE plt2_ad;

CREATE TABLE plt3_ad (a int, b int, c text) PARTITION BY LIST (c);
CREATE TABLE plt3_ad_p1 PARTITION OF plt3_ad FOR VALUES IN ('0001');
CREATE TABLE plt3_ad_p2 PARTITION OF plt3_ad FOR VALUES IN ('0003', '0004');
INSERT INTO plt3_ad SELECT i, i, to_char(i % 5, 'FM0000') FROM generate_series(0, 24) i WHERE i % 5 IN (1, 3, 4);
ANALYZE plt3_ad;

EXPLAIN (COSTS OFF)
SELECT t1.a, t1.c, t2.a, t2.c, t3.a, t3.c FROM (plt1_ad t1 LEFT JOIN plt2_ad t2 ON (t1.c = t2.c)) FULL JOIN plt3_ad t3 ON (t1.c = t3.c) WHERE coalesce(t1.a, 0) % 5 != 3 AND coalesce(t1.a, 0) % 5 != 4 ORDER BY t1.c, t1.a, t2.a, t3.a;
SELECT t1.a, t1.c, t2.a, t2.c, t3.a, t3.c FROM (plt1_ad t1 LEFT JOIN plt2_ad t2 ON (t1.c = t2.c)) FULL JOIN plt3_ad t3 ON (t1.c = t3.c) WHERE coalesce(t1.a, 0) % 5 != 3 AND coalesce(t1.a, 0) % 5 != 4 ORDER BY t1.c, t1.a, t2.a, t3.a;

CREATE TABLE raw_data (a text);
INSERT INTO raw_data (a) VALUES ('Türkiye'),
								('TÜRKIYE'),
								('bıt'),
								('BIT'),
								('äbç'),
								('ÄBÇ'),
								('aaá'),
								('coté'),
								('Götz'),
								('ὀδυσσεύς'),
								('ὈΔΥΣΣΕΎΣ'),
								('を読み取り用'),
								('にオープンできませんでした');

CREATE TABLE alpha
	(a TEXT, b TEXT)
	PARTITION BY RANGE(a, b);
CREATE TABLE alpha_a PARTITION OF alpha FOR VALUES FROM ('a','v') TO ('c','q');
CREATE TABLE alpha_b PARTITION OF alpha FOR VALUES FROM ('c','q') TO ('d','f');
CREATE TABLE alpha_c PARTITION OF alpha FOR VALUES FROM ('d','f') TO ('p','m');
CREATE TABLE alpha_d PARTITION OF alpha FOR VALUES FROM ('p','m') TO ('z','z');
CREATE TABLE alpha_e PARTITION OF alpha FOR VALUES FROM ('z','z') TO ('√','Σ');
CREATE TABLE alpha_f PARTITION OF alpha FOR VALUES FROM ('√','Σ') TO ('き','ま');
CREATE TABLE alpha_default PARTITION OF alpha DEFAULT;

CREATE TABLE beta
	(a TEXT, b TEXT)
	PARTITION BY RANGE(a, b);
CREATE TABLE beta_a PARTITION OF beta FOR VALUES FROM ('a','z') TO ('d','z');
CREATE TABLE beta_b PARTITION OF beta FOR VALUES FROM ('d','z') TO ('g','z');
CREATE TABLE beta_c PARTITION OF beta FOR VALUES FROM ('g','z') TO ('k','z');
CREATE TABLE beta_d PARTITION OF beta FOR VALUES FROM ('k','z') TO ('o','z');
CREATE TABLE beta_e PARTITION OF beta FOR VALUES FROM ('o','z') TO ('t','z');
CREATE TABLE beta_f PARTITION OF beta FOR VALUES FROM ('t','z') TO ('Δ','υ');
CREATE TABLE beta_g PARTITION OF beta FOR VALUES FROM ('Δ','υ') TO ('ὀ','√');
CREATE TABLE beta_h PARTITION OF beta FOR VALUES FROM ('ὀ','√') TO ('ん', '用');
CREATE TABLE beta_default PARTITION OF beta DEFAULT;

INSERT INTO alpha (SELECT a, a FROM raw_data);
INSERT INTO beta (SELECT a, a FROM raw_data);

ANALYZE alpha;
ANALYZE beta;

-- inner join
EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IN ('äbç', 'ὀδυσσεύς');
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IN ('äbç', 'ὀδυσσεύς');

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'äbç';
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'äbç';

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'ὀδυσσεύς';
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'ὀδυσσεύς';

-- semi join
EXPLAIN (COSTS OFF)
SELECT t1.a FROM alpha t1 WHERE EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a IN ('äbç', 'ὀδυσσεύς');
SELECT t1.a FROM alpha t1 WHERE EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a IN ('äbç', 'ὀδυσσεύς');

EXPLAIN (COSTS OFF)
SELECT t1.a FROM alpha t1 WHERE EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a = 'äbç';
SELECT t1.a FROM alpha t1 WHERE EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a = 'äbç';

EXPLAIN (COSTS OFF)
SELECT t1.a FROM alpha t1 WHERE EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a = 'ὀδυσσεύς';
SELECT t1.a FROM alpha t1 WHERE EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a = 'ὀδυσσεύς';

-- left join
EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 LEFT JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IN ('äbç', 'ὀδυσσεύς');
SELECT t1.a, t2.a FROM alpha t1 LEFT JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IN ('äbç', 'ὀδυσσεύς');

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 LEFT JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'äbç';
SELECT t1.a, t2.a FROM alpha t1 LEFT JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'äbç';

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 LEFT JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'ὀδυσσεύς';
SELECT t1.a, t2.a FROM alpha t1 LEFT JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'ὀδυσσεύς';

-- anti join
EXPLAIN (COSTS OFF)
SELECT t1.a FROM alpha t1 WHERE NOT EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a IN ('äbç', 'ὀδυσσεύς');
SELECT t1.a FROM alpha t1 WHERE NOT EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a IN ('äbç', 'ὀδυσσεύς');

EXPLAIN (COSTS OFF)
SELECT t1.a FROM alpha t1 WHERE NOT EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a = 'äbç';
SELECT t1.a FROM alpha t1 WHERE NOT EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a = 'äbç';

EXPLAIN (COSTS OFF)
SELECT t1.a FROM alpha t1 WHERE NOT EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a = 'ὀδυσσεύς';
SELECT t1.a FROM alpha t1 WHERE NOT EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a = 'ὀδυσσεύς';

-- full join
EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 FULL JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IN ('äbç', 'ὀδυσσεύς');
SELECT t1.a, t2.a FROM alpha t1 FULL JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IN ('äbç', 'ὀδυσσεύς');

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 FULL JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'äbç';
SELECT t1.a, t2.a FROM alpha t1 FULL JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'äbç';

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 FULL JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'ὀδυσσεύς';
SELECT t1.a, t2.a FROM alpha t1 FULL JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'ὀδυσσεύς';

-- Try again, this time with list partitioning
DROP TABLE alpha CASCADE;
DROP TABLE beta CASCADE;

CREATE TABLE alpha (a TEXT) PARTITION BY LIST(a);
CREATE TABLE alpha_a PARTITION OF alpha FOR VALUES IN ('Türkiye', 'TÜRKIYE');
CREATE TABLE alpha_b PARTITION OF alpha FOR VALUES IN ('bıt', 'BIT');
CREATE TABLE alpha_c PARTITION OF alpha FOR VALUES IN ('äbç', 'ÄBÇ');
CREATE TABLE alpha_d PARTITION OF alpha FOR VALUES IN ('aaá', 'coté', 'Götz');
CREATE TABLE alpha_e PARTITION OF alpha FOR VALUES IN ('ὀδυσσεύς', 'ὈΔΥΣΣΕΎΣ');
CREATE TABLE alpha_f PARTITION OF alpha FOR VALUES IN ('を読み取り用', 'にオープンできませんでした', NULL);
CREATE TABLE alpha_default PARTITION OF alpha DEFAULT;

CREATE TABLE beta (a TEXT) PARTITION BY LIST(a);
CREATE TABLE beta_a PARTITION OF beta FOR VALUES IN ('Türkiye', 'coté', 'ὈΔΥΣΣΕΎΣ');
CREATE TABLE beta_b PARTITION OF beta FOR VALUES IN ('bıt', 'TÜRKIYE');
CREATE TABLE beta_c PARTITION OF beta FOR VALUES IN ('äbç', 'を読み取り用', 'にオープンできませんでした');
CREATE TABLE beta_d PARTITION OF beta FOR VALUES IN ('aaá', 'Götz', 'BIT', 'ὀδυσσεύς', 'ÄBÇ', NULL);
CREATE TABLE beta_default PARTITION OF beta DEFAULT;

INSERT INTO alpha (SELECT a FROM raw_data);
INSERT INTO beta (SELECT a FROM raw_data);
INSERT INTO alpha VALUES (null);
INSERT INTO beta VALUES (null);
INSERT INTO alpha VALUES ('grumble');
INSERT INTO beta VALUES ('grumble');

ANALYZE alpha;
ANALYZE beta;

-- inner join
EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IN ('äbç', 'ὀδυσσεύς');
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IN ('äbç', 'ὀδυσσεύς');

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'äbç';
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'äbç';

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'ὀδυσσεύς';
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'ὀδυσσεύς';

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IS NULL;
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IS NULL;

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a IS NOT DISTINCT FROM t2.a) WHERE t1.a IS NULL;
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a IS NOT DISTINCT FROM t2.a) WHERE t1.a IS NULL;

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IS NULL;
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IS NULL;

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a IS NOT DISTINCT FROM t2.a) WHERE t1.a IS NULL;
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a IS NOT DISTINCT FROM t2.a) WHERE t1.a IS NULL;

-- semi join
EXPLAIN (COSTS OFF)
SELECT t1.a FROM alpha t1 WHERE EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a IN ('äbç', 'ὀδυσσεύς');
SELECT t1.a FROM alpha t1 WHERE EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a IN ('äbç', 'ὀδυσσεύς');

EXPLAIN (COSTS OFF)
SELECT t1.a FROM alpha t1 WHERE EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a = 'äbç';
SELECT t1.a FROM alpha t1 WHERE EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a = 'äbç';

EXPLAIN (COSTS OFF)
SELECT t1.a FROM alpha t1 WHERE EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a = 'ὀδυσσεύς';
SELECT t1.a FROM alpha t1 WHERE EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a = 'ὀδυσσεύς';

EXPLAIN (COSTS OFF)
SELECT t1.a FROM alpha t1 WHERE EXISTS (SELECT 1 FROM beta t2 WHERE t1.a IS NOT DISTINCT FROM t2.a) AND t1.a IS NULL;
SELECT t1.a FROM alpha t1 WHERE EXISTS (SELECT 1 FROM beta t2 WHERE t1.a IS NOT DISTINCT FROM t2.a) AND t1.a IS NULL;

EXPLAIN (COSTS OFF)
SELECT t1.a FROM alpha t1 WHERE EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a IS NULL;
SELECT t1.a FROM alpha t1 WHERE EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a IS NULL;

-- left join
EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 LEFT JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IN ('äbç', 'ὀδυσσεύς');
SELECT t1.a, t2.a FROM alpha t1 LEFT JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IN ('äbç', 'ὀδυσσεύς');

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 LEFT JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'äbç';
SELECT t1.a, t2.a FROM alpha t1 LEFT JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'äbç';

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 LEFT JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'ὀδυσσεύς';
SELECT t1.a, t2.a FROM alpha t1 LEFT JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'ὀδυσσεύς';

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 LEFT JOIN beta t2 ON (t1.a IS NOT DISTINCT FROM t2.a) WHERE t1.a IS NULL;
SELECT t1.a, t2.a FROM alpha t1 LEFT JOIN beta t2 ON (t1.a IS NOT DISTINCT FROM t2.a) WHERE t1.a IS NULL;

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 LEFT JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IS NULL;
SELECT t1.a, t2.a FROM alpha t1 LEFT JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IS NULL;

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 LEFT JOIN beta t2 ON (t1.a IS NOT DISTINCT FROM t2.a) WHERE t1.a IS NOT DISTINCT FROM t2.a;
SELECT t1.a, t2.a FROM alpha t1 LEFT JOIN beta t2 ON (t1.a IS NOT DISTINCT FROM t2.a) WHERE t1.a IS NOT DISTINCT FROM t2.a;

-- anti join
EXPLAIN (COSTS OFF)
SELECT t1.a FROM alpha t1 WHERE NOT EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a IN ('äbç', 'ὀδυσσεύς');
SELECT t1.a FROM alpha t1 WHERE NOT EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a IN ('äbç', 'ὀδυσσεύς');

EXPLAIN (COSTS OFF)
SELECT t1.a FROM alpha t1 WHERE NOT EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a = 'äbç';
SELECT t1.a FROM alpha t1 WHERE NOT EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a = 'äbç';

EXPLAIN (COSTS OFF)
SELECT t1.a FROM alpha t1 WHERE NOT EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a = 'ὀδυσσεύς';
SELECT t1.a FROM alpha t1 WHERE NOT EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a = 'ὀδυσσεύς';

EXPLAIN (COSTS OFF)
SELECT t1.a FROM alpha t1 WHERE NOT EXISTS (SELECT 1 FROM beta t2 WHERE t1.a IS NOT DISTINCT FROM t2.a) AND t1.a IS NULL;
SELECT t1.a FROM alpha t1 WHERE NOT EXISTS (SELECT 1 FROM beta t2 WHERE t1.a IS NOT DISTINCT FROM t2.a) AND t1.a IS NULL;

EXPLAIN (COSTS OFF)
SELECT t1.a FROM alpha t1 WHERE NOT EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a IS NULL;
SELECT t1.a FROM alpha t1 WHERE NOT EXISTS (SELECT 1 FROM beta t2 WHERE t1.a = t2.a) AND t1.a IS NULL;

-- full join
EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 FULL JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IN ('äbç', 'ὀδυσσεύς');
SELECT t1.a, t2.a FROM alpha t1 FULL JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IN ('äbç', 'ὀδυσσεύς');

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 FULL JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'äbç';
SELECT t1.a, t2.a FROM alpha t1 FULL JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'äbç';

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 FULL JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'ὀδυσσεύς';
SELECT t1.a, t2.a FROM alpha t1 FULL JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a = 'ὀδυσσεύς';

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 FULL JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IS NULL;
SELECT t1.a, t2.a FROM alpha t1 FULL JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IS NULL;
