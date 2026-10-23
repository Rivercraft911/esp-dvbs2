// Six-symbol, four-sample pulse. Two triples, with one aligned vector per part.
#include "shaper.h"
#include "shaper_taps.h"

void shaper_short_build(shaper_table_t *t, uint32_t *blocks, unsigned peak)
{
    // Inverse-hold RRC, alpha .35. Current coverage: tests/test_stream.py.
    // Independent pulse derivation: archive/dev-2026-10-17:tests/test_shaper_short.py.
    // Recovery and invocation: docs/how-it-works.md#table-provenance.
    static const int32_t pulse[24] = {
        -1785,138,3694,5601,2264,-6452,-15014,-14236,2790,34634,70116,93505,
        93505,70116,34634,2790,-14236,-15014,-6452,2264,5601,3694,138,-1785
    };
    int32_t taps[24];
    // Use the same gain as the legacy stream, so peak retains its envelope meaning.
    for (int i = 0; i < 24; ++i) taps[i] = (int32_t)((double)pulse[i] * peak / 718.4);
    // Build each part directly. The table sum is offset binary, as in shaper_build.
    *t = (shaper_table_t){.blocks=blocks, .n=4, .block_words=8, .bits=3, .parts=2};
    const int32_t *level[] = {shaper_level_i, shaper_level_q};
    int bias[2][4];
    for (int axis = 0; axis < 2; ++axis) {
        for (int p = 0; p < 4; ++p) {
            int low = 0;
            for (int w = 0; w < 512; ++w) {
                int64_t sum = 0;
                for (int a = 0; a < 3; ++a)
                    sum += (int64_t)level[axis][w >> (3*a) & 7] * taps[(3+a)*4+p];
                int v = (int)((sum + ((int64_t)1 << 37)) >> 38);
                if (v < low) low = v;
            }
            bias[axis][p] = -low;
        }
    }
    for (int w = 0; w < 512; ++w)
        for (int part = 0; part < 2; ++part)
            for (int p = 0; p < 4; ++p) {
                unsigned field[2];
                for (int axis = 0; axis < 2; ++axis) {
                    int64_t sum = 0;
                    for (int a = 0; a < 3; ++a)
                        sum += (int64_t)level[axis][w >> (3*a) & 7] * taps[(3*part+a)*4+p];
                    int v = (int)((sum + ((int64_t)1 << 37)) >> 38);
                    field[axis] = v + (part ? bias[axis][p] : 512-bias[axis][p]);
                }
                blocks[w*8+part*4+p] = field[0] | field[1] << 10;
            }
}
