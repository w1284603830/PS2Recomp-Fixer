"""Independent byte/Unicode expectations from Python standard codecs."""
from pathlib import Path
import sys

PROFILES = (
    ('utf8', 'utf-8', 'Utf8', '存档', '表'),
    ('sjis', 'shift_jis', 'Windows932', '記録', '表'),
    ('gbk', 'gbk', 'Windows936', '存档', '乗'),
    ('big5', 'big5', 'Windows950', '存檔', '兝'),
)


def cpp_bytes(value):
    return 'bytes({' + ','.join(str(byte) for byte in value) + '})'


def prepare(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    rows = []
    for label, codec, enum, stem, trail in PROFILES:
        leaf, trail_leaf = stem + '.bin', trail + '.bin'
        raw, raw_trail = leaf.encode(codec), trail_leaf.encode(codec)
        assert raw.decode(codec) == leaf and raw_trail.decode(codec) == trail_leaf
        if label != 'utf8':
            assert raw_trail[1] == 0x5c
        values = [cpp_bytes(v) for v in (raw, leaf.encode(), raw_trail, trail_leaf.encode())]
        rows.append('{"' + label + '",port_paths::Encoding::' + enum + ',' + ','.join(values) + '}')
    (directory / 'encoding_fixtures.inc').write_text(
        'static const Profile profiles[] = {\n' + ',\n'.join(rows) + '\n};\n', encoding='ascii')


if __name__ == '__main__':
    prepare(sys.argv[1])
