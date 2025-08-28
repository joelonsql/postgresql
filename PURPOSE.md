# **High-Level Specification for the `pg_dump` "split" Format**

## **1. Command and Invocation**

A new format option, named "split", shall be added to `pg_dump`. It will be invoked via the command-line flags `-F s` or `--format=split`. This format requires a directory to be specified as the output target using the `-f <directory>` option.

**Example Invocation:**
```bash
pg_dump -Fs -f my_dump_directory my_database
```

## **2. Output Structure**

The output of the command will be a directory with the following structure and contents:

1.  **Root Directory:** The directory specified by the `-f` flag will be created if it does not exist. The command will fail with an error if the specified directory exists and is not empty.

2.  **Manifest File (`toc.dat`):**
    *   A single file named `toc.dat` will be created at the top level of the output directory.
    *   This file serves as the master Table of Contents (TOC) for the dump.
    *   It will contain metadata for every dumped object, including its unique Dump ID, object type, name, owner, and dependencies.
    *   Critically, this file **must not** contain any `CREATE` statements, `COPY` statements, or other SQL definition code. It is a pure metadata manifest.
    *   For each object that has a corresponding file on disk (as described below), its entry in `toc.dat` must contain the relative path to that file.

3.  **Object Files (`.sql`):**
    *   For each dumped database object (such as a table, schema, view, function, sequence, etc.), a corresponding `.sql` file will be created within the output directory.
    *   The relative path for each file will be generated dynamically based on the object's identity, following the pattern:
        `[type]/[schema]/[name]-[hash].sql`
        *   `[type]`: The lowercased object type (e.g., `table`, `schema`, `view`).
        *   `[schema]`: The lowercased and sanitized name of the object's schema (if applicable).
        *   `[name]`: The lowercased and sanitized name of the object itself.
        *   `[hash]`: The first 32 characters (128 bits) of the SHA256 hash of the object's canonical address, as determined by `pg_identify_object_as_address`.
    *   All path segments (`type`, `schema`, `name`) must be sanitized to contain only lowercase letters, numbers, and the characters `_`, `.`, and `-`. Any other character must be replaced with an underscore `_`.
    *   The directory hierarchy (e.g., `table/public/`) must be created automatically.

## **3. Content of Object Files**

The content of each `.sql` file will be as follows:

1.  **Schema-Only Objects (e.g., a View):** The file will contain the complete `CREATE VIEW ...` statement.
2.  **Table Objects:** The file will contain the complete `CREATE TABLE ...` statement, followed immediately by the `COPY table_name (...) FROM stdin;` statement, followed by the table's data in `COPY` format, and finally the `\.` terminator.
3.  **Data-Only Dumps (`-a`):** The `.sql` files for tables will contain only the `COPY` statement and the data. No `CREATE TABLE` statement will be present.
4.  **Schema-Only Dumps (`-s`):** The `.sql` files for tables will contain only the `CREATE TABLE` statement. No `COPY` statement or data will be present.

## **4. Parallelism (`-j` flag)**

The format implementation **must** be fully compatible with parallel dumping (`-j N`). The creation and writing of all individual `.sql` object files must be capable of being performed concurrently by multiple worker processes without conflict or race conditions. The creation of the `toc.dat` manifest may be performed serially by the main process before the workers are dispatched.

## **5. `pg_restore` Compatibility**

The generated dump directory, including the `toc.dat` manifest and the individual `.sql` files, must be a valid archive that can be fully understood and restored by `pg_restore`. `pg_restore` should be able to use the `toc.dat` manifest to identify objects and their dependencies, and then read the corresponding `.sql` files to perform the restore. This includes support for parallel restore (`pg_restore -j N`).

## **6. Building and Testing**

Use meson and ninja.
The build dir is in the `build` directory.
Use `ninja install` to install.
The install dir is the `pgsql` directory.
After installing, test using `./pgsql/bin/pg_dump -Fs -f dumps/test`
