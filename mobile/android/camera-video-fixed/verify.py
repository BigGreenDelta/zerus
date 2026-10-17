#!/usr/bin/env python3
"""Compare original JVM APIs, generated classes and CameraX consumer metadata."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import zipfile


class ClassReader:
    def __init__(self, data):
        if data[:4] != b'\xca\xfe\xba\xbe':
            raise ValueError('Invalid class file')
        self.data = data
        self.position = 8
        self.major = struct.unpack_from('>H', data, 6)[0]
        self.pool = [None] * self.u2()
        index = 1
        while index < len(self.pool):
            tag = data[self.position]
            self.position += 1
            if tag == 1:
                size = self.u2()
                self.pool[index] = self.take(size).decode('utf-8', errors='replace')
            elif tag in (3, 4):
                self.pool[index] = {'number': self.take(4).hex()}
            elif tag in (5, 6):
                self.pool[index] = {'number': self.take(8).hex()}
                index += 1
            elif tag in (7, 8, 16, 19, 20):
                self.pool[index] = self.u2()
            elif tag in (9, 10, 11, 12, 17, 18):
                self.take(4)
            elif tag == 15:
                self.take(3)
            else:
                raise ValueError(f'Unknown class constant: {tag}')
            index += 1

    def take(self, count):
        value = self.data[self.position:self.position + count]
        if len(value) != count:
            raise ValueError('Truncated class file')
        self.position += count
        return value

    def u2(self):
        return struct.unpack('>H', self.take(2))[0]

    def u4(self):
        return struct.unpack('>I', self.take(4))[0]

    def class_name(self, index):
        return self.pool[self.pool[index]] if index else None

    def attributes(self):
        result = {}
        for _ in range(self.u2()):
            name = self.pool[self.u2()]
            data = self.take(self.u4())
            if name == 'Signature':
                result[name] = self.pool[struct.unpack('>H', data)[0]]
            elif name == 'ConstantValue':
                value = self.pool[struct.unpack('>H', data)[0]]
                result[name] = self.pool[value] if isinstance(value, int) else value
            elif name == 'Exceptions':
                count = struct.unpack_from('>H', data)[0]
                result[name] = [self.class_name(struct.unpack_from('>H', data, 2 + 2 * index)[0])
                                for index in range(count)]
        return result

    def abi(self, *, include_private=False):
        access = self.u2()
        self.u2()  # This class's own name is already the archive entry name.
        parent = self.class_name(self.u2())
        interfaces = [self.class_name(self.u2()) for _ in range(self.u2())]
        members = []
        for kind in ('field', 'method'):
            for _ in range(self.u2()):
                flags = self.u2()
                name, descriptor = self.pool[self.u2()], self.pool[self.u2()]
                attributes = self.attributes()
                # Include package-private/internal APIs and generated AutoValue members too.
                # Synthetic Kotlin/default and package-local bridges are caller ABI too.
                if include_private or not flags & 0x0002:
                    members.append((kind, name, descriptor, flags, attributes))
        return {'java_major': self.major, 'access': access, 'parent': parent,
                'interfaces': interfaces, 'members': sorted(members, key=lambda x: x[:4]),
                'attributes': self.attributes()}


def contents(path, *, include_private=False):
    with zipfile.ZipFile(path) as aar:
        outer = {name: aar.read(name) for name in aar.namelist() if name != 'classes.jar'}
        bytecode = aar.read('classes.jar')
    with zipfile.ZipFile(io.BytesIO(bytecode)) as classes:
        if len(classes.namelist()) != len(set(classes.namelist())):
            raise ValueError('Duplicate class JAR entries')
        names = classes.namelist()
        abis = {name: ClassReader(classes.read(name)).abi(include_private=include_private)
                for name in names if name.endswith('.class')}
        metadata = {name: classes.read(name) for name in names if not name.endswith(('.class', '/'))}
        recorder = classes.read('androidx/camera/video/Recorder.class')
    return outer, bytecode, abis, metadata, recorder


def verify(original, rebuilt, report):
    original_outer, original_jar, before, original_metadata, _ = contents(original)
    rebuilt_outer, rebuilt_jar, after, rebuilt_metadata, recorder = contents(rebuilt)
    missing = sorted(before.keys() - after.keys())
    extra = sorted(after.keys() - before.keys())
    # Java 8 javac adds exactly these two package-local bridges for the new private completer.
    # Keep every original synthetic member as well as ordinary public/internal APIs.
    permitted_bridges = [
        ('method', 'access$1900', '(Landroidx/camera/video/Recorder;)Landroidx/concurrent/futures/CallbackToFutureAdapter$Completer;', 0x1008, {}),
        ('method', 'access$1902', '(Landroidx/camera/video/Recorder;Landroidx/concurrent/futures/CallbackToFutureAdapter$Completer;)Landroidx/concurrent/futures/CallbackToFutureAdapter$Completer;', 0x1008, {}),
    ]
    changes, bridges = [], []
    for name in sorted(before.keys() & after.keys()):
        rebuilt_abi = dict(after[name])
        if name == 'androidx/camera/video/Recorder.class':
            bridges = [item for item in rebuilt_abi['members'] if item in permitted_bridges]
            rebuilt_abi['members'] = [item for item in rebuilt_abi['members'] if item not in permitted_bridges]
        if before[name] != rebuilt_abi:
            changes.append(name)
    permitted_annotations = {
        'androidx/camera/video/FallbackStrategy$FallbackRule.class',
        'androidx/camera/video/Recorder$VideoCapabilitiesSource.class',
        'androidx/camera/video/Recorder$VideoRecordingType.class',
    }
    # javac 17 emits these enum-switch holders for the same pinned Java 8 sources;
    # javac 21 does not. Permit their exact declarations, never arbitrary synthetic classes.
    switch_fields = {
        'androidx/camera/video/Recorder$8.class': (
            '$SwitchMap$androidx$camera$video$Recorder$AudioState',
            '$SwitchMap$androidx$camera$video$Recorder$State'),
        'androidx/camera/video/VideoEncoderSession$2.class': (
            '$SwitchMap$androidx$camera$video$VideoEncoderSession$VideoEncoderState',),
        'androidx/camera/video/internal/audio/AudioSource$3.class': (
            '$SwitchMap$androidx$camera$video$internal$audio$AudioSource$InternalState',),
        'androidx/camera/video/internal/encoder/EncoderImpl$4.class': (
            '$SwitchMap$androidx$camera$video$internal$encoder$EncoderImpl$InternalState',),
    }
    full_abis = contents(rebuilt, include_private=True)[2]
    permitted_switches = []
    for name in extra:
        if name not in switch_fields:
            continue
        expected = {
            'java_major': 52, 'access': 0x1020, 'parent': 'java/lang/Object',
            'interfaces': [], 'attributes': {},
            'members': [('field', field, '[I', 0x1018, {}) for field in switch_fields[name]]
                       + [('method', '<clinit>', '()V', 0x0008, {})],
        }
        if full_abis[name] == expected:
            permitted_switches.append(name)
    unexpected_classes = sorted(set(extra) - permitted_annotations - set(permitted_switches))
    consumer_changes = [name for name in original_outer if original_outer[name] != rebuilt_outer.get(name)]
    metadata_changes = [name for name in original_metadata if original_metadata[name] != rebuilt_metadata.get(name)]
    notice = 'META-INF/androidx/camera/camera-video/ZERUS_BACKPORT_NOTICE.txt'
    fields_present = all(name in recorder for name in (
        b'mVideoEncodingFuture', b'mAudioEncodingFuture', b'mVideoEncoderCompleter', b'mInProgressVideoEncoder'))
    result = {
        'schema': 1, 'original_classes': len(before), 'rebuilt_classes': len(after),
        'removed_classes': missing, 'additional_source_annotations': sorted(set(extra) & permitted_annotations),
        'additional_synthetic_enum_switch_classes': permitted_switches,
        'unexpected_additional_classes': unexpected_classes, 'changed_nonprivate_abis': changes,
        'additional_synthetic_recorder_bridges': bridges,
        'changed_consumer_entries': consumer_changes, 'changed_jar_metadata': metadata_changes,
        'backport_notice_present': notice in rebuilt_metadata, 'backport_owner_fields_present': fields_present,
        'autovalue_class_names': sorted(name for name in before if '/AutoValue_' in name),
        'original_classes_sha256': hashlib.sha256(original_jar).hexdigest(),
        'rebuilt_classes_sha256': hashlib.sha256(rebuilt_jar).hexdigest(),
        'original_aar_sha256': hashlib.sha256(Path(original).read_bytes()).hexdigest(),
        'rebuilt_aar_sha256': hashlib.sha256(Path(rebuilt).read_bytes()).hexdigest(),
    }
    result['passed'] = not (missing or changes or consumer_changes or metadata_changes or
                            unexpected_classes) and notice in rebuilt_metadata and fields_present
    Path(report).parent.mkdir(parents=True, exist_ok=True)
    Path(report).write_text(json.dumps(result, indent=2) + '\n')
    if not result['passed']:
        raise ValueError(f'CameraX compatibility check failed: {result}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('original', 'rebuilt', 'report'):
        parser.add_argument('--' + name, required=True)
    verify(**vars(parser.parse_args()))
