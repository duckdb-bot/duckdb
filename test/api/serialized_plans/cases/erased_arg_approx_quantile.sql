CREATE TABLE t1 (g INT, v INTEGER, d DECIMAL(10,3));
INSERT INTO t1 VALUES (1, 1, 1.5), (1, 2, 2.5), (1, 3, 3.5), (2, 10, 10.5);
-- the plan was written when approx_quantile over a DECIMAL cast its input to the integer type of the DECIMAL's physical type and returned that type, so its last column is approx_quantile(d::BIGINT, 0.5) in the current syntax
SELECT g, approx_quantile(v, 0.5), approx_quantile(v, [0.25, 0.75]), approx_quantile(d::BIGINT, 0.5) FROM t1 GROUP BY g ORDER BY g;
