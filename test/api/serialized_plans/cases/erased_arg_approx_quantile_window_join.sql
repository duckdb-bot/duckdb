CREATE TABLE t1 (g INT, v INTEGER, d DECIMAL(10,3));
INSERT INTO t1 VALUES (1,1,1.2),(1,2,2.5),(1,3,3.8),(1,4,2.5),(2,10,10.5),(2,11,NULL),(3,20,NULL),(3,21,NULL);
SELECT w.g,w.v,w.q+1,w.c,x.v FROM (SELECT g,v,CAST(approx_quantile(d, 0.5) OVER (PARTITION BY g) AS BIGINT) q,count(*) OVER (PARTITION BY g) c FROM t1) w LEFT JOIN t1 x ON x.v=w.q WHERE w.q>2 OR w.q IS NULL ORDER BY w.q NULLS LAST,w.g,w.v,x.v;
