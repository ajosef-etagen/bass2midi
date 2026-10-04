#!/usr/bin/env python3
"""Regenerates the song-file test fixtures in this directory.

- synthetic_5.00.gp5 / synthetic_5.10.gp5: written with PyGuitarPro (pip install pyguitarpro, LGPL),
  used here only as an external file writer and reference parser. Its expected notes are dumped to
  synthetic_gp5_expected.csv (track,start_quarters,midi_note,tie) from PyGuitarPro's own parse.
- minimal.gp: a hand-written GPIF score in a deflated ZIP, exercising rhythms, dots, tuplets, ties
  and tempo automation.

The C++ tests compare bass2midi_song's readers against these files.
"""
import random
import zipfile

import guitarpro as gp


def build_gp5(seed):
    random.seed(seed)
    song = gp.Song()
    song.title = 'Synthetic'
    song.artist = 'Bass2MIDI tests'
    song.tracks = []
    song.measureHeaders = []
    for i in range(8):
        h = gp.MeasureHeader(number=i + 1)
        if i == 3:
            h.timeSignature = gp.TimeSignature(numerator=3, denominator=gp.Duration(value=4))
        if i == 5:
            h.timeSignature = gp.TimeSignature(numerator=7, denominator=gp.Duration(value=8))
        if i == 2:
            h.marker = gp.Marker(title='Verse', color=gp.Color(255, 0, 0))
        if i == 4:
            h.isRepeatOpen = True
        if i == 6:
            h.repeatClose = 2
        song.measureHeaders.append(h)
    specs = [([43, 38, 33, 28], False, 'Bass', 34), ([64, 59, 55, 50, 45, 40], False, 'Guitar', 29),
             ([0] * 6, True, 'Drums', 0)]
    for ti, (tuning, percussion, name, program) in enumerate(specs):
        track = gp.Track(song, number=ti + 1, name=name)
        track.strings = [gp.GuitarString(i + 1, v) for i, v in enumerate(tuning)]
        track.isPercussionTrack = percussion
        track.channel = gp.MidiChannel(channel=9 if percussion else ti, effectChannel=ti, instrument=program)
        track.measures = []
        for h in song.measureHeaders:
            m = gp.Measure(track, h)
            for vi, v in enumerate(m.voices):
                if vi == 1 and random.random() < 0.6:
                    continue
                total = h.timeSignature.numerator * (4 / h.timeSignature.denominator.value) * 960
                pos = 0
                prev_strings = set()
                while pos < total - 1:
                    d = gp.Duration(value=random.choice([2, 4, 8, 16]))
                    if random.random() < 0.2:
                        d.isDotted = True
                    if random.random() < 0.15:
                        d.tuplet = gp.Tuplet(3, 2)
                    if pos + d.time > total:
                        d = gp.Duration(value=16)
                    if pos + d.time > total:
                        break
                    b = gp.Beat(v, duration=d, status=gp.BeatStatus.normal)
                    if random.random() < 0.1:
                        b.status = gp.BeatStatus.rest
                    if random.random() < 0.1:
                        b.text = 'text'
                    if random.random() < 0.08:
                        chord = gp.Chord(len(track.strings))
                        chord.name = 'Am'
                        chord.firstFret = 1
                        chord.strings = [0] * len(track.strings)
                        b.effect.chord = chord
                    if random.random() < 0.08:
                        b.effect.mixTableChange = gp.MixTableChange(
                            tempo=gp.MixTableItem(random.randint(60, 200), 0),
                            volume=gp.MixTableItem(100, 2) if random.random() < 0.5 else None, tempoName='')
                    if random.random() < 0.05:
                        b.effect.stroke = gp.BeatStroke(gp.BeatStrokeDirection.down, 8)
                    if random.random() < 0.05:
                        b.effect.slapEffect = gp.SlapEffect.popping
                    if random.random() < 0.05:
                        b.effect.pickStroke = gp.BeatStrokeDirection.up
                    if random.random() < 0.04:
                        b.effect.tremoloBar = gp.BendEffect(type=gp.BendType.dip, value=50,
                                                            points=[gp.BendPoint(0, 0), gp.BendPoint(6, -2), gp.BendPoint(12, 0)])
                    if b.status != gp.BeatStatus.rest:
                        strings = random.sample(range(1, len(track.strings) + 1), random.randint(1, min(3, len(track.strings))))
                        for sn in sorted(strings):
                            n = gp.Note(b, value=random.randint(0, 15), string=sn, type=gp.NoteType.normal)
                            r = random.random()
                            if r < 0.08 and sn in prev_strings:
                                n.type = gp.NoteType.tie
                            elif r < 0.12:
                                n.type = gp.NoteType.dead
                            e = n.effect
                            if random.random() < 0.05:
                                e.bend = gp.BendEffect(type=gp.BendType.bend, value=100,
                                                       points=[gp.BendPoint(0, 0), gp.BendPoint(6, 4), gp.BendPoint(12, 4)])
                            if random.random() < 0.05:
                                e.grace = gp.GraceEffect(fret=3)
                            if random.random() < 0.05:
                                e.slides = [gp.SlideType.shiftSlideTo]
                            if random.random() < 0.05:
                                e.harmonic = random.choice([gp.NaturalHarmonic(), gp.ArtificialHarmonic(gp.PitchClass(7), gp.Octave.ottava),
                                                            gp.TappedHarmonic(12), gp.PinchHarmonic(), gp.SemiHarmonic()])
                            if random.random() < 0.05:
                                e.trill = gp.TrillEffect(fret=2, duration=gp.Duration(16))
                            if random.random() < 0.05:
                                e.tremoloPicking = gp.TremoloPickingEffect(gp.Duration(16))
                            if random.random() < 0.05:
                                n.durationPercent = 0.5
                            if random.random() < 0.05:
                                e.leftHandFinger = gp.Fingering.index
                                e.rightHandFinger = gp.Fingering.middle
                            if random.random() < 0.1:
                                n.velocity = gp.Velocities.forte
                            b.notes.append(n)
                    v.beats.append(b)
                    prev_strings = (set(n.string for n in b.notes if n.type != gp.NoteType.dead)
                                    if b.status != gp.BeatStatus.rest else set())
                    pos += d.time
            track.measures.append(m)
        song.tracks.append(track)
    return song


def expected_notes(path):
    song = gp.parse(path)
    rows = []
    for ti, track in enumerate(song.tracks):
        for m in track.measures:
            for v in m.voices:
                for b in v.beats:
                    for n in b.notes:
                        if n.type == gp.NoteType.dead or b.status == gp.BeatStatus.rest or not 0 < n.realValue <= 127:
                            continue
                        rows.append((ti, round((b.start - 960) / 960, 4), n.realValue, int(n.type == gp.NoteType.tie)))
    return sorted(rows)


MINIMAL_GPIF = """<?xml version="1.0" encoding="utf-8"?>
<GPIF>
  <Score><Title><![CDATA[Minimal & test]]></Title><Artist><![CDATA[Bass2MIDI]]></Artist></Score>
  <MasterTrack>
    <Tracks>0 1</Tracks>
    <Automations>
      <Automation><Type>Tempo</Type><Bar>0</Bar><Position>0</Position><Value>100 2</Value></Automation>
      <Automation><Type>Tempo</Type><Bar>1</Bar><Position>0</Position><Value>132 2</Value></Automation>
    </Automations>
  </MasterTrack>
  <Tracks>
    <Track id="0"><Name><![CDATA[Guitar]]></Name><InstrumentSet><Type>electricGuitar</Type></InstrumentSet>
      <Staves><Staff><Properties><Property name="Tuning"><Pitches>40 45 50 55 59 64</Pitches></Property></Properties></Staff></Staves></Track>
    <Track id="1"><Name><![CDATA[Bass]]></Name><InstrumentSet><Type>electricBass</Type></InstrumentSet>
      <Staves><Staff><Properties><Property name="Tuning"><Pitches>28 33 38 43</Pitches></Property></Properties></Staff></Staves></Track>
  </Tracks>
  <MasterBars>
    <MasterBar><Time>4/4</Time><Bars>0 1</Bars></MasterBar>
    <MasterBar><Time>3/4</Time><Bars>2 3</Bars></MasterBar>
  </MasterBars>
  <Bars>
    <Bar id="0"><Voices>0 -1 -1 -1</Voices></Bar>
    <Bar id="1"><Voices>1 -1 -1 -1</Voices></Bar>
    <Bar id="2"><Voices>-1 -1 -1 -1</Voices></Bar>
    <Bar id="3"><Voices>2 -1 -1 -1</Voices></Bar>
  </Bars>
  <Voices>
    <Voice id="0"><Beats>0</Beats></Voice>
    <Voice id="1"><Beats>1 2 3 4</Beats></Voice>
    <Voice id="2"><Beats>5 6 7 8 9</Beats></Voice>
  </Voices>
  <Beats>
    <Beat id="0"><Rhythm ref="0"/><Notes>0</Notes></Beat>
    <Beat id="1"><Rhythm ref="1"/><Notes>1</Notes></Beat>
    <Beat id="2"><Rhythm ref="2"/><Notes>2</Notes></Beat>
    <Beat id="3"><Rhythm ref="3"/><Notes>3</Notes></Beat>
    <Beat id="4"><Rhythm ref="3"/></Beat>
    <Beat id="5"><Rhythm ref="4"/><Notes>4</Notes></Beat>
    <Beat id="6"><Rhythm ref="4"/><Notes>5</Notes></Beat>
    <Beat id="7"><Rhythm ref="4"/><Notes>6</Notes></Beat>
    <Beat id="8"><Rhythm ref="3"/><Notes>7</Notes></Beat>
    <Beat id="9"><Rhythm ref="3"/><Notes>8</Notes></Beat>
  </Beats>
  <Notes>
    <Note id="0"><Properties><Property name="String"><String>0</String></Property><Property name="Fret"><Fret>3</Fret></Property></Properties></Note>
    <Note id="1"><Properties><Property name="String"><String>0</String></Property><Property name="Fret"><Fret>0</Fret></Property></Properties></Note>
    <Note id="2"><Tie origin="true" destination="false"/><Properties><Property name="String"><String>1</String></Property><Property name="Fret"><Fret>2</Fret></Property></Properties></Note>
    <Note id="3"><Tie origin="false" destination="true"/><Properties><Property name="String"><String>1</String></Property><Property name="Fret"><Fret>2</Fret></Property></Properties></Note>
    <Note id="4"><Properties><Property name="String"><String>2</String></Property><Property name="Fret"><Fret>0</Fret></Property></Properties></Note>
    <Note id="5"><Properties><Property name="String"><String>2</String></Property><Property name="Fret"><Fret>2</Fret></Property></Properties></Note>
    <Note id="6"><Properties><Property name="String"><String>3</String></Property><Property name="Fret"><Fret>0</Fret></Property></Properties></Note>
    <Note id="7"><Properties><Property name="String"><String>3</String></Property><Property name="Fret"><Fret>12</Fret></Property></Properties></Note>
    <Note id="8"><Properties><Property name="Midi"><Number>36</Number></Property></Properties></Note>
  </Notes>
  <Rhythms>
    <Rhythm id="0"><NoteValue>Whole</NoteValue></Rhythm>
    <Rhythm id="1"><NoteValue>Quarter</NoteValue><AugmentationDot count="1"/></Rhythm>
    <Rhythm id="2"><NoteValue>Eighth</NoteValue></Rhythm>
    <Rhythm id="3"><NoteValue>Quarter</NoteValue></Rhythm>
    <Rhythm id="4"><NoteValue>Eighth</NoteValue><PrimaryTuplet num="3" den="2"/></Rhythm>
  </Rhythms>
</GPIF>
"""


def main():
    with open('synthetic_gp5_expected.csv', 'w') as out:
        out.write('version,track,start_quarters,midi_note,tie\n')
        for version, name in [((5, 0, 0), '5.00'), ((5, 1, 0), '5.10')]:
            path = f'synthetic_{name}.gp5'
            gp.write(build_gp5(7), path, version=version)
            for row in expected_notes(path):
                out.write(f'{name},{row[0]},{row[1]:.4f},{row[2]},{row[3]}\n')
    with zipfile.ZipFile('minimal.gp', 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('VERSION', '7.0')
        z.writestr('Content/score.gpif', MINIMAL_GPIF)


if __name__ == '__main__':
    main()
