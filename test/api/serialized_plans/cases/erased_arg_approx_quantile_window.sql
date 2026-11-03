CREATE TABLE t1 (g INT, v INTEGER, d DECIMAL(10,3));
INSERT INTO t1 VALUES (1, 1, 1.5), (1, 2, 2.5), (1, 3, 3.5), (2, 10, 10.5);
SELECT g, v, d, CAST(approx_quantile(d, 0.5) OVER (PARTITION BY g) AS BIGINT) FROM t1 ORDER BY g, v;
