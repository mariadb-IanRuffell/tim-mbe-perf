# `.mbe.ini` files

`wlib-mbe-sql` emulates Enscribe on top of a SQL database. A program opens an "Enscribe file"
with the usual `FILE_OPEN_` (or `MBE_FILE_OPEN_`) and the library finds the table behind it
through a small configuration file that describes it: the database, the table, and the layout of
the record (the C structure that the program reads and writes) in terms of columns.

This document describes the format of that file. The behaviour of the API on top of it is that of
Enscribe: see `include/wlib/usrlib.h` and `include/wlib/cextdecs.h`.

## How a file is found

* The name passed to `FILE_OPEN_` is used as it is, as a path (relative to the working directory
  of the process, or absolute). It is not translated and there is no search path.
* A name is handled by this library if it **ends in `.mbe.ini`** (the comparison ignores case).
  Other names go to the other file backends of wlib.
* The name can have up to 255 characters
* At most 256 files can be open at the same time in a process.

## Syntax

A standard INI file (GLib `GKeyFile`): `[sections]`, `key = value` lines, and comments that start
with `#` or `;`. The names of sections and keys are case sensitive and the keys must be in
lowercase.

Every key can be overridden by an environment variable, which takes precedence over the file. Its
name is `SECTION__KEY`: the section, two underscores and the key, in uppercase and with every
character that is not a letter or a digit replaced by `_`. For example `general/url` is
`GENERAL__URL` and `field-2/size` is `FIELD_2__SIZE`. This is meant for the URL, which is
different in every environment, but note that it applies to *every* `.mbe.ini` file of the
process.

## Sections

### `[general]`

| key | required | |
|-----|----------|---|
| `url` | yes | The database. See [URLs](#urls) |
| `table` | yes | Name of the table |
| `fields` | yes | Number of fields of the record (an integer greater than zero). There must be this many `[field-N]` sections |

`url` and `table` can have up to 255 characters.

### `[field-N]`

One section for each field, numbered from 1 to `fields` **in the order of the record**: `field-1`
is the first one in memory. A field that is missing or incomplete makes the open fail.

| key | required | |
|-----|----------|---|
| `name` | yes | Name of the column (for `filler` fields it is just a label). Up to 255 characters |
| `type` | yes | One of the [types](#types) (the case is ignored) |
| `size` | yes | Size in bytes of the field in the record |
| `key_spec` | no | Makes the field a key. See [Keys](#keys) |
| `align` | no | Alignment of the field in the record. See [Layout](#layout-of-the-record) |
| `unique` | no | `1` if the values of this alternate key are unique (the column has a `UNIQUE` index). See [Keys](#keys) |

## Types

| type | in the record | in the database | key | generic |
|------|---------------|-----------------|-----|---------|
| `int` | unsigned integer of `size` 1, 2, 4 or 8 bytes, in the byte order of the machine | decimal text, as an unsigned number (`%llu`) | yes | no |
| `float` | `float` (`size` 4) or `double` (`size` 8) | decimal text with enough digits to read the same value back | yes | no |
| `string` | `size` bytes of text, padded with zeroes (`NUL`). The text ends at the first zero, or after `size` bytes | text without the padding | yes | yes |
| `rstring` | `size` bytes: the text written **backwards**, followed by blanks up to `size` (some INS applications store keys this way) | the text in the normal order, without the blanks | yes | no |
| `binary` | `size` raw bytes | uppercase hexadecimal text, 2 characters per byte | yes | yes |
| `filler` | `size` bytes that are not stored | nothing: there is no column | no | no |

Notes:

* `int` values are handled as unsigned. A column that holds them has to be able to represent the
  whole range (an 8 byte value above 2<sup>63</sup>-1 does not fit in a signed `BIGINT`).
* A `filler` is read as zeroes and ignored when written. Use it for reserved bytes, or for any
  part of the record that the database does not need. It does not count as one of the `fields` columns
  of the table.
* **Generic** is whether the field can be used in a generic search (`FILE_SETKEY_` with the
  generic positioning mode), which the library runs as `LIKE 'prefix%'`. For `string` the prefix is
  the text up to the first `NUL`, with `%`, `_` and `!` taken literally; for `binary`, the bytes
  that are given.
* The text form of a value (including the hexadecimal one) must fit in 1023 characters, so a
  `string` or `rstring` can have up to 1023 bytes, and a `binary` up to 511.

## Keys

A field is a key if it has `key_spec`:

* `key_spec = 0` is the **primary key**. Exactly one field must have it. It has to identify the
  record uniquely (it is the primary key of the table), and the records are written, updated and
  deleted through it.
* `key_spec = 1, 2, ...` are the **alternate keys**. They need not be unique: records with the same
  value are returned ordered by primary key, as Enscribe does.
* An alternate key whose values really are unique should say so with `unique = 1` (it is not
  allowed on the primary key). The library then does not ask the database for the extra ordering by
  primary key, and MariaDB can stop reading after one page; without it, each page of a scan on a
  `UNIQUE` index makes the server read and sort the whole remaining range. **Only set it if the
  column is unique**: with duplicates, records can be skipped or repeated between pages.
* The numbers are used as the key specifier of `FILE_SETKEY_`. Each can be used once, and it must
  be lower than `fields`; they need not be consecutive.
* Only the types that are marked as keys in the table above can be keys.

Other rules:

* The length of the table name plus the length of the name of each key column must not exceed
  **40** characters. Only key columns count.
* `FILE_SETKEY_` must be given a key no longer than the field. The generic positioning mode only
  works on `string` and `binary` keys.

## Layout of the record

The offset of each field is computed by the library, as a C compiler would: the fields are placed
one after the other, in the order of the sections, and a field is moved forward until its offset is
a multiple of its alignment. `record_size` is the offset at which the last field ends (there is no
padding at the end).

The default alignment is, for `int`, `float` and `filler`, the size of the field; for `string`,
`rstring` and `binary` there is none (1). It is never larger than the pointer-size of the
platform: **8 on 64 bit** builds and **4 on 32 bit** ones, so the layout of a record with an 8
byte integer after an odd-sized field is not the same on the two (it is the same as the one of a
C `struct` on each of them). The `align` key sets it explicitly: `align = 0` disables it for the
field (it goes right after the previous one), and any other value is used as it is (but capped
at that maximum).

The structure that the program uses must have exactly this layout. If it is not obvious, a good
way to be sure is to put the largest fields first, or to add `filler` fields for the padding.

## URLs

The scheme chooses the database driver. The drivers are included when the library is built (see
`ENABLE_POSTGRES` and `ENABLE_MYSQL`); any other URL makes the open fail.

* **`postgresql:`** A libpq connection URI, passed as it is to `PQconnectdb`:
  `postgresql://user:password@host:5432/dbname`. Any parameter that libpq understands can be
  added (`?sslmode=require`, `?connect_timeout=5`...).
* **`mysql:`** `mysql://user:password@host:3306/dbname`. The user, the password and the port
  can be left out (the port is 3306); the database cannot. There are no parameters, and
  special characters in the password are not decoded.

## Schema of the table

The library does not create the table: it has to exist, with a column for each field that is not
a `filler`, with the name of the field and a type that can hold the text form of the values.
Suggested types:

| field | PostgreSQL | MySQL |
|-------|-----------|-------|
| `int` of 1, 2, 4 bytes | `BIGINT` | `BIGINT` |
| `int` of 8 bytes | `NUMERIC(20)`, or `BIGINT` if the values are less than 2<sup>63</sup> | `BIGINT UNSIGNED` |
| `float` | `DOUBLE PRECISION` | `DOUBLE` |
| `string`, `rstring` | `VARCHAR(size)` | `VARCHAR(size)` |
| `binary` | `VARCHAR(2 * size)` | `VARCHAR(2 * size)` |

Columns should be `NOT NULL`: every column is given a value when a record is inserted. In addition:

* The primary key column is the primary key of the table.
* Enscribe compares strings byte by byte, and the order of the records, the approximate positioning
  and the generic one (`LIKE 'prefix%'`) have to follow that. Declare the text key columns with
  a binary collation: `COLLATE "C"` on PostgreSQL (otherwise the `LIKE` is not served by the index
  and the order is the one of the locale), `CHARACTER SET latin1 COLLATE latin1_bin` or similar on
  MySQL. Hexadecimal keys (`binary`) are compared as uppercase text, and are fine with any
  collation that orders digits before letters.
* Every alternate key needs an index on **(key column, primary key column)**, in that order. Records
  are read a page at a time in that order, and without this index the server sorts the
  matches for each page.
* Table names are never quoted. Column names are quoted with backticks on MySQL, and **not** on
  PostgreSQL, where they must be valid unquoted identifiers (and are folded to lowercase).

## Related system configuration

Two keys in the `[mbe]` section of the system file (`wlib.ini`, see `doc/wlib.ini`) apply to all
the files: `retry_timeout` (1000 ms) and `retry_random` (100 ms): when the connection is lost and
can't be restored at once, the library waits this long (plus a random time up to `retry_random`)
before trying again.

## Errors

If the file can't be used `FILE_OPEN_` fails (the details are in the log, with the `L_FEAT_DATA`
feature). The usual causes are: a missing `url`, `table` or `fields`; a field that is missing or
has a type that does not exist; no field with `key_spec = 0`, a repeated or too high `key_spec`,
or a key on a type that can't be one; table and key names too long; a value type too large; a URL
with an unknown scheme; the database not reachable; or a table or columns that do not exist (the
library checks that the statements are valid when it opens the file).

## Example

A file for a subscribers table: the primary key is a 30 byte binary key, and there is an alternate
key, the owner.

`subscribers.mbe.ini`:

```ini
# Subscribers: key = 14 digit IMEI + 16 digit IMSI. The owner is an alternate key.
#
# The URL can be replaced for the environment with GENERAL__URL.

[general]
url    = postgresql://eir:secret@db.example.com:5432/eir
table  = subscribers
fields = 6

# Primary key (key_spec 0): 30 raw bytes, stored as 60 hex digits
[field-1]
name     = rkey
type     = binary
size     = 30
key_spec = 0

[field-2]
name = flags
type = int
size = 1

# Reserved bytes: no column. align = 0 puts it right after "flags"
[field-3]
name  = reserved
type  = filler
size  = 5
align = 0

[field-4]
name = last_update
type = int
size = 4

# Alternate key (key_spec 1): text, zero padded
[field-5]
name     = owner
type     = string
size     = 16
key_spec = 1

[field-6]
name = info
type = string
size = 32
```

The record that goes with it (88 bytes), in C:

```c
struct subscriber {
    char     rkey[30];          /* offset  0 */
    uint8_t  flags;             /* offset 30 */
    char     reserved[5];       /* offset 31 */
    uint32_t last_update;       /* offset 36, 4 byte aligned: no padding */
    char     owner[16];         /* offset 40 */
    char     info[32];          /* offset 56 */
};                              /* 88 bytes */
```

and the table, for PostgreSQL:

```sql
CREATE TABLE subscribers (
    rkey        VARCHAR(60) COLLATE "C" NOT NULL PRIMARY KEY,
    flags       BIGINT      NOT NULL DEFAULT 0,
    last_update BIGINT      NOT NULL DEFAULT 0,
    owner       VARCHAR(16) COLLATE "C" NOT NULL DEFAULT '',
    info        VARCHAR(32) NOT NULL DEFAULT ''
);
CREATE INDEX subscribers_owner ON subscribers (owner, rkey);
```

The program then uses it as it would use an Enscribe file:

```c
short fd;
struct subscriber rec;
unsigned short count;
char name[] = "subscribers.mbe.ini";

MBE_FILE_OPEN_(name, strlen(name), &fd);

/* Exact search on the primary key */
MBE_FILE_SETKEY_(fd, key, 30, 0, MBE_POS_EXACT, 0);
MBE_READX(fd, (char *) &rec, sizeof(rec), &count);

/* All the records of an owner, by the alternate key: generic positioning, key specifier 1 */
MBE_FILE_SETKEY_(fd, "mario", 5, 1, MBE_POS_GENERIC, 0);
while (MBE_READX(fd, (char *) &rec, sizeof(rec), &count) == 0) {
    /* ... */
}
```

More examples are in `test/wlib-mbe-sql/data/` (fillers at different positions, name length
limits) and in `test/wlib-mbe-sql/perf/` (the fixtures of the performance tool).
