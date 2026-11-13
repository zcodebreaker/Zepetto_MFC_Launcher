"""Publish the game catalog and its ZIPs. Optionally activate the release in Version.ini."""
import argparse
from contextlib import contextmanager
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
import hashlib
import io
import os
from pathlib import Path, PureWindowsPath
import re
import struct
import tempfile
from zipfile import BadZipFile, ZipFile, ZIP_DEFLATED


def version_text(high, low):
    number = str(high << 32 | low)
    if len(number) != 14:
        raise ValueError("Versão inválida no catálogo")
    return datetime.strptime(number, "%Y%m%d%H%M%S").strftime("%Y-%m-%d_%H-%M-%S")


def catalog_data(blob):
    if not 6 <= len(blob) <= 64 << 20:
        raise ValueError("Tamanho de catálogo inválido")
    plain = bytes(((blob[i - 1] << 4) | (b >> 4)) & 255 for i, b in enumerate(blob))
    fmt = int.from_bytes(plain[:4], "little")
    if fmt not in range(1, 5) or plain[-1] or b"\0" in plain[4:-1]:
        raise ValueError("Formato de catálogo inválido")
    text = plain[4:-1].decode("cp1252")
    tokens = iter(re.findall(r'"[^"]*"|[^",;\s\b]+|"', text))

    def word():
        token = next(tokens)
        if not token.isdecimal() or int(token) > 0xffffffff:
            raise ValueError("Campo numérico inválido")
        return int(token)

    def quoted():
        token = next(tokens)
        if len(token) < 2 or not token.startswith('"') or not token.endswith('"'):
            raise ValueError("Caminho inválido")
        return token[1:-1]

    entries = []
    seen = set()
    try:
        if next(tokens) != "*Header":
            raise ValueError("Cabeçalho inválido")
        version = version_text(word(), word())
        for tag in tokens:
            if tag != "*FileInfo":
                raise ValueError("Registro desconhecido no catálogo")
            name, folder = quoted(), PureWindowsPath(quoted())
            path = folder / name
            if (not name or PureWindowsPath(name).name != name or path.anchor or path in seen or
                    any(part == ".." or part.endswith((" ", ".")) or PureWindowsPath(part).is_reserved() or
                        any(c in '<>:"|?*' or ord(c) < 32 for c in part) for part in path.parts)):
                raise ValueError(f"Caminho inválido ou duplicado: {path}")
            seen.add(path)
            tag = version_text(word(), word())
            size = word()
            digest = struct.pack("<4I", *reversed([word() for _ in range(4)]))
            for _ in range((4 if fmt >= 2 else 0) + (fmt >= 3) + (fmt >= 4)):
                word()
            entries.append((Path(path), tag, size, digest))
    except StopIteration as err:
        raise ValueError("Catálogo incompleto") from err
    if not entries:
        raise ValueError("Catálogo sem arquivos")
    return version, entries


def read_catalog(path):
    return catalog_data(path.read_bytes())


@contextmanager
def temporary_file(folder):
    with tempfile.NamedTemporaryFile(dir=folder, prefix=".publish-", delete=False) as handle:
        path = Path(handle.name)
    try:
        yield path
    finally:
        path.unlink(missing_ok=True)


def checked(root, relative):
    if relative.anchor or ".." in relative.parts:
        raise ValueError(f"Caminho fora da distribuição: {relative}")
    path = root
    for part in relative.parts:
        path = path / part
        if path.is_symlink() or path.is_junction():
            raise ValueError(f"Link no caminho da distribuição: {path}")
    return path


def verified_zip(path, member, size, digest):
    with ZipFile(path) as archive:
        infos = archive.infolist()
        if len(infos) != 1 or infos[0].filename != member or infos[0].file_size != size:
            return False
        with archive.open(infos[0]) as stream:
            return hashlib.file_digest(stream, "md5").digest() == digest


def publish_zip(target, member, size, digest, open_source, staging):
    if target.is_file():
        try:
            if verified_zip(target, member, size, digest):
                return
        except (BadZipFile, EOFError):
            pass
    with temporary_file(staging) as temp:
        with open_source() as stream, ZipFile(temp, "w", ZIP_DEFLATED, compresslevel=1) as archive:
            with archive.open(member, "w", force_zip64=size >= (1 << 31) - 1) as out:
                md5 = hashlib.md5()
                total = 0
                while chunk := stream.read(1024 * 1024):
                    total += len(chunk)
                    md5.update(chunk)
                    out.write(chunk)
        if total != size or md5.digest() != digest:
            raise ValueError(f"Conteúdo de {member} diverge do catálogo")
        if not verified_zip(temp, member, size, digest):
            raise ValueError(f"Não foi possível validar {target.name}")
        target.parent.mkdir(parents=True, exist_ok=True)
        os.replace(temp, target)


def publish(catalog, game, output, ini):
    blob = catalog.read_bytes()
    version, entries = catalog_data(blob)
    game, output = game.resolve(strict=True), output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if ini:
        initial = ini.read_bytes()
        lines = initial.splitlines(keepends=True)
        if len(lines) < 3 or not lines[0].strip().isdigit() or not lines[2].startswith((b"http://", b"https://")):
            raise ValueError(f"Version.ini inválido: {ini}")
        line = lines[1].rstrip(b"\r\n")
        datetime.strptime(line.decode("ascii"), "%Y-%m-%d_%H-%M-%S")
        lines[1] = version.encode("ascii") + lines[1][len(line):]
    with tempfile.TemporaryDirectory(prefix="pb-publish-", dir=output.parent) as tmp:
        staging = Path(tmp)

        def one(entry):
            rel, stamp, size, digest = entry
            target = checked(output, Path(stamp) / (str(rel) + ".zip"))
            source = checked(game, rel)
            publish_zip(target, rel.name, size, digest, lambda: source.open("rb"), staging)

        with ThreadPoolExecutor(max_workers=6) as pool:
            for count, _ in enumerate(pool.map(one, entries), 1):
                if count % 500 == 0:
                    print(f"{count}/{len(entries)} pacotes verificados", flush=True)
        if catalog.read_bytes() != blob:
            raise ValueError("O catálogo mudou durante a publicação")
        name = version + "_FileList.dat"
        publish_zip(output / (name + ".zip"), name, len(blob), hashlib.md5(blob).digest(),
                    lambda: io.BytesIO(blob), staging)
        if ini:
            if ini.read_bytes() != initial:
                raise ValueError("Version.ini foi editado durante a publicação")
            with temporary_file(ini.parent) as temp:
                temp.write_bytes(b"".join(lines))
                os.replace(temp, ini)
    print(f"{version}: {len(entries)} pacotes e {name}.zip verificados.", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("catalog", type=Path)
    parser.add_argument("game", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--version-ini", type=Path, help="Atualiza a segunda linha após verificar os pacotes.")
    args = parser.parse_args()
    publish(args.catalog, args.game, args.output, args.version_ini)
