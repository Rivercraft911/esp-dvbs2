"""Check the issue model's hazards and physical bank/encoder budgets, no hardware."""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'host'))
import shaper_cycles as c
import shaper_budget as b


def test_interlocks():
    # A loaded scalar or vector is unavailable to the immediately following E-stage use.
    assert c.replay([('l8ui a8, a3, 0',False),('addx8 a6, a6, a8',False)])['cycles'] == 3
    assert c.replay([('ee.vld.128.ip q1, a12, 16',False),
                     ('ee.vadds.s32 q0, q0, q1',False)])['cycles'] == 3
    assert c.replay([('ee.vld.128.ip q1, a12, 16',False),('addi a3, a3, 1',False),
                     ('ee.vadds.s32 q0, q0, q1',False)])['cycles'] == 3
    # Fused load/add defines the loaded q2 at M, its sum at E; the distinction matters.
    u,d = c.operands('ee.vadds.s32.ld.incp q2, a10, q0, q0, q1')
    assert ('q2',2) in d and ('q0',1) in d
    assert c.replay([('bnez a4, 20',True)])['cycles'] == 3
    try:
        c.operands('unknown a2, a3')
    except ValueError:
        pass
    else:
        raise AssertionError('unsupported instructions must not silently use one cycle')
    print('pipeline: scalar/vector and fused hazards, branch penalty, unknown-op rejection')


def test_geometry():
    assert b.STRIDE == 14268 and b.DEADLINE == 42804
    assert b.BANK%4 == b.STRIDE%4 == 0
    assert b.STRIDE%20 != 0  # n5 cannot silently inherit the old schedule.
    stride = b.BANK-60-2040
    assert stride%20 == 0 and b.BANK%20 == 0 and stride*3 == 42840
    # Exhaust every wrap offset reached by each candidate geometry.
    for n,step,group,lead in ((4,b.STRIDE,4,64),(5,stride,20,60)):
        for bank in range(b.BANK):
            start = bank*step-lead if bank else 0
            offset = start%b.BANK
            assert offset%group == 0 and (b.BANK-offset)%group == 0
            # Both pieces are complete symbols and together represent the full bank.
            assert (b.BANK-offset)//n + offset//n == b.BANK//n
    print('geometry: every physical bank wrap remains symbol/group aligned; 80 MS/s deadlines exact')


def test_encoder_budget():
    # Normal 8PSK with pilots is independently derived from 64800 FEC bits and Table 11.
    slots = (64800//3)//90
    symbols = 90 + slots*90 + ((slots-1)//16)*36
    assert symbols == 22194
    available16, available20 = [b.CPU*symbols/r for r in (16e6,20e6)]
    assert available16 == 332910 and available20 == 266328
    assert 313000 < available16 and 313000 > available20
    assert 367000 > available16
    p = b.scenario('test',37576,4,copy_bytes=4103)
    assert p['slack_cycles'] < 0
    p = b.scenario('test',25275,4)
    assert p['stress_slack_cycles'] > 0
    print('encoder: measured 5/6 GCS fits 16 alone, misses 20; TS misses both; copy cost retained')


if __name__ == '__main__':
    test_interlocks()
    test_geometry()
    test_encoder_budget()
