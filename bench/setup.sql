-- Benchmark fixtures.  @rows is set by run.sh before this file runs.
--
--   bench.t        @rows rows        batch paths (query, query_array)
--   bench.t_small  @rows / 10 rows   one Python call per row
--   bench.t_tiny   @rows / 100 rows  one internal SQL statement per row

DROP SCHEMA IF EXISTS bench;
CREATE SCHEMA bench;

CREATE TABLE bench.t (id INT NOT NULL PRIMARY KEY, x DOUBLE NOT NULL,
                      k BIGINT NOT NULL, s VARCHAR(32) NOT NULL);
CREATE TABLE bench.t_small LIKE bench.t;
CREATE TABLE bench.t_tiny  LIKE bench.t;

SET @rows_small= GREATEST(@rows DIV 10, 1);
SET @rows_tiny=  GREATEST(@rows DIV 100, 1);

-- A recursive CTE generates the rows, so the Sequence engine is not needed
SET SESSION max_recursive_iterations= @rows + 10;
INSERT INTO bench.t
  WITH RECURSIVE gen (seq) AS
    (SELECT 1 UNION ALL SELECT seq + 1 FROM gen WHERE seq < @rows)
  SELECT seq, seq * 0.25, seq * 1000, CONCAT('row', seq) FROM gen;
EXECUTE IMMEDIATE CONCAT(
  'INSERT INTO bench.t_small SELECT * FROM bench.t WHERE id <= ', @rows_small);
EXECUTE IMMEDIATE CONCAT(
  'INSERT INTO bench.t_tiny SELECT * FROM bench.t WHERE id <= ', @rows_tiny);

CREATE TABLE bench.results (
  seq INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  name VARCHAR(64) NOT NULL, run INT NOT NULL,
  seconds DOUBLE NOT NULL, row_count BIGINT NOT NULL);

SELECT py_replace_function('bench_double', 'DOUBLE', 'x', 'result = x * 2.0');
SELECT py_replace_function('bench_query', 'INT', NULL, '
import inhost
result = len(inhost.sql.query("SELECT id, x, k FROM bench.t"))
');
SELECT py_replace_function('bench_query_str', 'INT', NULL, '
import inhost
result = len(inhost.sql.query("SELECT id, s FROM bench.t"))
');
SELECT py_replace_function('bench_query_array', 'DOUBLE', NULL, '
import inhost
result = float(inhost.sql.query_array("SELECT id, x, k FROM bench.t")["x"].sum())
');
SELECT py_replace_function('bench_point_query', 'INT', 'i', '
import inhost
result = inhost.sql.query("SELECT k FROM bench.t_tiny WHERE id = %s", (i,))[0][0]
');
