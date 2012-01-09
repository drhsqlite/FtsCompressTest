#include <assert.h>
#include <err.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>

#include "sqlite3.h"

#define DBPATH "./test.db"

static void
usage(void)
{
	printf("testfts -w\n");
	printf("testfts -c\n");
	printf("testfts -s <query>\n");
	exit(EXIT_FAILURE);
}

/*
 * zip --
 *  User defined Sqlite function to compress the FTS table
 */
static void
zip(sqlite3_context *pctx, int nval, sqlite3_value **apval)
{	
	int nin;
	long int nout;
	const unsigned char * inbuf;
	unsigned char *outbuf;

	assert(nval == 1);
	nin = sqlite3_value_bytes(apval[0]);
	inbuf = (const unsigned char *) sqlite3_value_blob(apval[0]);
	nout = nin + 13 + (nin + 999) / 1000;
	outbuf = malloc(nout);
	assert(nout);
	compress(outbuf, (unsigned long *) &nout, inbuf, nin);
	sqlite3_result_blob(pctx, outbuf, nout, free);
}

/*
 * unzip --
 *  User defined Sqlite function to uncompress the FTS table.
 */
static void
unzip(sqlite3_context *pctx, int nval, sqlite3_value **apval)
{
	unsigned int rc;
	unsigned char *outbuf;
	z_stream stream;

	assert(nval == 1);
	stream.next_in = __UNCONST(sqlite3_value_blob(apval[0]));
	stream.avail_in = sqlite3_value_bytes(apval[0]);
	stream.avail_out = stream.avail_in * 2 + 100;
	stream.next_out = outbuf = malloc(stream.avail_out);
	assert(outbuf);
	stream.zalloc = NULL;
	stream.zfree = NULL;
	inflateInit(&stream);

	if (inflateInit(&stream) != Z_OK) {
		free(outbuf);
		return;
	}

	while ((rc = inflate(&stream, Z_SYNC_FLUSH)) != Z_STREAM_END) {
		if (rc != Z_OK ||
		    (stream.avail_out != 0 && stream.avail_in == 0)) {
			free(outbuf);
			return;
		}
		outbuf = realloc(outbuf, stream.total_out * 2);
		assert(outbuf);
		stream.next_out = outbuf + stream.total_out;
		stream.avail_out = stream.total_out;
	}
	if (inflateEnd(&stream) != Z_OK) {
		free(outbuf);
		return;
	}
	outbuf = realloc(outbuf, stream.total_out);
	assert(outbuf);
	sqlite3_result_blob(pctx, outbuf, stream.total_out, free);
}

sqlite3 *
init_db()
{
	sqlite3 *db;
	int rc = 0;

	sqlite3_initialize();
	rc = sqlite3_open_v2(DBPATH, &db,
		SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
	
	if (rc != SQLITE_OK) {
		warnx("%s", sqlite3_errmsg(db));
		sqlite3_shutdown();
		exit(EXIT_FAILURE);
	}
	sqlite3_extended_result_codes(db, 1);


	/* Register the zip and unzip functions for FTS compression */
	rc = sqlite3_create_function(db, "zip", 1, SQLITE_ANY, NULL, 
                             zip, NULL, NULL);
	if (rc != SQLITE_OK) {
		sqlite3_close(db);
		sqlite3_shutdown();
		errx(EXIT_FAILURE, "Unable to register function: compress");
	}

	rc = sqlite3_create_function(db, "unzip", 1, SQLITE_ANY, NULL, 
		                         unzip, NULL, NULL);
	if (rc != SQLITE_OK) {
		sqlite3_close(db);
		sqlite3_shutdown();
		errx(EXIT_FAILURE, "Unable to register function: uncompress");
	}
	return db;

}

void
makedb(int compress)
{
	ssize_t nchars;
	char *line = 0;
	size_t len = 0;
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	char *query;
	int rc = 0;
	int i = 0;
	int idx = 0;
	char *errmsg = NULL;

	FILE *file = fopen("./sample.txt", "r");
	if (file == NULL)
		err(EXIT_FAILURE, "fopen failed");

	remove(DBPATH);
	db = init_db();
	if (compress)
		query = "CREATE VIRTUAL TABLE fts USING FTS4(data, i, tokenize=porter, "
			"compress=zip, uncompress=unzip)";
	else
		query = "CREATE VIRTUAL TABLE fts USING FTS4(data, i, tokenize=porter)";

	rc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		warnx("sqlite3_prepare failed while creating db");
		sqlite3_close(db);
		fclose(file);
		sqlite3_free(query);
		exit(EXIT_FAILURE);
	}
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	
	sqlite3_exec(db, "BEGIN", NULL, NULL, &errmsg);
	if (errmsg) {
		warnx("%s", errmsg);
		fclose(file);
		sqlite3_close(db);
		free(errmsg);
		exit(EXIT_FAILURE);
	}
	
	
	while ((nchars = getline(&line, &len, file)) != -1) {
		query = "INSERT INTO fts VALUES(:line, :i)";
		rc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
		if (rc != SQLITE_OK) {
			warnx("sqlite3_prepare failed while inserting data");
			warnx("%s", sqlite3_errmsg(db));
			sqlite3_close(db);
			fclose(file);
			exit(EXIT_FAILURE);
		}
	
		idx = sqlite3_bind_parameter_index(stmt, ":line");
		rc = sqlite3_bind_text(stmt, idx, line, -1, NULL);
		if (rc != SQLITE_OK) {
			warnx("%s", sqlite3_errmsg(db));
			sqlite3_finalize(stmt);
			sqlite3_close(db);
			fclose(file);
			exit(EXIT_FAILURE);
		}
		idx = sqlite3_bind_parameter_index(stmt, ":i");
		rc = sqlite3_bind_int(stmt, idx, i);
		if (rc != SQLITE_OK) {
			warnx("%s", sqlite3_errmsg(db));
			sqlite3_finalize(stmt);
			sqlite3_close(db);
			fclose(file);
			exit(EXIT_FAILURE);
		}
		
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	
	sqlite3_exec(db, "END", NULL, NULL, &errmsg);
	if (errmsg) {
		warnx("%s", errmsg);
		free(errmsg);
	}
	fclose(file);
	sqlite3_close(db);
}

void
query_db(char *query)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	int rc = 0;
	char *sql = NULL;

	db = init_db();	
	sql = sqlite3_mprintf("SELECT snippet(fts) FROM fts WHERE fts MATCH %Q AND "
			"i = 0 LIMIT 10", query);
	if (sql == NULL) {
		sqlite3_close(db);
		errx(EXIT_FAILURE, "sqlite3_mprintf failed");
	}
	
	rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		warnx("sqlite3_prepare failed while searching");
		warnx("%S", sqlite3_errmsg(db));
		sqlite3_close(db);
		sqlite3_free(sql);
		exit(EXIT_FAILURE);
	}
	
	sqlite3_free(sql);
	
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		printf("%s\n", sqlite3_column_text(stmt, 0));
	}
	
	sqlite3_finalize(stmt);
	sqlite3_close(db);
}

int
main(int argc, char **argv)
{
	int ch;
	char *query = NULL;
	while ((ch = getopt(argc, argv, "cs:w")) != -1) {
		switch (ch) {
		case 'c':
			makedb(1);
			break;
		case 's':
			query = optarg;
			break;
		case 'w':
			makedb(0);
			break;
		case '?':
		default:
			usage();
		}
	}
	
	argc -= optind;
	argv += optind;
	
	if (query) {
		query_db(query);
	}
		
	return 0;
}
