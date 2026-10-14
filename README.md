# Mini Filesystem Builder — CSE321

Two C programs for creating a small filesystem image and adding a host file to its root directory. The project makes filesystem layout tangible: a superblock, allocation bitmaps, an inode table, and data blocks are written into a regular image file.

## Programs and files

| File | Purpose |
| --- | --- |
| [mkfs_builder.c](mkfs_builder.c) | Create an image and initialize the root directory |
| [mkfs_adder.c](mkfs_adder.c) | Copy a host file into an existing image and write a new image |
| `file_9.txt`, `file_15.txt`, `file_22.txt`, `file_26.txt` | Small sample host files |
| `test.img` | Included example image |

The repository also includes the assignment PDF and prebuilt executables.

## Build

Use a C compiler in a POSIX-style environment, such as Linux or WSL. From the repository root:

```bash
gcc -std=c11 -Wall -Wextra mkfs_builder.c -o mkfs_builder
gcc -std=c11 -Wall -Wextra mkfs_adder.c -o mkfs_adder
```

Rebuilding avoids relying on the bundled executables' original environment.

## Create an image and add a file

Choose unused output filenames; the programs open output images for writing and can replace existing files.

```bash
./mkfs_builder --image demo-empty.img --size-kib 200 --inodes 128
./mkfs_adder --input demo-empty.img --output demo-with-file.img --file file_9.txt
```

The first command creates a 200 KiB image. The second reads that image, adds the sample file to its root directory, and writes the result separately.

| Option | Constraint |
| --- | --- |
| Builder `--size-kib` | 180–4096 KiB, in multiples of 4 |
| Builder `--inodes` | 128–512 inodes |
| Builder `--seed` | Optional integer; timestamps still use the current time |
| Adder `--input` / `--output` | Different path strings are required |
| Adder `--file` | Nonempty host file fitting within 12 direct blocks |

## Image layout

The format uses 4096-byte blocks and 128-byte inodes. Block 0 holds the superblock, blocks 1 and 2 hold the allocation bitmaps, and the inode table starts at block 3. The data region follows the inode table. Root inode 1 initially contains `.` and `..` directory entries.

The adder supports files up to 49,152 bytes, subject to available blocks and directory slots. It adds files to the root directory; there is no general directory-management interface.

This is a custom educational image format. The repository does not include an operating-system mount driver. Build commands and image operations have not been executed as part of this documentation update.
