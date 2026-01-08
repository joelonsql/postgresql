CREATE ROLE acl_bench_grantee;
CREATE TABLE acl_bench_tbl (id int);
CREATE VIEW acl_bench_view AS SELECT 1 AS x;
CREATE SEQUENCE acl_bench_seq;
CREATE FUNCTION acl_bench_fn() RETURNS int LANGUAGE sql AS 'SELECT 1';
CREATE TYPE acl_bench_type AS (x int, y int);
CREATE SCHEMA acl_bench_schema;
SELECT lo_create(12345);
