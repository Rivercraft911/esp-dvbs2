// Copy absolute symbols, retaining choices until no future overlap can read them.
static RING_TEXT int RING_READ(ring_t *r, uint8_t *dst, size_t to, int64_t first, size_t count RING_LENGTHS)
{
    const uint32_t published = __atomic_load_n(&r->published, __ATOMIC_ACQUIRE);
    const int64_t end = first + (int64_t)count;
    if (first < 0) {
        const size_t zeros = end < 0 ? count : (size_t)-first;
        // Before symbol zero, quad code zero is phase 1, as in the legacy shaper.
        memset(dst, 0, RING_QUADS ? (zeros+3)/4 : zeros);
        if (RING_QUADS) to += zeros;
        else dst += zeros;
        count -= zeros, first = 0;
    }
    if (!count) {
        return 0;
    }
    if ((uint64_t)first < (r->release_position == r->next_position ? r->chosen_end :
                           r->start[r->release_position % RING_CHOICES])) {
        return -1;
    }
    uint32_t p = r->release_position;
    while (p < r->next_position && r->end[p % RING_CHOICES] <= (uint64_t)first) {
        ++p;
    }
    while (r->chosen_end < (uint64_t)end) {
        if (r->next_position - r->release_position >= RING_CHOICES) {
            return -1;
        }
        const unsigned i = r->next_position++ % RING_CHOICES;
        uint32_t c = RING_FILLER;
        if (r->next_frame != published) {
            c = r->next_frame++;
        } else {
            r->underruns++;
        }
        r->choice[i] = c;
        r->start[i] = r->chosen_end;
#if RING_VARIABLE
        r->chosen_end += c == RING_FILLER ? r->filler_symbols : frame_length(r, c, lengths);
#else
        r->chosen_end += c == RING_FILLER ? r->filler_symbols : r->frame_symbols;
#endif
        r->end[i] = r->chosen_end;
    }
#if RING_VARIABLE
    int64_t lead = (int64_t)r->chosen_end - end;
    for (uint32_t e = r->next_frame; e != published; ++e) lead += frame_length(r, e, lengths);
#else
    const int64_t lead = (int64_t)r->chosen_end +
                         (int64_t)(published - r->next_frame) * r->frame_symbols - end;
#endif
    if (lead < r->min_lead) {
        r->min_lead = lead;
    }
    while (count) {
        const unsigned i = p++ % RING_CHOICES;
        const uint32_t c = r->choice[i];
        const uint8_t *src = c == RING_FILLER ? r->filler :
                            r->slots[c % r->nslots];
        const size_t at = (uint64_t)first - r->start[i];
        const size_t avail = r->end[i] - (uint64_t)first;
        const size_t part = avail < count ? avail : count;
        if (RING_QUADS) {
            ring_copy_quads(dst, to, src, at, part);
        } else if (r->packed == 3) {
            ring_unpack_qpsk(dst, src, at, part);
        } else if (r->packed == 2) {
            ring_unpack_codes(dst, src, at, part);
        } else if (r->packed) {
            ring_unpack(dst, src, at, part);
        } else {
            memcpy(dst, src + at, part);
        }
        if (RING_QUADS) to += part;
        else dst += part;
        count -= part, first += part;
    }
    return 0;
}
