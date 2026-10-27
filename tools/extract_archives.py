"""Extract each pinned source archive into third_party/src/<name>, stripping the archive's top folder.

Usage: python -I extract_archives.py <downloads_dir> <src_dir> name=archive.zip [name=archive.zip ...]
"""
import os
import shutil
import sys
import zipfile


def extract(archive, dest):
    if os.path.exists(dest):
        shutil.rmtree(dest)
    os.makedirs(dest)
    with zipfile.ZipFile(archive) as z:
        names = z.namelist()
        top = names[0].split("/")[0] + "/"
        for info in z.infolist():
            if not info.filename.startswith(top):
                raise ValueError("unexpected layout: " + info.filename)
            rel = info.filename[len(top):]
            if not rel:
                continue
            target = os.path.normpath(os.path.join(dest, rel))
            if not target.startswith(os.path.normpath(dest) + os.sep):
                raise ValueError("path escapes destination: " + info.filename)
            if info.is_dir():
                os.makedirs(target, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with z.open(info) as src, open(target, "wb") as out:
                shutil.copyfileobj(src, out)
    return top


def main():
    downloads, src = sys.argv[1], sys.argv[2]
    for spec in sys.argv[3:]:
        name, archive = spec.split("=", 1)
        top = extract(os.path.join(downloads, archive), os.path.join(src, name))
        print(name, "<-", archive, "top", top)


if __name__ == "__main__":
    main()
