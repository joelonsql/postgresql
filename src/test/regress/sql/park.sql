--
-- Test backend parking functionality
--

-- Test pg_park function exists and can be called
SELECT pg_park();

-- Test that we can still execute queries after pg_park
SELECT 1 as test_after_park;

-- Test parking with different configurations
-- (These would need enable_parking to be on for full testing)