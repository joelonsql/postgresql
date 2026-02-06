/*-------------------------------------------------------------------------
 *
 * conn_scale_bench.c
 *	  Benchmark connection establishment throughput vs idle connections.
 *
 * This program measures how connection establishment throughput degrades
 * as the number of existing idle connections increases.
 *
 * Copyright (c) 2024, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "libpq-fe.h"

/* Default values */
#define DEFAULT_ITERATIONS	1000
#define DEFAULT_MAX_IDLE	100
#define DEFAULT_STEP		1
#define DEFAULT_HOST		"localhost"
#define DEFAULT_DBNAME		"postgres"

static void
usage(const char *progname)
{
	printf("Usage: %s [OPTIONS]\n\n", progname);
	printf("Options:\n");
	printf("  -d DATABASE     Database name (default: %s)\n", DEFAULT_DBNAME);
	printf("  -h HOST         Host (default: %s)\n", DEFAULT_HOST);
	printf("  -p PORT         Port (default: 5432)\n");
	printf("  -U USER         Username\n");
	printf("  -n ITERATIONS   Connections per measurement (default: %d)\n", DEFAULT_ITERATIONS);
	printf("  -m MAX_IDLE     Maximum idle connections to test (default: %d)\n", DEFAULT_MAX_IDLE);
	printf("  -s STEP         Increment idle connections by this amount (default: %d)\n", DEFAULT_STEP);
	printf("  --help          Show this help\n");
}

static double
get_time_sec(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (double) tv.tv_sec + (double) tv.tv_usec / 1000000.0;
}

static PGconn *
connect_db(const char *conninfo)
{
	PGconn	   *conn;

	conn = PQconnectdb(conninfo);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, "Connection failed: %s\n", PQerrorMessage(conn));
		PQfinish(conn);
		return NULL;
	}
	return conn;
}

int
main(int argc, char *argv[])
{
	const char *dbname = DEFAULT_DBNAME;
	const char *host = DEFAULT_HOST;
	const char *port = NULL;
	const char *user = NULL;
	int			iterations = DEFAULT_ITERATIONS;
	int			max_idle = DEFAULT_MAX_IDLE;
	int			step = DEFAULT_STEP;

	PGconn	  **idle_conns = NULL;
	char		conninfo[1024];
	int			opt;
	int			n_idle;
	int			i;

	static struct option long_options[] = {
		{"help", no_argument, NULL, '?'},
		{NULL, 0, NULL, 0}
	};

	while ((opt = getopt_long(argc, argv, "d:h:p:U:n:m:s:", long_options, NULL)) != -1)
	{
		switch (opt)
		{
			case 'd':
				dbname = optarg;
				break;
			case 'h':
				host = optarg;
				break;
			case 'p':
				port = optarg;
				break;
			case 'U':
				user = optarg;
				break;
			case 'n':
				iterations = atoi(optarg);
				if (iterations <= 0)
				{
					fprintf(stderr, "Invalid iterations: %s\n", optarg);
					exit(1);
				}
				break;
			case 'm':
				max_idle = atoi(optarg);
				if (max_idle < 0)
				{
					fprintf(stderr, "Invalid max_idle: %s\n", optarg);
					exit(1);
				}
				break;
			case 's':
				step = atoi(optarg);
				if (step <= 0)
				{
					fprintf(stderr, "Invalid step: %s\n", optarg);
					exit(1);
				}
				break;
			case '?':
			default:
				usage(argv[0]);
				exit(opt == '?' ? 0 : 1);
		}
	}

	/* Raise fd limit to accommodate many idle connections */
	{
		struct rlimit rl;

		if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
		{
			rl.rlim_cur = rl.rlim_max;
			setrlimit(RLIMIT_NOFILE, &rl);
		}
	}

	/* Build connection string */
	snprintf(conninfo, sizeof(conninfo), "host=%s dbname=%s%s%s%s%s",
			 host, dbname,
			 port ? " port=" : "", port ? port : "",
			 user ? " user=" : "", user ? user : "");

	/* Allocate array for idle connections */
	idle_conns = (PGconn **) calloc(max_idle + 1, sizeof(PGconn *));
	if (idle_conns == NULL)
	{
		fprintf(stderr, "Out of memory\n");
		exit(1);
	}

	/* Print CSV header */
	printf("idle_connections,conns_per_second,total_time_sec\n");

	/* Main benchmark loop */
	for (n_idle = 0; n_idle <= max_idle; n_idle += step)
	{
		double		start_time;
		double		end_time;
		double		elapsed;
		double		conns_per_sec;

		/* Measure connection throughput */
		start_time = get_time_sec();

		for (i = 0; i < iterations; i++)
		{
			PGconn	   *conn;
			PGresult   *res;

			conn = connect_db(conninfo);
			if (conn == NULL)
			{
				fprintf(stderr, "Connection failed at iteration %d with %d idle connections\n",
						i, n_idle);
				goto cleanup;
			}

			res = PQexec(conn, "SELECT 1");
			if (PQresultStatus(res) != PGRES_TUPLES_OK)
			{
				fprintf(stderr, "Query failed: %s\n", PQerrorMessage(conn));
				PQclear(res);
				PQfinish(conn);
				goto cleanup;
			}
			PQclear(res);
			PQfinish(conn);
		}

		end_time = get_time_sec();
		elapsed = end_time - start_time;
		conns_per_sec = (double) iterations / elapsed;

		printf("%d,%.2f,%.3f\n", n_idle, conns_per_sec, elapsed);
		fflush(stdout);

		/* Add idle connections up to next measurement point */
		if (n_idle < max_idle)
		{
			int			target = n_idle + step;

			if (target > max_idle)
				target = max_idle;

			for (i = n_idle; i < target; i++)
			{
				idle_conns[i] = connect_db(conninfo);
				if (idle_conns[i] == NULL)
				{
					fprintf(stderr, "Failed to create idle connection %d\n", i);
					max_idle = i;	/* Stop at this point */
					break;
				}
			}
		}
	}

cleanup:
	/* Close all idle connections */
	for (i = 0; i < max_idle; i++)
	{
		if (idle_conns[i] != NULL)
			PQfinish(idle_conns[i]);
	}
	free(idle_conns);

	return 0;
}
