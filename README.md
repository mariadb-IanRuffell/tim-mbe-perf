# tim-mbe-perf

Performance tooling and a MySQL/MariaDB backend review for **wlib-mbe-sql**, the library that
emulates HP/Tandem Enscribe files on top of a SQL database (used by the TIM EIR programs).

The project contains:

- **`mbeperf`**, a 32-bit benchmark that replays the Enscribe access patterns of the EIR programs
  against the emulator and reports throughput and per-pattern latency.
- The **`wlib-mbe-sql`** library sources (the MySQL backend only).
- A reviewed and optimised version of the backend, **`my.c`**.

## Layout

| Path | Contents |
|------|----------|
| `my.c` | Optimised MySQL backend (see [Backend changes](#backend-changes)) |
| `my.c.orig` | The backend as received, for diffing |
| `sql-be.h` | The backend interface (`wmbe_sql_be_ifc` vtable) |
| `mbe-ini.md` | Format of the `.mbe.ini` files that map an Enscribe file to a table |
| `wlib-mbe-sql-share/` | Buildable source tree: `src/`, `include/`, `perf/`, `Dockerfile`, `CMakeLists.txt`, `vendor/*.ini` |

`wlib-mbe-sql-share/src/my.c` is the backend as shipped in the source share. The optimised
version is the top-level `my.c`; copy it over `src/my.c` to build with it.

## Not in this repository

- `mbe-perf.tar`, a prebuilt Docker image (500 MB, over GitHub's file size limit).
- `wlib-mbe-sql-share/vendor/lib/`: the prebuilt, proprietary wlib runtime libraries
  (`libwlib.so`, `libwlib-grpc.so`, ...). The Dockerfile and CMake build need them. Put them in
  `wlib-mbe-sql-share/vendor/lib/` before building.

## Running the prebuilt image

The image is 32-bit x86 (Rocky Linux 9). On Apple Silicon it runs under emulation, so expect
higher and noisier latencies than on native x86.

```bash
docker load -i mbe-perf.tar
docker run -d --name mbe-perf --platform linux/amd64 -it localhost/mbe-perf:latest
docker run -d --name mbe-perf-db --network container:mbe-perf \
  -e MARIADB_ROOT_PASSWORD=root -e MARIADB_DATABASE=wlib_test mariadb:11.4
docker exec mbe-perf bash -c 'cd /app && ./mbeperf -c mbeperf.ini -d 10'
```

`--network container:mbe-perf` puts MariaDB in the benchmark container's network namespace, so
the default URL `mysql://root:root@127.0.0.1/wlib_test` works unchanged.

Clean up:

```bash
docker rm -f mbe-perf-db mbe-perf
```

## Building

With the vendor libraries in place:

```bash
cd wlib-mbe-sql-share
docker build --platform linux/amd64 -t wlib-mbe-sql .
docker run --rm --platform linux/amd64 --network <net> wlib-mbe-sql \
  mbeperf -f mbeperf-my.mbe.ini --url mysql://user:pw@host/db
```

Or natively (needs 32-bit glib2, mariadb-connector-c, hiredis and librdkafka development
packages, plus `libwlib` and `libwlib-grpc`):

```bash
mkdir build && cd build
cmake .. -DWLIB_LIBRARY=/path/to/libwlib.so -DWLIB_GRPC_LIBRARY=/path/to/libwlib-grpc.so
make
```

This produces `libwlib-mbe-sql.so` and `mbeperf`. See the header of `CMakeLists.txt` for details,
including linking against a static `libwlib`.

## Using mbeperf

`mbeperf` creates and fills the test table, runs the load, prints a latency report and drops the
table. Every client is a separate process with its own database connection.

```bash
mbeperf -c mbeperf.ini                      # settings from a config file
mbeperf -f mbeperf-my.mbe.ini -R 2000 -j 4 -d 30   # or on the command line
```

Command-line options override the config file. Useful ones:

| Option | Meaning |
|--------|---------|
| `-f, --file` | `.mbe.ini` file describing the table and record |
| `--url` | Database URL (also `GENERAL__URL` in the environment) |
| `-r, --rows` | Rows to preload |
| `-j, --clients` | Number of client processes |
| `-R, --rate` | Total operations per second over all clients (0 = unthrottled) |
| `-d, --duration`, `-w, --warmup`, `-n, --ops` | Run length, warm-up, or a fixed op count |
| `-m, --mix` | Pattern mix, e.g. `p1=40,p2=20,...` (relative; scaled to 100) |
| `--arrival` | `fixed` or `poisson` |
| `--p5-lock` | `none`, `direct` or `readfirst` |
| `--csv` | Write per-operation results to a CSV file |
| `--no-setup`, `--no-teardown`, `--purge` | Control table creation and cleanup |

Run `mbeperf --help` for the full list.

### Patterns

| | Pattern |
|---|---------|
| p1 | Peek: read by primary key |
| p2 | Upsert: read-lock then update, or insert when the record is missing (`p2-new-pct`) |
| p3 | Generic: prefix search on a key |
| p4 | Scan: sequential read of `scan-len` records |
| p5 | Alternate-key read (optionally with a lock) |
| p7 | Append: insert at the end |

The report includes achieved rate, errors, and p50/p99 for service and total (queueing included)
time. A high count of "late" operations means the load generator could not keep to the schedule,
so latencies under it are understated.

## Backend architecture notes

Findings from reading the sources and from disassembling the shipped library:

- Enscribe record locks map to SQL transactions: the lock call runs `begin` and the unlock call
  runs `commit`, inside each read.
- Statements are prepared lazily. A lost connection or statement is recovered transparently
  (`WMBE_S_STMT_LOST`).
- Cursors fetch a page at a time. The first page is `wlib_mbe_sql_page_size` rows (default 8),
  then grows 4x up to 1024. A new page is fetched only if the previous one was full.
- The UPDATE statement sets every non-filler column, including the primary key.

## Backend changes

`my.c` differs from `my.c.orig` as follows:

- **Fewer round trips.** `begin` is `START TRANSACTION` and `commit` is `mysql_commit`, instead
  of toggling autocommit.
- **Cheaper result handling.** Bind arrays are cached per statement and rows are streamed
  without `mysql_stmt_store_result`. Per-statement hints pre-size the cursor.
- **Connection robustness.** Failed connects close and null the handle. Connect and read/write
  timeouts are set (5 s and 60 s). The client charset defaults to `latin1` (override with
  `WMBE_MYSQL_CHARSET`). The password is never logged.
- **Error mapping.** Lock wait timeout and deadlock map to `MBE_S_TIMEOUT`, duplicate key to
  `MBE_S_RECORD_EXISTS`, statement loss to `WMBE_S_STMT_LOST`, and lost connections to
  `MBE_S_IO_ERROR`.
- **Misc.** URL credentials end at the last `@` before the first `/`. The client library is
  initialised once via `pthread_once`. `max_statement_time` is built from integer milliseconds.

The new `my.c` passed a functional test under ASan/UBSan. It has **not** been built into the
real library and benchmarked with `mbeperf`. The extra `SET max_statement_time` round trips are
issued from `sql.c` and are untouched.

## Schema recommendations

See `mbe-ini.md`. In short: declare text key columns with a binary collation
(`latin1_bin` on MySQL/MariaDB), and give each alternate key an index on
`(key column, primary key column)`.
