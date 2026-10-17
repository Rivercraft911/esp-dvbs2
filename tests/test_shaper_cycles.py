"""Instruction model interlocks are assumptions from the TRM, never timing measurements."""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'host'))
import shaper_cycles as c


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
    # An expanded long LOOP ends immediately before RETW. Its final stores
    # execute on every iteration, not once after the loop.
    long_loop = list(enumerate(('entry a1, 32','wsr.lbeg a4','addi a4, a4, 1',
                               'addi a3, a3, 1','ee.vst.128.ip q0, a2, 16',
                               'ee.vst.128.ip q1, a2, 16','retw.n')))
    assert c.replay(c.loop_stream(long_loop,2))['cycles']-c.replay(c.loop_stream(long_loop,1))['cycles'] == 3
    try:
        c.operands('unknown a2, a3')
    except ValueError:
        pass
    else:
        raise AssertionError('unsupported instructions must not silently use one cycle')
    # Alignment padding between two macro-built entry points is dropped, nothing else.
    import tempfile
    with tempfile.NamedTemporaryFile('w', suffix='.dis') as f:
        f.write('00000000 <k>:\n   0:\tentry\ta1, 32\n   3:\tretw.n\n   5:\till\n\n'
                '00000008 <k_raw>:\n   8:\tentry\ta1, 32\n')
        f.flush()
        assert c.functions(f.name) == {'k': [(0,'entry\ta1, 32'),(3,'retw.n')],
                                       'k_raw': [(8,'entry\ta1, 32')]}
    print('pipeline: scalar/vector and fused hazards, branch penalty, unknown-op rejection, padding')


if __name__ == '__main__':
    test_interlocks()
