#!/usr/bin/env python3
"""
Organize images into subfolders of 100.

Usage:
    python organize_images.py "/path/to/Documents/beehive"

What it does:
    - Looks at all image files directly inside the target folder
    - Sorts them (by filename by default)
    - Moves the first 100 into a subfolder named "1"
    - The next 100 into "2", and so on

Run with --dry-run first to see what WOULD happen without moving anything:
    python organize_images.py "/path/to/Documents/beehive" --dry-run

By default it sorts by filename. To sort by date modified instead, add --by-date:
    python organize_images.py "/path/to/Documents/beehive" --by-date
"""

import argparse
import shutil
import sys
from pathlib import Path

IMAGE_EXTENSIONS = {
    ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".tif", ".tiff",
    ".heic", ".heif", ".webp", ".raw", ".cr2", ".nef", ".arw"
}

BATCH_SIZE = 100


def main():
    parser = argparse.ArgumentParser(description="Organize images into numbered subfolders of 100.")
    parser.add_argument("folder", help="Path to the folder containing the images")
    parser.add_argument("--dry-run", action="store_true", help="Show what would happen without moving files")
    parser.add_argument("--by-date", action="store_true", help="Sort by date modified instead of filename")
    parser.add_argument("--batch-size", type=int, default=BATCH_SIZE, help="Number of images per subfolder (default 100)")
    args = parser.parse_args()

    folder = Path(args.folder).expanduser().resolve()

    if not folder.is_dir():
        print(f"Error: '{folder}' is not a valid folder.")
        sys.exit(1)

    # Only look at files directly in this folder (not in subfolders already)
    images = [
        f for f in folder.iterdir()
        if f.is_file() and f.suffix.lower() in IMAGE_EXTENSIONS
    ]

    if not images:
        print(f"No image files found directly inside '{folder}'.")
        sys.exit(0)

    if args.by_date:
        images.sort(key=lambda f: f.stat().st_mtime)
    else:
        images.sort(key=lambda f: f.name.lower())

    print(f"Found {len(images)} images in '{folder}'.")
    print(f"Will create subfolders of {args.batch_size} images each.")
    if args.dry_run:
        print("--- DRY RUN: no files will actually be moved ---\n")

    batch_num = 1
    for i in range(0, len(images), args.batch_size):
        batch = images[i:i + args.batch_size]
        subfolder = folder / str(batch_num)

        print(f"Folder '{batch_num}': {len(batch)} images "
              f"({batch[0].name} ... {batch[-1].name})")

        if not args.dry_run:
            subfolder.mkdir(exist_ok=True)
            for img in batch:
                dest = subfolder / img.name
                # avoid overwriting if a name collision somehow occurs
                if dest.exists():
                    print(f"  ! Skipping {img.name}, already exists in {subfolder}")
                    continue
                shutil.move(str(img), str(dest))

        batch_num += 1

    if args.dry_run:
        print("\nDry run complete. Re-run without --dry-run to actually move the files.")
    else:
        print("\nDone! Images have been moved into numbered subfolders.")


if __name__ == "__main__":
    main()
