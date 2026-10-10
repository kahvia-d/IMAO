"""Project the shipped feature+index payload under a different detector.

Reads two measurements that already exist rather than re-running anything:

  * ``measurements/sweep-sift-per-tile.json`` - keypoint counts and encoded
    feature sizes for one detector configuration, produced per 1024px tile so
    the numbers match what tools/KuroMapFeatureBuilder would ship.
  * each pack's ``visual-index.imx`` header - the exact byte size of every
    section of the shipped visual index.

The index is not one blob that scales with the keypoint count. Its five
sections behave differently, and only two of them follow the features:

    vocabulary   4096 words x 128 columns x float32 = 2.00 MB, identical in
                 every shard, and independent of how many keypoints exist
    tile table   per index tile
    histogram    a sparse TF-IDF vector per index tile; independent of the
                 keypoint count
    featureRow   one row per feature occurrence
    posting      one posting per (word, feature) occurrence

So the honest projection has to hold vocabulary/tile/histogram fixed and scale
only featureRow/posting. Reporting a single "index scales with keypoints" ratio
overstates the win by roughly a factor of two.
"""

import glob
import json
import os
import struct
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PACK_ROOT = os.path.join(REPO_ROOT, 'Assets', 'FeaturesDatas', 'KuroTilePacks')
SWEEP = os.path.join(REPO_ROOT, 'measurements', 'sweep-sift-per-tile.json')

# MapVisualIndexHeader: magic(8) + 12 uint32 + 5 uint64 + 3 x 32-byte hashes.
HEADER_FIELDS = ('version', 'headerLength', 'endianMarker', 'wordCount', 'descriptorColumns',
                 'tileSize', 'tileStride', 'featureCount', 'tileCount', 'histogramEntryCount',
                 'featureRowCount', 'postingCount')
PAYLOAD_FIELDS = ('vocabularyPayloadLength', 'tilePayloadLength', 'histogramPayloadLength',
                  'featureRowPayloadLength', 'postingPayloadLength')


def read_index_sections(path):
    with open(path, 'rb') as handle:
        header = handle.read(192)
    if header[:8] != b'IMAOIX01':
        raise ValueError('%s is not an IMAOIX01 index' % path)
    values = dict(zip(HEADER_FIELDS, struct.unpack_from('<12I', header, 8)))
    lengths = dict(zip(PAYLOAD_FIELDS, struct.unpack_from('<5Q', header, 56)))
    values['vocabularySha256'] = header[128:160].hex()
    values['fileBytes'] = os.path.getsize(path)
    values.update(lengths)
    return values


def load_sweep():
    if not os.path.isfile(SWEEP):
        raise SystemExit('sweep summary is missing: %s\nrun measurements/accuracy-check/'
                         'sweep_per_tile.ps1 first' % SWEEP)
    with open(SWEEP, encoding='utf-8') as handle:
        return json.load(handle)


def main():
    sweep = load_sweep()
    measured = {entry['pack']: entry for entry in sweep['packs']}

    rows = []
    for directory in sorted(glob.glob(os.path.join(PACK_ROOT, '*'))):
        if not os.path.isdir(directory):
            continue
        name = os.path.basename(directory)
        manifest_path = os.path.join(directory, 'features.imf.manifest.json')
        index_path = os.path.join(directory, 'visual-index.imx')
        if not (os.path.isfile(manifest_path) and os.path.isfile(index_path)):
            continue
        with open(manifest_path, encoding='utf-8') as handle:
            manifest = json.load(handle)
        index = read_index_sections(index_path)
        shipped_kp = int(manifest['keypointCount'])
        shipped_feature_bytes = os.path.getsize(os.path.join(directory, 'features.imf'))
        entry = measured.get(name)
        row = {
            'pack': name,
            'shippedKeypoints': shipped_kp,
            'shippedFeatureBytes': shipped_feature_bytes,
            'indexFileBytes': index['fileBytes'],
            'vocabularyBytes': index['vocabularyPayloadLength'],
            'tileBytes': index['tilePayloadLength'],
            'histogramBytes': index['histogramPayloadLength'],
            'featureRowBytes': index['featureRowPayloadLength'],
            'postingBytes': index['postingPayloadLength'],
            'vocabularySha256': index['vocabularySha256'],
            'tileCount': index['tileCount'],
        }
        if entry is not None:
            detected = int(entry['detectorKeypoints'])
            row.update({
                'detectedKeypoints': detected,
                'ratio': detected / max(shipped_kp, 1),
                'projectedFeatureBytes': int(entry['quantizedZlibBytes']),
                'verified': int(entry['verified']),
                'queryCount': int(entry['queryCount']),
            })
            scaling = row['ratio']
            row['projectedIndexBytes'] = int(
                index['vocabularyPayloadLength'] + index['tilePayloadLength']
                + index['histogramPayloadLength']
                + (index['featureRowPayloadLength'] + index['postingPayloadLength']) * scaling)
        rows.append(row)

    total = lambda key: sum(r.get(key, 0) or 0 for r in rows)
    mb = lambda value: value / 1048576.0

    print('== shipped (measured, all %d packs)' % len(rows))
    print('   features          %8.2f MB' % mb(total('shippedFeatureBytes')))
    print('   index             %8.2f MB' % mb(total('indexFileBytes')))
    print('     vocabulary      %8.2f MB  (%d copies of %.2f MB, %d distinct hash%s)' % (
        mb(total('vocabularyBytes')), len(rows),
        mb(total('vocabularyBytes')) / max(len(rows), 1),
        len({r['vocabularySha256'] for r in rows}),
        '' if len({r['vocabularySha256'] for r in rows}) == 1 else 'es'))
    print('     tile table      %8.2f MB' % mb(total('tileBytes')))
    print('     histogram       %8.2f MB  (%d index tiles)' % (
        mb(total('histogramBytes')), total('tileCount')))
    print('     featureRow      %8.2f MB' % mb(total('featureRowBytes')))
    print('     posting         %8.2f MB' % mb(total('postingBytes')))
    print('   TOTAL             %8.2f MB' % mb(total('shippedFeatureBytes') + total('indexFileBytes')))

    projected = [r for r in rows if 'projectedFeatureBytes' in r]
    if not projected:
        print('\nno pack has a detector measurement in %s' % SWEEP)
        return 1
    missing = sorted(set(r['pack'] for r in rows) - set(r['pack'] for r in projected))

    def sub(collection, key):
        return sum(r.get(key, 0) or 0 for r in collection)

    scaling_row = sub(projected, 'featureRowBytes') + sub(projected, 'postingBytes')
    fixed = (sub(projected, 'vocabularyBytes') + sub(projected, 'tileBytes')
             + sub(projected, 'histogramBytes'))
    projected_index = sub(projected, 'projectedIndexBytes')
    projected_features = sub(projected, 'projectedFeatureBytes')
    measured_shipped_features = sub(projected, 'shippedFeatureBytes')
    measured_shipped_index = sub(projected, 'indexFileBytes')
    kp_ratio = sub(projected, 'detectedKeypoints') / max(sub(projected, 'shippedKeypoints'), 1)

    print('\n== projected under %s (%d packs, per-tile detection)' % (
        sweep.get('config', '?'), len(projected)))
    if missing:
        print('   NOT projected (no measurement): %s' % ', '.join(missing))
    print('   keypoints         %8d -> %8d  (%.1f%%)' % (
        sub(projected, 'shippedKeypoints'), sub(projected, 'detectedKeypoints'), 100 * kp_ratio))
    print('   features          %8.2f MB -> %7.2f MB   (%.1fx)' % (
        mb(measured_shipped_features), mb(projected_features),
        measured_shipped_features / max(projected_features, 1)))
    print('   index             %8.2f MB -> %7.2f MB   (%.1fx)' % (
        mb(measured_shipped_index), mb(projected_index),
        measured_shipped_index / max(projected_index, 1)))
    print('     fixed part      %8.2f MB  (vocabulary + tiles + histograms, unchanged)' % mb(fixed))
    print('     scaled part     %8.2f MB -> %7.2f MB  (featureRow + posting)' % (
        mb(scaling_row), mb(scaling_row * kp_ratio)))
    print('   TOTAL             %8.2f MB -> %7.2f MB   (%.1fx)' % (
        mb(measured_shipped_features + measured_shipped_index),
        mb(projected_features + projected_index),
        (measured_shipped_features + measured_shipped_index)
        / max(projected_features + projected_index, 1)))

    deduped = projected_index - sub(projected, 'vocabularyBytes') + (
        sub(projected, 'vocabularyBytes') / max(len(projected), 1))
    print('\n== the same projection, with the vocabulary shipped once instead of %d times'
          % len(projected))
    print('   index             %8.2f MB -> %7.2f MB' % (mb(projected_index), mb(deduped)))
    print('   TOTAL             %8.2f MB -> %7.2f MB   (%.1fx vs shipped)' % (
        mb(measured_shipped_features + measured_shipped_index),
        mb(projected_features + deduped),
        (measured_shipped_features + measured_shipped_index) / max(projected_features + deduped, 1)))
    print('   vocabulary saving %8.2f MB, with no detector change at all' % (
        mb(sub(projected, 'vocabularyBytes') - sub(projected, 'vocabularyBytes') / max(len(projected), 1))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
