# zip

Zip archives and zlib/deflate/gzip compression for Clay, built on
[miniz](https://github.com/richgel999/miniz). It reads and writes `.zip` archives, from
files or in memory, and compresses Strings in the zlib, raw deflate and gzip formats. A
native brick: part of it is C, built with your C compiler the first time it is imported.

```clay norun
import("zip")

// compress and decompress Strings
packed = Zlib gzip(File read("report.csv"))
File write("report.csv.gz", packed)
Zlib gunzip(packed) length println

// write an archive
Zip create("site.zip") { |z|
    z add("hello.txt", "Hello!")
    z addDir("public", "site/")
}

// read it back
Zip open("site.zip") { |z|
    z names println
    z read("hello.txt") println      // Hello!
    z extractAll("out")
}
```

## Install

```sh
clay bricks add github.com/mseminatore/clay-zip
```

`clay bricks add` tells people that a brick runs C code and asks before going ahead.

## Zlib: compression

Clay Strings hold bytes, so compressed data is a String, and `File read` and `File write`
are binary-safe.

| Method | What |
|---|---|
| `Zlib compress(data, level = 6)` | zlib format (RFC 1950): what most "zlib" APIs mean |
| `Zlib decompress(data, max = nil)` | and back |
| `Zlib deflate(data, level = 6)` | raw deflate (RFC 1951), with no header or checksum |
| `Zlib inflate(data, max = nil)` | and back |
| `Zlib gzip(data, level = 6)` | gzip (RFC 1952), as in `.gz` files and `Content-Encoding: gzip` |
| `Zlib gunzip(data, max = nil)` | and back; reads concatenated members as one |
| `Zlib crc32(data, crc = 0)` | CRC-32; pass an earlier result to continue it |
| `Zlib adler32(data, adler = 1)` | Adler-32; likewise |

`level` is 0 (store, no compression) to 9 (smallest), or 10, miniz's slowest and
smallest. `max` limits the decompressed size in bytes: past it the method raises
`ZipError` instead of using up memory on a decompression bomb. With `nil`, there is no
limit. Corrupt or truncated data raises `ZipError`, and so does data after the end of a
zlib or deflate stream.

## Zip: archives

### Reading

| Method | What |
|---|---|
| `Zip open(file)` | opens the archive in a file |
| `Zip fromBytes(data)` | opens an archive held in a String (copied) |
| `z count` | the number of entries |
| `z names` | their names, in archive order |
| `z entries` | a Dict for each entry (see below) |
| `z entry(name)` | one entry's Dict, or `nil` |
| `z has(name)` | whether there is an entry called `name` |
| `z read(name)` | an entry's contents, as a String (`name` may also be an index) |
| `z extract(name, file)` | writes an entry to `file`, making its directory; returns `file` |
| `z extractAll(dir)` | writes every entry under `dir`; returns the paths of the files written |
| `z close` | closes the archive |

An entry's Dict has `name`, `size`, `compressedSize`, `isDir`, `modified` (Unix
seconds), `crc32`, `encrypted`, `comment` and `index`. Names are matched exactly,
including case.

`extractAll` checks every name before it writes anything. A name that is absolute
(`/etc/x`, `C:\x`) or has a `..` part would land outside `dir` (the "zip slip" attack),
so `extractAll` raises `ZipError` instead.

### Writing

| Method | What |
|---|---|
| `Zip create(file)` | starts a new archive in `file` |
| `Zip create` | starts a new archive in memory |
| `z add(name, data, level = 6)` | adds an entry holding `data` (a String) |
| `z addFile(file, name = nil, level = 6)` | adds a file from disk (by default under its own name), keeping its modification time |
| `z addDir(dir, prefix = "", level = 6)` | adds everything under `dir`, including empty directories, with names starting with `prefix` |
| `z close` | finishes the archive; returns its file, or its bytes if it was built in memory |
| `z bytes` | the same as `close` for an archive built in memory |

`add`, `addFile` and `addDir` return the archive, so calls chain. Entry names use `/`
between directories, and a name ending in `/` is a directory. `add` refuses unsafe names
(absolute, or with `..`) with `ArgumentError`. Archives over 4 GB, or with over 65535
entries, are written as zip64.

A file archive that is never closed is finished when its last reference goes away, but
call `close` (or use a block) so you know when it's done.

### Blocks

Give `open`, `fromBytes` or `create` a block and the archive is closed when the block
ends, even if the block raises an error. `open` and `fromBytes` then return the block's
value, and `create` returns what `close` does:

```clay norun
text = Zip open("site.zip") { |z| z read("hello.txt") }
bytes = Zip create { |z| z add("a.txt", "a") add("b.txt", "b") }
```

### Errors

Failures raise `ZipError`: a file that can't be opened or isn't a zip archive, a missing
entry, corrupt data, an unsafe name in `extractAll`, or an archive used after `close` or
in the wrong mode (reading one that is being written). Arguments of the wrong type raise
`TypeError`, and bad values (a level of 11, an unsafe entry name for `add`) raise
`ArgumentError`.

### Limits

- Encrypted entries can be listed but not read: miniz doesn't decrypt.
- Only the deflate and stored methods are supported (not bzip2, LZMA or zstd), which
  covers what nearly every zip tool writes.
- `read` and `Zlib` work on whole Strings in memory. `extract`, `addFile` and the file
  archives themselves stream through miniz, so large entries don't need to fit in memory
  there.

## Layout

| File | What |
|---|---|
| `brick.json` | the manifest; `native` lists the C sources and flags |
| `native/zip.c` | the plugin: registers `ZipCore` (entry point `zip_plugin_init`), glue to miniz |
| `native/miniz/` | miniz, vendored (see below) |
| `src/main.clay` | the Clay API, `Zip` and `Zlib`, wrapping `ZipCore` |
| `tests/test_zip.clay` | tests, including data made by other implementations |

The brick defines three top-level names: `Zip`, `Zlib` and `ZipCore`.

`.github/workflows/test.yml` runs the tests on Linux, macOS and Windows for every push and
pull request on GitHub.

## Develop

```sh
clay tests/test_zip.clay       # builds the plugin on first use; a brick imports itself by name
clay --compile -o t tests/test_zip.clay && ./t     # the same tests, compiled
```

The plugin is cached in `build/` (not committed); delete it to force a rebuild. To
debug, add `"-g", "-O0"` to `cflags` in `brick.json` and run under `lldb` or `gdb`:
`lldb -- clay tests/test_zip.clay`. To see warnings:

```sh
cc -std=c99 -Wall -Wextra -fsyntax-only -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES \
   -I<clay>/src/runtime -Inative/miniz native/zip.c
```

## Third-party code

`native/miniz/` holds `miniz.c`, `miniz.h` and `LICENSE` from the amalgamated release of
[miniz](https://github.com/richgel999/miniz), MIT licensed. `native/miniz/VERSION`
records the release. To upgrade, download `miniz-<version>.zip` from a newer release,
copy those three files over, and update `VERSION`. miniz is compiled with
`MINIZ_NO_ZLIB_COMPATIBLE_NAMES`, so its zlib-style names (`deflate`, `crc32`, ...) are
not defined as macros. Its own functions all start with `mz_` or `tdefl_`/`tinfl_`, so
they don't clash with a system zlib.

## License

MIT (see `LICENSE`).
